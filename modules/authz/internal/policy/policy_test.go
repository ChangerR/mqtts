package policy

import (
	"crypto/sha256"
	"fmt"
	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func TestFilterContainment(t *testing.T) {
	for _, c := range []struct {
		grant, request string
		want           bool
	}{
		{"devices/+/events", "devices/a/events", true}, {"devices/+/events", "devices/+/events", true},
		{"devices/a/events", "devices/+/events", false}, {"devices/+", "devices/#", false},
		{"devices/#", "devices", true}, {"devices/#", "devices/+/events", true},
		{"#", "$SYS/info", false}, {"+/info", "$SYS/info", false}, {"$SYS/#", "$SYS/info", true},
		{"devices/a", "devices/a/", false}, {"devices/#/bad", "devices/a", false},
	} {
		if got := Covers(c.grant, c.request); got != c.want {
			t.Errorf("%q covers %q = %v", c.grant, c.request, got)
		}
	}
}

func TestPayloadBindings(t *testing.T) {
	p := &pb.PayloadPolicy{CaseInsensitiveKeys: true, Bindings: []*pb.JsonBinding{
		{Paths: []string{"/identity/id", "/actor_id"}, EqualsString: "sensor-1", RequiredAny: true},
		{Paths: []string{"/destination"}, EqualsTopic: true},
	}}
	for _, c := range []struct {
		body string
		want bool
	}{
		{`{"identity":{"id":"sensor-1"},"destination":"devices/1"}`, true},
		{`{"IDENTITY":{"ID":"sensor-1"}}`, true},
		{`{"actor_id":"sensor-1","identity":{"id":"sensor-2"}}`, false},
		{`{"actor_id":"sensor-1","ACTOR_ID":"sensor-2"}`, false},
		{`{"actor_id":"sensor-1","actor_id":"sensor-1"}`, false},
		{`{"actor_id":"sensor-1","destination":"devices/2"}`, false},
		{`{"actor_id":1}`, false}, {`{}`, false}, {`null`, false},
		{`{"actor_id":"sensor-1"} {}`, false},
	} {
		if got := PayloadMatches(p, &pb.AuthorizeRequest{Topic: "devices/1", HasPayload: true, Payload: []byte(c.body)}); got != c.want {
			t.Errorf("%s: %v", c.body, got)
		}
	}
	if !PayloadMatches(nil, &pb.AuthorizeRequest{HasPayload: true, Payload: []byte{0, 255}}) {
		t.Fatal("generic binary payload rejected")
	}
}

func record(name string) *pb.Session {
	hash := sha256.Sum256([]byte("secret"))
	now := uint64(time.Now().UnixMilli())
	return &pb.Session{Username: name, ClientId: name, PasswordSha256: hash[:], Enabled: true, Namespace: "test", ExpiresAtMs: now + 120000, PolicyValidUntilMs: now + 60000, Permissions: []*pb.Permission{{Action: pb.Action_PUBLISH, TopicFilter: "devices/+"}}}
}
func TestStoreTransactionsRestartAndLeaseBounds(t *testing.T) {
	path := filepath.Join(t.TempDir(), "auth.db")
	s, err := Open(path, 10, MaxSessionBytes)
	if err != nil {
		t.Fatal(err)
	}
	row := record("a")
	_, revision := s.Versions()
	v, r, err := s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{row}})
	if err != nil || r != revision {
		t.Fatalf("insertion flushed unrelated grants: %v", err)
	}
	row.PolicyValidUntilMs += 1000
	v, r, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{row}})
	if err != nil || r != revision {
		t.Fatalf("lease renewal flushed grants: %v", err)
	}
	old := v
	row.Enabled = false
	v, r, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{row}})
	if err != nil || r == revision {
		t.Fatalf("revocation failed: %v", err)
	}
	row.Enabled = true
	if _, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: old, Upserts: []*pb.Session{row}}); err != ErrConflict {
		t.Fatal("late projection overwrote revocation")
	}
	row.ExpiresAtMs += 1000
	if _, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: []*pb.Session{row}}); err != ErrConflict {
		t.Fatal("original session lifetime was extended")
	}
	tooLong := record("b")
	tooLong.PolicyValidUntilMs = uint64(time.Now().Add(6 * time.Minute).UnixMilli())
	if _, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{tooLong}}); err != ErrInvalid {
		t.Fatal("unbounded projection lease")
	}
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err = Open(path, 10, MaxSessionBytes)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	rows, restartRevision := s.Snapshot([]string{"a"})
	if rows[0] == nil || rows[0].Enabled || restartRevision == r {
		t.Fatal("restart lost durable revocation or reused revision")
	}
}
func TestConcurrentReadersAndCapacity(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "auth.db"), 1, MaxSessionBytes)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	_, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{record("a")}})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{record("b")}}); err != ErrCapacity {
		t.Fatal("capacity not enforced")
	}
	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 500; j++ {
				rows, _ := s.Snapshot([]string{"a"})
				if rows[0] == nil {
					t.Error("lost session")
				}
			}
		}()
	}
	for i := 0; i < 20; i++ {
		v, _ := s.Versions()
		rows, _, _ := s.List("test", "", 64)
		rows[0].Enabled = !rows[0].Enabled
		if _, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: v, Upserts: rows}); err != nil {
			t.Fatal(err)
		}
	}
	wg.Wait()
}
func TestOriginalAndProjectionExpiry(t *testing.T) {
	row := record("a")
	req := &pb.AuthorizeRequest{ClientId: "a", Action: pb.Action_PUBLISH, Topic: "devices/1"}
	now := uint64(time.Now().UnixMilli())
	if !Authorize(row, req, now) {
		t.Fatal("valid request denied")
	}
	if Authorize(row, req, row.PolicyValidUntilMs) {
		t.Fatal("expired projection allowed")
	}
	row.PolicyValidUntilMs = row.ExpiresAtMs + 1000
	if Authorize(row, req, row.ExpiresAtMs) {
		t.Fatal("expired session allowed")
	}
	req.ClientId = "other"
	if Authorize(row, req, now) {
		t.Fatal("client binding bypass")
	}
}

