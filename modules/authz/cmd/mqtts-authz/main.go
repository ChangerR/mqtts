package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"log"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	"github.com/ChangerR/mqtts/modules/authz/internal/policy"
	"github.com/ChangerR/mqtts/modules/authz/internal/server"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
)

func setting(name, fallback string) string {
	if s := os.Getenv(name); s != "" {
		return s
	}
	return fallback
}
func secret(name string) (string, error) {
	if path := os.Getenv(name + "_FILE"); path != "" {
		b, err := os.ReadFile(path)
		return strings.TrimSpace(string(b)), err
	}
	return os.Getenv(name), nil
}
func number(name string, fallback int) (int, error) {
	return strconv.Atoi(setting(name, strconv.Itoa(fallback)))
}

func run() error {
	query, err := secret("AUTHZ_QUERY_TOKEN")
	if err != nil {
		return err
	}
	admin, err := secret("AUTHZ_ADMIN_TOKEN")
	if err != nil {
		return err
	}
	capacity, err := number("AUTHZ_MAX_SESSIONS", 100000)
	if err != nil {
		return err
	}
	bytes, err := number("AUTHZ_MAX_STORE_BYTES", 256*1024*1024)
	if err != nil {
		return err
	}
	concurrency, err := number("AUTHZ_CONCURRENCY", 128)
	if err != nil {
		return err
	}
	ttl, err := number("AUTHZ_CACHE_TTL_MS", 10000)
	if err != nil || ttl < 0 || ttl > 300000 {
		return errors.New("invalid cache TTL")
	}
	store, err := policy.Open(setting("AUTHZ_STORE", "data/authorization.db"), capacity, bytes)
	if err != nil {
		return err
	}
	defer store.Close()
	service, err := server.New(store, query, admin, uint32(ttl), concurrency)
	if err != nil {
		return err
	}
	options := []grpc.ServerOption{grpc.UnaryInterceptor(service.Intercept), grpc.MaxRecvMsgSize(policy.MaxRPCBytes), grpc.MaxSendMsgSize(policy.MaxRPCBytes), grpc.MaxConcurrentStreams(uint32(concurrency))}
	certFile, keyFile := os.Getenv("AUTHZ_TLS_CERT"), os.Getenv("AUTHZ_TLS_KEY")
	if certFile != "" || keyFile != "" {
		cert, err := tls.LoadX509KeyPair(certFile, keyFile)
		if err != nil {
			return err
		}
		tlsConfig := &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{cert}}
		if caFile := os.Getenv("AUTHZ_CLIENT_CA"); caFile != "" {
			ca, err := os.ReadFile(caFile)
			if err != nil {
				return err
			}
			pool := x509.NewCertPool()
			if !pool.AppendCertsFromPEM(ca) {
				return errors.New("invalid client CA")
			}
			tlsConfig.ClientCAs = pool
			tlsConfig.ClientAuth = tls.RequireAndVerifyClientCert
		}
		options = append(options, grpc.Creds(credentials.NewTLS(tlsConfig)))
	} else if os.Getenv("AUTHZ_INSECURE") != "true" {
		return errors.New("configure TLS or explicitly set AUTHZ_INSECURE=true on a private network")
	}
	rpc := grpc.NewServer(options...)
	pb.RegisterAuthorizationServer(rpc, service)
	pb.RegisterAdministrationServer(rpc, service)
	listener, err := net.Listen("tcp", setting("AUTHZ_LISTEN", "127.0.0.1:50051"))
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				store.Expire()
			}
		}
	}()
	go func() {
		<-ctx.Done()
		timer := time.AfterFunc(5*time.Second, rpc.Stop)
		defer timer.Stop()
		rpc.GracefulStop()
	}()
	log.Printf("MQTTS authorization RPC listening on %s", listener.Addr())
	return rpc.Serve(listener)
}
func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}
