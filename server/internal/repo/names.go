package repo

import (
	"errors"
	"fmt"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/recipe"
)

var (
	ErrInvalid  = errors.New("invalid request")
	ErrConflict = errors.New("conflict")
	ErrNotFound = errors.New("not found")
)

const (
	ChannelStable   = "stable"
	ChannelUnstable = "unstable"
	RepoName        = "pacsmith"
	KeyringPackage  = "pacsmith-keyring"

	TrustDirect        = "direct"
	TrustRootCertified = "root-certified"

	SoakSoaking  = "soaking"
	SoakEligible = "eligible"
	SoakPromoted = "promoted"
	SoakSkipped  = "skipped"

	SecretSigningKey = "repo.signing.key"

	DefaultListenHost  = "127.0.0.1"
	DefaultListenPort  = 8080
	DefaultSoakSeconds = 30 * 24 * 60 * 60
	DefaultKeyIDTrust  = "4"
)

func ReservedNames() []string {
	return []string{"pacsmith", "pacsmithd", "pacsmith-gui", KeyringPackage}
}

func IsReserved(name string) bool {
	n := strings.ToLower(strings.TrimSpace(name))
	for _, reserved := range ReservedNames() {
		if n == reserved {
			return true
		}
	}
	return false
}

func ValidChannel(channel string) bool {
	return channel == ChannelStable || channel == ChannelUnstable
}

func ValidTrustMode(mode string) bool {
	return mode == TrustDirect || mode == TrustRootCertified
}

func SanitizePrefix(prefix string) (string, error) {
	prefix = strings.TrimSpace(prefix)
	if prefix == "" {
		return "", nil
	}
	sanitized := recipe.SanitizePackageName(strings.TrimSuffix(prefix, "-"))
	if sanitized == "" || sanitized == "vendor-package-bin" {
		return "", fmt.Errorf("%w: package-name prefix is invalid", ErrInvalid)
	}
	if !strings.HasSuffix(prefix, "-") {
		sanitized += "-"
	} else {
		sanitized = recipe.SanitizePackageName(strings.TrimSuffix(prefix, "-")) + "-"
	}
	return sanitized, nil
}

func EffectiveName(archPackageName, originalName, prefix, override string) (effective, original string) {
	original = strings.TrimSpace(originalName)
	base := strings.TrimSpace(archPackageName)
	if original == "" {
		original = base
	}
	if base == "" {
		base = original
	}
	if trimmed := strings.TrimSpace(override); trimmed != "" {
		return recipe.SanitizePackageName(trimmed), original
	}
	prefix = strings.TrimSpace(prefix)
	if prefix == "" {
		return recipe.SanitizePackageName(base), original
	}
	return recipe.SanitizePackageName(prefix + base), original
}

func Compatibility(effective, original string, existingProvides, existingConflicts []string) (provides, conflicts []string) {
	provides = uniqueKeep(existingProvides)
	conflicts = uniqueKeep(existingConflicts)
	if effective == "" || original == "" || effective == original {
		return provides, conflicts
	}
	if !hasName(provides, original) {
		provides = append(provides, original+"=${pkgver}")
	}
	if !hasName(conflicts, original) {
		conflicts = append(conflicts, original)
	}
	return provides, conflicts
}

func SplitDebField(value string) []string {
	if strings.TrimSpace(value) == "" {
		return nil
	}
	parts := strings.FieldsFunc(value, func(r rune) bool {
		return r == ',' || r == '\n'
	})
	return uniqueKeep(parts)
}

func nameToken(value string) string {
	value = strings.TrimSpace(value)
	for _, sep := range []string{"=", "<", ">", ":"} {
		if i := strings.IndexAny(value, sep); i > 0 {
			value = value[:i]
		}
	}
	return strings.TrimSpace(value)
}

func hasName(values []string, name string) bool {
	for _, value := range values {
		if nameToken(value) == name {
			return true
		}
	}
	return false
}

func uniqueKeep(values []string) []string {
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

func FormatFingerprint(fp string) string {
	fp = strings.ToUpper(strings.ReplaceAll(strings.TrimSpace(fp), " ", ""))
	var b strings.Builder
	for i := 0; i < len(fp); i++ {
		if i > 0 && i%4 == 0 {
			b.WriteByte(' ')
		}
		b.WriteByte(fp[i])
	}
	return b.String()
}

func NormalizeFingerprint(fp string) string {
	return strings.ToUpper(strings.ReplaceAll(strings.TrimSpace(fp), " ", ""))
}
