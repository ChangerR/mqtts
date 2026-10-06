// Disposable, loopback-only load fixture. It is not part of the service image.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"flag"
	"fmt"
	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	"github.com/ChangerR/mqtts/modules/authz/internal/policy"
	"github.com/ChangerR/mqtts/modules/authz/internal/server"
	"google.golang.org/grpc"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
	"log"
	"net"
	"net/http"
	"os"
	"sync/atomic"
	"time"
)

func main() {
	directory := flag.String("directory", "", "temporary data directory")
	pairs := flag.Int("pairs", 32, "fixture pairs")
	ttl := flag.Uint("cache-ttl-ms", 60000, "positive cache freshness")
	flag.Parse()
	if *directory == "" || *pairs < 1 || *pairs > 512 || *ttl > 300000 {
		log.Fatal("invalid fixture arguments")
	}
	store, err := policy.Open(*directory+"/fixture.db", 2048, 8*1024*1024)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	hash := sha256.Sum256([]byte("test-password"))
	now := uint64(time.Now().UnixMilli())
	for i := 0; i < *pairs; i++ {
		topic := fmt.Sprintf("fixture/pair-%d", i)
		var rows []*pb.Session
		for _, kind := range []string{"reader", "writer"} {
			name := fmt.Sprintf("%s-%d", kind, i)
			action := pb.Action_SUBSCRIBE
			var payload *pb.PayloadPolicy
			if kind == "writer" {
				action = pb.Action_PUBLISH
				payload = &pb.PayloadPolicy{Bindings: []*pb.JsonBinding{{Paths: []string{"/actor"}, EqualsString: name, RequiredAny: true}}}
			}
			rows = append(rows, &pb.Session{Username: name, ClientId: name, PasswordSha256: hash[:], Namespace: "fixture", Enabled: true, ExpiresAtMs: now + 300000, PolicyValidUntilMs: now + 300000, Permissions: []*pb.Permission{{Action: action, TopicFilter: topic, PayloadPolicy: payload}, {Action: action, TopicFilter: "fixture/fanout", PayloadPolicy: payload}}})
		}
		if _, _, err = store.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: rows}); err != nil {
			log.Fatal(err)
		}
	}
	service, err := server.New(store, os.Getenv("AUTHZ_QUERY_TOKEN"), os.Getenv("AUTHZ_ADMIN_TOKEN"), uint32(*ttl), 128)
	if err != nil {
		log.Fatal(err)
	}
	var batches, items, maximum, connects, active, peak, delay atomic.Int64
	interceptor := func(ctx context.Context, req any, info *grpc.UnaryServerInfo, next grpc.UnaryHandler) (any, error) {
		if batch, ok := req.(*pb.BatchAuthorizeRequest); ok {
			batches.Add(1)
			size := int64(len(batch.Requests))
			items.Add(size)
			for old := maximum.Load(); size > old; old = maximum.Load() {
				if maximum.CompareAndSwap(old, size) {
					break
				}
			}
		}
		if _, ok := req.(*pb.AuthenticateRequest); ok {
			connects.Add(1)
		}
		return service.Intercept(ctx, req, info, func(ctx context.Context, req any) (any, error) {
			n := active.Add(1)
			defer active.Add(-1)
			for old := peak.Load(); n > old; old = peak.Load() {
				if peak.CompareAndSwap(old, n) {
					break
				}
			}
			if _, ok := req.(*pb.BatchAuthorizeRequest); ok && delay.Load() > 0 {
				timer := time.NewTimer(time.Duration(delay.Load()) * time.Millisecond)
				defer timer.Stop()
				select {
				case <-ctx.Done():
					return nil, status.FromContextError(ctx.Err()).Err()
				case <-timer.C:
				}
			}
			return next(ctx, req)
		})
	}
	rpc := grpc.NewServer(grpc.UnaryInterceptor(interceptor), grpc.MaxRecvMsgSize(policy.MaxRPCBytes), grpc.MaxSendMsgSize(policy.MaxRPCBytes), grpc.MaxConcurrentStreams(128))
	pb.RegisterAuthorizationServer(rpc, service)
	pb.RegisterAdministrationServer(rpc, service)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}
	metrics, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}
	go http.Serve(metrics, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Test-only fault injection, bound to an ephemeral loopback port. This
		// executable is never included in the authorization service image.
		if r.Method == "POST" && r.URL.Path == "/control" {
			var input struct {
				DelayMS int64  `json:"delay_ms"`
				Revoke  string `json:"revoke"`
			}
			if json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&input) != nil || input.DelayMS < 0 || input.DelayMS > 2000 {
				http.Error(w, "invalid control", 400)
				return
			}
			delay.Store(input.DelayMS)
			if input.Revoke != "" {
				version, _ := store.Versions()
				rows, _ := store.Snapshot([]string{input.Revoke})
				if rows[0] == nil {
					http.Error(w, "unknown session", 400)
					return
				}
				row := proto.Clone(rows[0]).(*pb.Session)
				row.Enabled = false
				if _, _, err := store.Apply(&pb.ApplyRequest{ExpectedVersion: version, Upserts: []*pb.Session{row}}); err != nil {
					http.Error(w, "revoke failed", 500)
					return
				}
			}
		} else if r.Method != "GET" || r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		json.NewEncoder(w).Encode(map[string]int64{"batches": batches.Load(), "items": items.Load(), "max_batch": maximum.Load(), "connects": connects.Load(), "active": active.Load(), "peak_active": peak.Load()})
	}))
	ports, _ := json.Marshal(map[string]string{"rpc": listener.Addr().String(), "metrics": metrics.Addr().String()})
	if err = os.WriteFile(*directory+"/ports.json", ports, 0600); err != nil {
		log.Fatal(err)
	}
	log.Fatal(rpc.Serve(listener))
}
