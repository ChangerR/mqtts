package policy

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
	bolt "go.etcd.io/bbolt"
	"google.golang.org/protobuf/proto"
)

var (
	ErrConflict = errors.New("policy version changed")
	ErrCapacity = errors.New("authorization store capacity exceeded")
	ErrInvalid  = errors.New("invalid policy update")
)

type Store struct {
	db                    *bolt.DB
	writes                sync.Mutex
	mu                    sync.RWMutex
	sessions              map[string]*pb.Session // Published records are immutable.
	version, revision     string
	bytes                 int
	maxSessions, maxBytes int
}

func randomVersion() string {
	var value [24]byte
	if _, err := rand.Read(value[:]); err != nil {
		panic(err)
	}
	return hex.EncodeToString(value[:])
}

func Open(path string, maxSessions, maxBytes int) (*Store, error) {
	if maxSessions < 1 || maxBytes < MaxSessionBytes {
		return nil, ErrInvalid
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	db, err := bolt.Open(path, 0600, &bolt.Options{Timeout: time.Second})
	if err != nil {
		return nil, err
	}
	s := &Store{db: db, sessions: map[string]*pb.Session{}, maxSessions: maxSessions, maxBytes: maxBytes, version: randomVersion(), revision: randomVersion()}
	err = db.Update(func(tx *bolt.Tx) error {
		bucket, err := tx.CreateBucketIfNotExists([]byte("sessions-v1"))
		if err != nil {
			return err
		}
		var expired [][]byte
		err = bucket.ForEach(func(key, value []byte) error {
			session := &pb.Session{}
			if len(value) > MaxSessionBytes || proto.Unmarshal(value, session) != nil || session.Username != string(key) {
				return ErrInvalid
			}
			if session.ExpiresAtMs != 0 && session.ExpiresAtMs <= uint64(time.Now().UnixMilli()) {
				expired = append(expired, bytes.Clone(key))
				return nil
			}
			s.sessions[string(key)] = session
			s.bytes += len(value)
			if len(s.sessions) > maxSessions || s.bytes > maxBytes {
				return ErrCapacity
			}
			return nil
		})
		if err != nil {
			return err
		}
		for _, key := range expired {
			if err := bucket.Delete(key); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}
func (s *Store) Close() error { return s.db.Close() }
func (s *Store) Snapshot(names []string) ([]*pb.Session, string) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	rows := make([]*pb.Session, len(names))
	for i, name := range names {
		rows[i] = s.sessions[name]
	}
	return rows, s.revision
}
func (s *Store) Versions() (string, string) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.version, s.revision
}
func (s *Store) List(namespace, after string, limit int) ([]*pb.Session, string, string) {
	if limit < 1 || limit > 128 {
		limit = 64
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	keys := make([]string, 0)
	for name, row := range s.sessions {
		if row.Namespace == namespace && name > after {
			keys = append(keys, name)
		}
	}
	sort.Strings(keys)
	next := ""
	if len(keys) > limit {
		keys = keys[:limit]
		next = keys[len(keys)-1]
	}
	rows := make([]*pb.Session, 0, len(keys))
	size := 0
	for _, key := range keys {
		row := s.sessions[key]
		if size+proto.Size(row) > MaxRPCBytes/2 && len(rows) > 0 {
			next = rows[len(rows)-1].Username
			break
		}
		rows = append(rows, proto.Clone(row).(*pb.Session))
		size += proto.Size(row)
	}
	return rows, s.version, next
}

func Validate(row *pb.Session, now uint64) error {
	if row == nil || row.Username == "" || row.ClientId == "" || len(row.Username) > 256 || len(row.ClientId) > 256 || strings.ContainsAny(row.Username+row.ClientId, "\x00\r\n") || len(row.PasswordSha256) != 32 || row.Namespace == "" || len(row.Namespace) > 128 || len(row.SourceContext) > 262144 || len(row.Permissions) > 4096 || proto.Size(row) > MaxSessionBytes {
		return ErrInvalid
	}
	if row.PolicyValidUntilMs <= now || row.PolicyValidUntilMs > now+300000 || (row.ExpiresAtMs != 0 && (row.ExpiresAtMs <= now || row.ExpiresAtMs > now+300000)) {
		return ErrInvalid
	}
	for _, rule := range row.Permissions {
		if rule == nil || (rule.Action != pb.Action_PUBLISH && rule.Action != pb.Action_SUBSCRIBE) || !ValidTopic(rule.TopicFilter, true) {
			return ErrInvalid
		}
		if p := rule.PayloadPolicy; p != nil {
			if rule.Action != pb.Action_PUBLISH || len(p.Bindings) == 0 || len(p.Bindings) > 16 {
				return ErrInvalid
			}
			for _, b := range p.Bindings {
				if b == nil || len(b.Paths) == 0 || len(b.Paths) > 8 || len(b.EqualsString) > 4096 {
					return ErrInvalid
				}
				for _, path := range b.Paths {
					if !strings.HasPrefix(path, "/") || len(path) > 256 || strings.Count(path, "/") > 16 {
						return ErrInvalid
					}
				}
			}
		}
	}
	return nil
}
func sameRules(a, b *pb.Session) bool {
	x, y := proto.Clone(a).(*pb.Session), proto.Clone(b).(*pb.Session)
	x.PolicyValidUntilMs, y.PolicyValidUntilMs = 0, 0
	x.SourceContext, y.SourceContext = nil, nil
	return proto.Equal(x, y)
}
func (s *Store) Apply(request *pb.ApplyRequest) (string, string, error) {
	if request == nil || len(request.Upserts)+len(request.Deletes) == 0 || len(request.Upserts)+len(request.Deletes) > 128 || (request.CreateOnly && len(request.Deletes) != 0) {
		return "", "", ErrInvalid
	}
	s.writes.Lock()
	defer s.writes.Unlock()
	now := uint64(time.Now().UnixMilli())
	upserts := map[string]*pb.Session{}
	changes := map[string]bool{}
	for _, row := range request.Upserts {
		if Validate(row, now) != nil || changes[row.GetUsername()] {
			return "", "", ErrInvalid
		}
		row = proto.Clone(row).(*pb.Session)
		upserts[row.Username] = row
		changes[row.Username] = true
	}
	for _, name := range request.Deletes {
		if name == "" || changes[name] {
			return "", "", ErrInvalid
		}
		changes[name] = true
	}
	s.mu.RLock()
	if (!request.CreateOnly && request.ExpectedVersion == "") || (request.ExpectedVersion != "" && request.ExpectedVersion != s.version) {
		s.mu.RUnlock()
		return "", "", ErrConflict
	}
	count, total, changed := len(s.sessions), s.bytes, false
	metadataChanged := false
	for name, row := range upserts {
		old := s.sessions[name]
		if old != nil {
			if request.CreateOnly || old.ClientId != row.ClientId || !bytes.Equal(old.PasswordSha256, row.PasswordSha256) || old.ExpiresAtMs != row.ExpiresAtMs || old.Namespace != row.Namespace {
				s.mu.RUnlock()
				return "", "", ErrConflict
			}
			total -= proto.Size(old)
			changed = changed || !sameRules(old, row)
			metadataChanged = metadataChanged || !bytes.Equal(old.SourceContext, row.SourceContext)
		} else {
			count++
		}
		total += proto.Size(row)
	}
	for _, name := range request.Deletes {
		if old := s.sessions[name]; old != nil {
			count--
			total -= proto.Size(old)
			changed = changed || old.ExpiresAtMs == 0 || old.ExpiresAtMs > now
		}
	}
	revision := s.revision
	s.mu.RUnlock()
	if count > s.maxSessions || total > s.maxBytes {
		return "", "", ErrCapacity
	}
	err := s.db.Update(func(tx *bolt.Tx) error {
		b := tx.Bucket([]byte("sessions-v1"))
		for name, row := range upserts {
			value, err := proto.Marshal(row)
			if err != nil {
				return err
			}
			if err = b.Put([]byte(name), value); err != nil {
				return err
			}
		}
		for _, name := range request.Deletes {
			if err := b.Delete([]byte(name)); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		return "", "", err
	}
	// Durable write completes before publishing the immutable records. Reads
	// keep using the previous revision while the disk transaction is pending.
	s.mu.Lock()
	defer s.mu.Unlock()
	for name, row := range upserts {
		s.sessions[name] = row
	}
	for _, name := range request.Deletes {
		delete(s.sessions, name)
	}
	s.bytes = total
	// Inserts use unique, immutable identities and cannot change existing grants.
	// Lease-only renewals also do not invalidate a policy read. Keeping the CAS
	// version stable for both prevents login bursts from starving revocations.
	if changed || metadataChanged {
		s.version = randomVersion()
	}
	if changed {
		revision = randomVersion()
	}
	s.revision = revision
	return s.version, s.revision, nil
}

func (s *Store) Expire() {
	now := uint64(time.Now().UnixMilli())
	s.mu.RLock()
	version := s.version
	var names []string
	for name, row := range s.sessions {
		if row.ExpiresAtMs != 0 && row.ExpiresAtMs <= now {
			names = append(names, name)
			if len(names) == 128 {
				break
			}
		}
	}
	s.mu.RUnlock()
	if len(names) > 0 {
		_, _, _ = s.Apply(&pb.ApplyRequest{ExpectedVersion: version, Deletes: names})
	}
}
