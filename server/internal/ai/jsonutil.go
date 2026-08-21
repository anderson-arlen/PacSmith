package ai

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"unicode/utf16"
)

func asObject(value any) map[string]any {
	switch typed := value.(type) {
	case map[string]any:
		return typed
	default:
		return map[string]any{}
	}
}

func asArray(value any) []any {
	switch typed := value.(type) {
	case []any:
		return typed
	case []map[string]any:
		out := make([]any, len(typed))
		for i, item := range typed {
			out[i] = item
		}
		return out
	case []string:
		out := make([]any, len(typed))
		for i, item := range typed {
			out[i] = item
		}
		return out
	default:
		return nil
	}
}

func asString(value any) string {
	switch typed := value.(type) {
	case string:
		return typed
	case json.Number:
		return typed.String()
	case fmt.Stringer:
		return typed.String()
	case float64:
		if typed == float64(int64(typed)) {
			return strconv.FormatInt(int64(typed), 10)
		}
		return strconv.FormatFloat(typed, 'f', -1, 64)
	case float32:
		return strconv.FormatFloat(float64(typed), 'f', -1, 32)
	case int:
		return strconv.Itoa(typed)
	case int64:
		return strconv.FormatInt(typed, 10)
	case bool:
		if typed {
			return "true"
		}
		return "false"
	case nil:
		return ""
	default:
		return fmt.Sprint(typed)
	}
}

func asBool(value any) bool {
	switch typed := value.(type) {
	case bool:
		return typed
	case string:
		return typed == "true" || typed == "1"
	case float64:
		return typed != 0
	case json.Number:
		n, err := typed.Int64()
		return err == nil && n != 0
	default:
		return false
	}
}

func objectString(object map[string]any, key string) string {
	if object == nil {
		return ""
	}
	return asString(object[key])
}

func objectBool(object map[string]any, key string) bool {
	if object == nil {
		return false
	}
	return asBool(object[key])
}

func objectObject(object map[string]any, key string) map[string]any {
	if object == nil {
		return map[string]any{}
	}
	return asObject(object[key])
}

func objectArray(object map[string]any, key string) []any {
	if object == nil {
		return nil
	}
	return asArray(object[key])
}

func stringList(values []any) []string {
	out := make([]string, 0, len(values))
	for _, value := range values {
		text := asString(value)
		if text != "" {
			out = append(out, text)
		}
	}
	return out
}

func utf16Len(value string) int {
	return len(utf16.Encode([]rune(value)))
}

func truncateUTF16(value string, limit int) string {
	if limit <= 0 {
		return ""
	}
	encoded := utf16.Encode([]rune(value))
	if len(encoded) <= limit {
		return value
	}
	return string(utf16.Decode(encoded[:limit]))
}

func compactJSON(value any) (string, error) {
	raw, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	return string(raw), nil
}

func uniqueStrings(values []string) []string {
	seen := map[string]struct{}{}
	out := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	return out
}