func TestNewConnectionsDoNotStarveRevocationCAS(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "auth.db"), 100, 4*MaxSessionBytes)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	row := record("existing")
	version, _, err := s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{row}})
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"new-a", "new-b", "new-c"} {
		if _, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{record(name)}}); err != nil {
			t.Fatal(err)
		}
	}
	row.Enabled = false
	if _, _, err = s.Apply(&pb.ApplyRequest{ExpectedVersion: version, Upserts: []*pb.Session{row}}); err != nil {
		t.Fatal("login burst blocked revocation", err)
	}
}

func TestLargePolicyStillFitsBoundedManagementPages(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "auth.db"), 10, 4*MaxSessionBytes)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	row := record("large")
	row.Permissions = nil
	row.SourceContext = make([]byte, 128*1024)
	for i := 0; i < 700; i++ {
		topic := fmt.Sprintf("devices/%04d/events", i)
		row.Permissions = append(row.Permissions, &pb.Permission{Action: pb.Action_SUBSCRIBE, TopicFilter: topic}, &pb.Permission{Action: pb.Action_PUBLISH, TopicFilter: topic, PayloadPolicy: &pb.PayloadPolicy{Bindings: []*pb.JsonBinding{{Paths: []string{"/identity/id"}, EqualsString: "sender", RequiredAny: true}}}})
	}
	if _, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{row}}); err != nil {
		t.Fatal("legitimate large policy denied", err)
	}
	rows, _, _ := s.List("test", "", 128)
	if len(rows) != 1 || len(rows[0].Permissions) != 1400 {
		t.Fatal("management page truncated policy")
	}
	row.Username = "too-large"
	row.SourceContext = make([]byte, 262145)
	if _, _, err = s.Apply(&pb.ApplyRequest{CreateOnly: true, Upserts: []*pb.Session{row}}); err != ErrInvalid {
		t.Fatal("source metadata bound not enforced")
	}
}
