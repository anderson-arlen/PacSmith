package inspect

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"time"
	"unicode/utf8"
)

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func fingerprint(name, evidence string) string {
	h := sha256.New()
	h.Write([]byte(name))
	h.Write([]byte{0})
	h.Write([]byte(evidence))
	return hex.EncodeToString(h.Sum(nil))
}

func scriptContentFingerprint(script MaintainerScript) string {
	return fingerprint(script.Name, script.Contents)
}

func bytesToString(data []byte) string {
	if utf8.Valid(data) {
		return string(data)
	}
	return strings.ToValidUTF8(string(data), "\uFFFD")
}

func utf8Preview(data []byte) string {
	if len(data) == 0 || containsNUL(data) || !utf8.Valid(data) {
		return ""
	}
	return string(data)
}

func containsNUL(data []byte) bool {
	for _, b := range data {
		if b == 0 {
			return true
		}
	}
	return false
}

func deterministicProvenance(sourceFingerprint, rationale string) FieldProvenance {
	return FieldProvenance{
		Origin:            OriginDeterministic,
		SourceFingerprint: sourceFingerprint,
		Rationale:         rationale,
		Timestamp:         time.Now().UTC(),
	}
}
