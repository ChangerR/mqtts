// Package server exposes the generic policy store over an authenticated RPC API.
package server

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"strings"
	"time"

	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	"github.com/ChangerR/mqtts/modules/authz/internal/policy"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

type Service struct {
	pb.UnimplementedAuthorizationServer
	pb.UnimplementedAdministrationServer
	Store                  *policy.Store
	CacheTTL               uint32
	queryToken, adminToken string
	queries, writes        chan struct{}
}

func New(store *policy.Store, queryToken, adminToken string, cacheTTL uint32, concurrency int) (*Service, error) {
	valid := func(s string) bool { return len(s) >= 32 && len(s) <= 4096 && !strings.ContainsAny(s, "\r\n") }
	if store == nil || !valid(queryToken) || !valid(adminToken) || queryToken == adminToken || cacheTTL > 300000 || concurrency < 1 || concurrency > 4096 {
		return nil, errors.New("invalid authorization service configuration")
	}
	return &Service{Store: store, CacheTTL: cacheTTL, queryToken: queryToken, adminToken: adminToken, queries: make(chan struct{}, concurrency), writes: make(chan struct{}, 4)}, nil
}

// Query and management traffic have separate credentials and admission limits.
// Reject overload immediately instead of accumulating unbounded waiting RPCs.
func (s *Service) Intercept(ctx context.Context, req any, info *grpc.UnaryServerInfo, next grpc.UnaryHandler) (any, error) {
	token, slots, timeout := s.queryToken, s.queries, 2*time.Second
	if strings.HasPrefix(info.FullMethod, "/mqtts.authz.v1.Administration/") {
		token, slots, timeout = s.adminToken, s.writes, 5*time.Second
	}
	values := metadata.ValueFromIncomingContext(ctx, "authorization")
	if len(values) != 1 || subtle.ConstantTimeCompare([]byte(values[0]), []byte("Bearer "+token)) != 1 {
		return nil, status.Error(codes.Unauthenticated, "invalid RPC credential")
	}
	select {
	case slots <- struct{}{}:
		defer func() { <-slots }()
	default:
		return nil, status.Error(codes.ResourceExhausted, "authorization service busy")
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	if err := ctx.Err(); err != nil {
		return nil, status.FromContextError(err).Err()
	}
	return next(ctx, req)
}

func (s *Service) decision(allow bool, expiry uint64, revision string) *pb.Decision {
	outcome := pb.Outcome_DENY
	ttl, age := s.CacheTTL, uint32(1000)
	if ttl > 1000 {
		ttl = 1000
	}
	if allow {
		outcome = pb.Outcome_ALLOW
		ttl = s.CacheTTL
		age = 300000
	}
	return &pb.Decision{Outcome: outcome, ExpiresAtMs: expiry, CacheTtlMs: ttl, CacheMaxAgeMs: age, CacheRevision: revision}
}

func expiry(row *pb.Session) uint64 {
	if row == nil {
		return 0
	}
	end := row.PolicyValidUntilMs
	if row.ExpiresAtMs != 0 && row.ExpiresAtMs < end {
		end = row.ExpiresAtMs
	}
	return end
}

func (s *Service) Authenticate(ctx context.Context, req *pb.AuthenticateRequest) (*pb.Decision, error) {
	if len(req.Username) > 256 || len(req.ClientId) > 256 || len(req.Password) > 4096 {
		return nil, status.Error(codes.InvalidArgument, "credential too large")
	}
	rows, revision := s.Store.Snapshot([]string{req.Username})
	row := rows[0]
	now := uint64(time.Now().UnixMilli())
	hash := sha256.Sum256([]byte(req.Password))
	allow := row != nil && row.Enabled && row.ClientId == req.ClientId && expiry(row) > now && subtle.ConstantTimeCompare(hash[:], row.PasswordSha256) == 1
	// A service identity may have no connection expiry; its permission leases
	// are still bounded by the policy publisher's five-minute projection lease.
	end := uint64(0)
	if row != nil {
		end = row.ExpiresAtMs
	}
	return s.decision(allow, end, revision), nil
}

func (s *Service) BatchAuthorize(ctx context.Context, req *pb.BatchAuthorizeRequest) (*pb.BatchAuthorizeResponse, error) {
	if len(req.Requests) == 0 || len(req.Requests) > policy.MaxBatch {
		return nil, status.Error(codes.InvalidArgument, "batch must contain 1..64 requests")
	}
	names := make([]string, len(req.Requests))
	ids := make(map[uint64]bool, len(names))
	for i, item := range req.Requests {
		if item == nil || item.RequestId == 0 || ids[item.RequestId] {
			return nil, status.Error(codes.InvalidArgument, "request IDs must be unique and nonzero")
		}
		ids[item.RequestId] = true
		names[i] = item.Username
	}
	rows, revision := s.Store.Snapshot(names)
	out := &pb.BatchAuthorizeResponse{Results: make([]*pb.AuthorizationResult, len(names))}
	now := uint64(time.Now().UnixMilli())
	for i, item := range req.Requests {
		if err := ctx.Err(); err != nil {
			return nil, status.FromContextError(err).Err()
		}
		out.Results[i] = &pb.AuthorizationResult{RequestId: item.RequestId, Decision: s.decision(policy.Authorize(rows[i], item, now), expiry(rows[i]), revision)}
	}
	return out, nil
}

func (s *Service) GetRevision(context.Context, *pb.RevisionRequest) (*pb.RevisionResponse, error) {
	_, revision := s.Store.Versions()
	return &pb.RevisionResponse{Revision: revision}, nil
}

func (s *Service) Apply(ctx context.Context, req *pb.ApplyRequest) (*pb.ApplyResponse, error) {
	if err := ctx.Err(); err != nil {
		return nil, status.FromContextError(err).Err()
	}
	version, revision, err := s.Store.Apply(req)
	if err != nil {
		code := codes.Internal
		switch {
		case errors.Is(err, policy.ErrInvalid):
			code = codes.InvalidArgument
		case errors.Is(err, policy.ErrConflict):
			code = codes.Aborted
		case errors.Is(err, policy.ErrCapacity):
			code = codes.ResourceExhausted
		}
		return nil, status.Error(code, "policy update failed")
	}
	return &pb.ApplyResponse{Version: version, PolicyRevision: revision}, nil
}

func (s *Service) ListSessions(ctx context.Context, req *pb.ListSessionsRequest) (*pb.ListSessionsResponse, error) {
	if req.Namespace == "" || len(req.Namespace) > 128 || len(req.AfterUsername) > 256 || req.PageSize > 128 {
		return nil, status.Error(codes.InvalidArgument, "invalid session page")
	}
	limit := int(req.PageSize)
	if limit == 0 {
		limit = 64
	}
	rows, version, next := s.Store.List(req.Namespace, req.AfterUsername, limit)
	return &pb.ListSessionsResponse{Sessions: rows, Version: version, NextCursor: next}, nil
}
