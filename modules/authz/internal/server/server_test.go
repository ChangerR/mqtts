package server

import (
	"context"
	"crypto/sha256"
	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	"github.com/ChangerR/mqtts/modules/authz/internal/policy"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"net"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestRPCBatchAndCredentialIsolation(t *testing.T) {
	store, err := policy.Open(filepath.Join(t.TempDir(), "auth.db"), 100, 4*1024*1024)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	queryToken, adminToken := strings.Repeat("q", 32), strings.Repeat("a", 32)
	service, err := New(store, queryToken, adminToken, 10000, 128)
	if err != nil {
		t.Fatal(err)
	}
	rpc := grpc.NewServer(grpc.UnaryInterceptor(service.Intercept), grpc.MaxRecvMsgSize(policy.MaxRPCBytes))
	defer rpc.Stop()
	pb.RegisterAuthorizationServer(rpc, service)
	pb.RegisterAdministrationServer(rpc, service)
	listener := bufconn.Listen(4 * 1024 * 1024)
	go rpc.Serve(listener)
	conn, err := grpc.NewClient("passthrough:///test", grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return listener.Dial() }))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	query, admin := pb.NewAuthorizationClient(conn), pb.NewAdministrationClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	qctx := metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+queryToken)
	actx := metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+adminToken)
	if _, err = query.GetRevision(ctx, &pb.RevisionRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Fatal("anonymous query permitted")
	}
	if _, err = admin.ListSessions(qctx, &pb.ListSessionsRequest{Namespace: "test"}); status.Code(err) != codes.Unauthenticated {
		t.Fatal("query token can manage policies")
	}
	if _, err = query.GetRevision(actx, &pb.RevisionRequest{}); status.Code(err) != codes.Unauthenticated {
		t.Fatal("admin token can query")
	}
	now := uint64(time.Now().UnixMilli())
	hash := sha256.Sum256([]byte("password"))
	row := &pb.Session{Username: "identity", ClientId: "connection", PasswordSha256: hash[:], Namespace: "test", Enabled: true, ExpiresAtMs: now + 120000, PolicyValidUntilMs: now + 60000, Permissions: []*pb.Permission{{Action: pb.Action_SUBSCRIBE, TopicFilter: "devices/+/events"}}}
	update, err := admin.Apply(actx, &pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{row}})
	if err != nil {
		t.Fatal(err)
	}
	d, err := query.Authenticate(qctx, &pb.AuthenticateRequest{Username: row.Username, ClientId: row.ClientId, Password: "password"})
	if err != nil || d.Outcome != pb.Outcome_ALLOW || d.ExpiresAtMs != row.ExpiresAtMs {
		t.Fatalf("authenticate: %v %v", d, err)
	}
	d, err = query.Authenticate(qctx, &pb.AuthenticateRequest{Username: row.Username, ClientId: row.ClientId, Password: "wrong"})
	if err != nil || d.Outcome != pb.Outcome_DENY {
		t.Fatal("wrong password allowed")
	}
	batch := &pb.BatchAuthorizeRequest{}
	for i := 0; i < 64; i++ {
		topic := "devices/1/events"
		if i%2 == 1 {
			topic = "devices/#"
		}
		batch.Requests = append(batch.Requests, &pb.AuthorizeRequest{RequestId: uint64(i + 1), Username: row.Username, ClientId: row.ClientId, Action: pb.Action_SUBSCRIBE, Topic: topic})
	}
	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 10; j++ {
				out, err := query.BatchAuthorize(qctx, batch)
				if err != nil {
					t.Error(err)
					return
				}
				if len(out.Results) != 64 {
					t.Error("missing decisions")
					return
				}
				for k, result := range out.Results {
					want := pb.Outcome_ALLOW
					if k%2 == 1 {
						want = pb.Outcome_DENY
					}
					if result.RequestId != uint64(k+1) || result.Decision.Outcome != want || result.Decision.ExpiresAtMs > row.PolicyValidUntilMs {
						t.Error("incorrect mixed batch decision")
					}
				}
			}
		}()
	}
	wg.Wait()
	batch.Requests = append(batch.Requests, batch.Requests[0])
	if _, err = query.BatchAuthorize(qctx, batch); status.Code(err) != codes.InvalidArgument {
		t.Fatal("oversized batch accepted")
	}
	batch.Requests = batch.Requests[:2]
	batch.Requests[1].RequestId = 1
	if _, err = query.BatchAuthorize(qctx, batch); status.Code(err) != codes.InvalidArgument {
		t.Fatal("duplicate request ID accepted")
	}
	row.Enabled = false
	if _, err = admin.Apply(actx, &pb.ApplyRequest{ExpectedVersion: update.Version, Upserts: []*pb.Session{row}}); err != nil {
		t.Fatal(err)
	}
	batch.Requests = batch.Requests[:1]
	out, err := query.BatchAuthorize(qctx, batch)
	if err != nil || out.Results[0].Decision.Outcome != pb.Outcome_DENY {
		t.Fatal("revocation not visible")
	}
}
