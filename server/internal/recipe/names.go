package recipe

import (
	"regexp"
	"strconv"
	"strings"
)

var (
	unsafePackageChars = regexp.MustCompile(`[^a-z0-9@._+-]+`)
	repeatedHyphen     = regexp.MustCompile(`-+`)
	unsafeVersionChars = regexp.MustCompile(`[^A-Za-z0-9+._]+`)
	repeatedUnderscore = regexp.MustCompile(`_+`)
)

// SanitizePackageName lowercases, replaces illegal pkgname characters with
// hyphens, and collapses hyphen runs. Empty results become vendor-package-bin.
func SanitizePackageName(name string) string {
	result := strings.TrimSpace(strings.ToLower(name))
	result = unsafePackageChars.ReplaceAllString(result, "-")
	result = repeatedHyphen.ReplaceAllString(result, "-")
	result = strings.Trim(result, ".-")
	if result == "" {
		return "vendor-package-bin"
	}
	return result
}

// SplitEpochAndVersion peels a Debian epoch and revision, then maps the
// upstream version into an Arch pkgver.
func SplitEpochAndVersion(debianVersion string) (epoch, version string) {
	version = strings.TrimSpace(debianVersion)
	if colon := strings.IndexByte(version, ':'); colon > 0 {
		if _, err := strconv.ParseInt(version[:colon], 10, 32); err == nil {
			epoch = version[:colon]
			version = version[colon+1:]
		}
	}
	if dash := strings.LastIndexByte(version, '-'); dash > 0 {
		version = version[:dash]
	}
	version = strings.ReplaceAll(version, "~", ".")
	version = unsafeVersionChars.ReplaceAllString(version, "_")
	version = repeatedUnderscore.ReplaceAllString(version, "_")
	if version == "" {
		version = "0"
	}
	return epoch, version
}

func TranslateVersion(debianVersion string) string {
	_, version := SplitEpochAndVersion(debianVersion)
	return version
}

func TranslateArchitecture(debianArchitecture string) string {
	architecture := strings.ToLower(debianArchitecture)
	switch architecture {
	case "amd64":
		return "x86_64"
	case "arm64":
		return "aarch64"
	case "i386", "i686":
		return "i686"
	case "all", "noarch":
		return "any"
	default:
		return architecture
	}
}

// ShellQuote wraps value in single quotes, matching C++ PkgbuildGenerator.
func ShellQuote(value string) string {
	return "'" + strings.ReplaceAll(value, "'", `'"'"'`) + "'"
}

// percentEncode matches Qt QUrl::toPercentEncoding: UTF-8 bytes, uppercase
// hex, unreserved ALPHA / DIGIT / "-" / "." / "_" / "~" left as-is.
func percentEncode(value string) string {
	var b strings.Builder
	b.Grow(len(value))
	for i := 0; i < len(value); i++ {
		c := value[i]
		if isUnreserved(c) {
			b.WriteByte(c)
		} else {
			b.WriteByte('%')
			b.WriteByte(hexUpper(c >> 4))
			b.WriteByte(hexUpper(c & 0x0f))
		}
	}
	return b.String()
}

func isUnreserved(c byte) bool {
	switch {
	case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		return true
	case c == '-', c == '.', c == '_', c == '~':
		return true
	default:
		return false
	}
}

func hexUpper(n byte) byte {
	if n < 10 {
		return '0' + n
	}
	return 'A' + (n - 10)
}

func doubleQuotedPath(value string) string {
	return escapeDoubleQuoted(value, false)
}

func doubleQuotedExpandable(value string) string {
	return escapeDoubleQuoted(value, true)
}

func escapeDoubleQuoted(value string, expandDollars bool) string {
	var b strings.Builder
	b.Grow(len(value))
	for i := 0; i < len(value); i++ {
		switch value[i] {
		case '\\':
			b.WriteString(`\\`)
		case '"':
			b.WriteString(`\"`)
		case '$':
			if expandDollars {
				b.WriteByte('$')
			} else {
				b.WriteString(`\$`)
			}
		case '`':
			b.WriteString("\\`")
		default:
			b.WriteByte(value[i])
		}
	}
	return b.String()
}
