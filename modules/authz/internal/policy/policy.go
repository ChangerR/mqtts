package policy

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strings"
	"unicode/utf8"

	pb "github.com/ChangerR/mqtts/modules/authz/api/authzv1"
)

const (
	MaxPayload      = 1024 * 1024
	MaxBatch        = 64
	MaxRPCBytes     = 4 * 1024 * 1024
	MaxSessionBytes = 256 * 1024
)

func ValidTopic(value string, filter bool) bool {
	if value == "" || len(value) > 4096 || !utf8.ValidString(value) || strings.ContainsRune(value, 0) {
		return false
	}
	parts := strings.Split(value, "/")
	for i, part := range parts {
		if strings.ContainsAny(part, "+#") && (!filter || (part != "+" && !(part == "#" && i == len(parts)-1))) {
			return false
		}
	}
	return true
}

// Covers checks filter containment, not just matching. A concrete grant cannot
// authorize a wildcard request. MQTT's root wildcard excludes $ system topics.
func Covers(allowed, requested string) bool {
	if !ValidTopic(allowed, true) || !ValidTopic(requested, true) {
		return false
	}
	if strings.HasPrefix(requested, "$") && !strings.HasPrefix(allowed, "$") {
		return false
	}
	a, r := strings.Split(allowed, "/"), strings.Split(requested, "/")
	for i, part := range a {
		if part == "#" {
			return i == len(a)-1
		}
		if i >= len(r) || r[i] == "#" {
			return false
		}
		if part != "+" && part != r[i] {
			return false
		}
	}
	return len(a) == len(r)
}

func pointer(root any, path string, fold bool) (any, bool) {
	current := root
	for _, part := range strings.Split(path[1:], "/") {
		part = strings.ReplaceAll(strings.ReplaceAll(part, "~1", "/"), "~0", "~")
		if fold {
			part = strings.ToLower(part)
		}
		object, ok := current.(map[string]any)
		if !ok {
			return nil, false
		}
		current, ok = object[part]
		if !ok {
			return nil, false
		}
	}
	return current, true
}

func readJSON(decoder *json.Decoder, fold bool, depth int) (any, error) {
	if depth > 64 {
		return nil, errors.New("JSON nesting limit")
	}
	token, err := decoder.Token()
	if err != nil {
		return nil, err
	}
	delim, compound := token.(json.Delim)
	if !compound {
		return token, nil
	}
	switch delim {
	case '{':
		value := map[string]any{}
		for decoder.More() {
			key, err := decoder.Token()
			if err != nil {
				return nil, err
			}
			name, ok := key.(string)
			if !ok {
				return nil, errors.New("invalid JSON key")
			}
			if fold {
				name = strings.ToLower(name)
			}
			if _, exists := value[name]; exists {
				return nil, errors.New("ambiguous JSON key")
			}
			item, err := readJSON(decoder, fold, depth+1)
			if err != nil {
				return nil, err
			}
			value[name] = item
		}
		end, err := decoder.Token()
		if err != nil || end != json.Delim('}') {
			return nil, errors.New("unterminated JSON object")
		}
		return value, nil
	case '[':
		var value []any
		for decoder.More() {
			item, err := readJSON(decoder, fold, depth+1)
			if err != nil {
				return nil, err
			}
			value = append(value, item)
		}
		end, err := decoder.Token()
		if err != nil || end != json.Delim(']') {
			return nil, errors.New("unterminated JSON array")
		}
		return value, nil
	}
	return nil, errors.New("invalid JSON delimiter")
}

func PayloadMatches(policy *pb.PayloadPolicy, request *pb.AuthorizeRequest) bool {
	if policy == nil {
		return true
	}
	if !request.HasPayload || len(request.Payload) > MaxPayload {
		return false
	}
	decoder := json.NewDecoder(bytes.NewReader(request.Payload))
	decoder.UseNumber()
	root, err := readJSON(decoder, policy.CaseInsensitiveKeys, 0)
	if err != nil {
		return false
	}
	if _, ok := root.(map[string]any); !ok {
		return false
	}
	if _, err = decoder.Token(); err != io.EOF {
		return false
	}
	for _, binding := range policy.Bindings {
		found := false
		want := binding.EqualsString
		if binding.EqualsTopic {
			want = request.Topic
		}
		for _, path := range binding.Paths {
			value, present := pointer(root, path, policy.CaseInsensitiveKeys)
			if !present {
				continue
			}
			found = true
			text, ok := value.(string)
			if !ok || text != want {
				return false
			}
		}
		if binding.RequiredAny && !found {
			return false
		}
	}
	return true
}

func Authorize(session *pb.Session, request *pb.AuthorizeRequest, now uint64) bool {
	if session == nil || !session.Enabled || session.ClientId != request.ClientId || session.PolicyValidUntilMs <= now || (session.ExpiresAtMs != 0 && session.ExpiresAtMs <= now) {
		return false
	}
	if request.Action != pb.Action_PUBLISH && request.Action != pb.Action_SUBSCRIBE {
		return false
	}
	if !ValidTopic(request.Topic, request.Action == pb.Action_SUBSCRIBE) || len(request.Payload) > MaxPayload || (!request.HasPayload && len(request.Payload) != 0) {
		return false
	}
	for _, permission := range session.Permissions {
		if permission.Action == request.Action && Covers(permission.TopicFilter, request.Topic) && PayloadMatches(permission.PayloadPolicy, request) {
			return true
		}
	}
	return false
}
