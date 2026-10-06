package policy

import (
	"fmt"
	"path/filepath"
	"reflect"
	"testing"

	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
)

func TestRevisionScopesAndHistoryLoss(t *testing.T) {
	path := filepath.Join(t.TempDir(), "auth.db")
	s, err := Open(path, 2000, MaxSessionBytes*20)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { s.Close() }()
	a, b := record("a"), record("b")
	v, initial, err := s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{a, b}})
	if err != nil {
		t.Fatal(err)
	}
	if r := s.Revision(""); r.IsDelta {
		t.Fatal("unknown revision must reset all grants")
	}
	b.PolicyValidUntilMs++
	v, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{b}})
	if err != nil {
		t.Fatal(err)
	}
	if r := s.Revision(initial); !r.IsDelta || len(r.InvalidatedUsernames) != 0 {
		t.Fatalf("lease renewed unrelated identities: %v", r)
	}
	a.Enabled = false
	v, changed, err := s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{a}})
	if err != nil {
		t.Fatal(err)
	}
	if r := s.Revision(initial); !r.IsDelta || !reflect.DeepEqual(r.InvalidatedUsernames, []string{"a"}) {
		t.Fatalf("wrong revocation scope: %v", r)
	}
	v, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Deletes: []string{"b"}})
	if err != nil {
		t.Fatal(err)
	}
	if r := s.Revision(changed); !r.IsDelta || !reflect.DeepEqual(r.InvalidatedUsernames, []string{"b"}) {
		t.Fatalf("wrong deletion scope: %v", r)
	}
	if r := s.Revision(initial); !r.IsDelta || !reflect.DeepEqual(r.InvalidatedUsernames, []string{"a", "b"}) {
		t.Fatalf("missing cumulative scope: %v", r)
	}
	for i := 0; i < maxRevisionChanges+1; i++ {
		a.Enabled = !a.Enabled
		v, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{a}})
		if err != nil {
			t.Fatal(err)
		}
	}
	if r := s.Revision(initial); r.IsDelta {
		t.Fatal("history gap must reset all grants")
	}
	_, old := s.Versions()
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err = Open(path, 2000, MaxSessionBytes*20)
	if err != nil {
		t.Fatal(err)
	}
	if r := s.Revision(old); r.IsDelta || r.Revision == old {
		t.Fatal("restart reused stale history")
	}
}

func TestRevisionReplyBound(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "auth.db"), 2000, MaxSessionBytes*20)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	var batches [][]string
	for batch := 0; batch < 9; batch++ {
		req := &pb.ApplyRequest{CreateOnly: true}
		var names []string
		for i := 0; i < 128; i++ {
			name := fmt.Sprintf("identity-%04d", batch*128+i)
			names = append(names, name)
			req.Upserts = append(req.Upserts, record(name))
		}
		if _, _, err = s.Apply(req); err != nil {
			t.Fatal(err)
		}
		batches = append(batches, names)
	}
	v, old := s.Versions()
	for _, names := range batches {
		v, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Deletes: names})
		if err != nil {
			t.Fatal(err)
		}
	}
	if r := s.Revision(old); r.IsDelta || len(r.InvalidatedUsernames) != 0 {
		t.Fatal("oversized delta must use bounded global reset")
	}
}
