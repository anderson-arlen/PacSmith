package inspect

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var iconDimension = regexp.MustCompile(`/(\d+)x(\d+)/apps/`)

func atoi(s string) int {
	n, _ := strconv.Atoi(s)
	return n
}

func looksLikeLibrary(path string) bool {
	name := strings.ToLower(lastPathComponent(path))
	return strings.HasSuffix(name, ".so") || strings.Contains(name, ".so.") ||
		strings.HasSuffix(name, ".dll") || strings.HasSuffix(name, ".dylib")
}

func isDirectOptApplication(path string) bool {
	components := splitPath(path)
	return len(components) == 3 && components[0] == "opt"
}

func debLikelyUserCommand(path string) bool {
	name := strings.ToLower(lastPathComponent(path))
	if name == "" || strings.HasSuffix(name, ".so") || strings.Contains(name, ".so.") ||
		strings.Contains(name, "debug") || strings.Contains(name, "crashpad") ||
		strings.Contains(name, "sandbox") {
		return false
	}
	return isDirectOptApplication(path) || strings.HasPrefix(path, "usr/bin/") ||
		strings.HasPrefix(path, "usr/local/bin/") || strings.HasPrefix(path, "bin/")
}

var ignoredCommandDirs = map[string]struct{}{
	"lib": {}, "lib64": {}, "libexec": {}, "plugins": {}, "imageformats": {},
	"iconengines": {}, "platforms": {}, "platformthemes": {}, "generic": {},
	"tls": {}, "qml": {}, "translations": {}, "resources": {}, "multimedia": {},
	"audio": {}, "bearer": {}, "printsupport": {}, "sqldrivers": {},
	"styles": {}, "xcbglintegrations": {},
}

func ignoredCommandDirectory(path string) bool {
	parts := splitPath(path)
	for i := 0; i+1 < len(parts); i++ {
		if _, ok := ignoredCommandDirs[strings.ToLower(parts[i])]; ok {
			return true
		}
	}
	return false
}

func archiveLikelyUserCommand(path string) bool {
	name := strings.ToLower(lastPathComponent(path))
	if name == "" || looksLikeLibrary(path) || strings.Contains(name, "debug") ||
		strings.Contains(name, "crashpad") || strings.Contains(name, "sandbox") ||
		name == "apprun" {
		return false
	}
	return isDirectOptApplication(path) || strings.HasPrefix(path, "bin/") ||
		strings.HasPrefix(path, "usr/bin/") || strings.HasPrefix(path, "usr/local/bin/") ||
		!strings.Contains(path, "/")
}

func looksLikeExecutableMagic(header []byte) bool {
	return bytesHasELF(header) || bytes.HasPrefix(header, []byte("#!"))
}

func bytesHasELF(header []byte) bool {
	return len(header) >= 4 && header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F'
}

var skippedPeekSuffixes = map[string]struct{}{
	"png": {}, "svg": {}, "xpm": {}, "jpg": {}, "jpeg": {}, "gif": {},
	"ico": {}, "bmp": {}, "webp": {}, "xml": {}, "json": {}, "txt": {},
	"md": {}, "html": {}, "css": {}, "qml": {}, "ts": {}, "qm": {},
	"rcc": {}, "pak": {}, "dat": {}, "desktop": {}, "so": {}, "a": {}, "o": {},
}

func shouldPeekExecutable(payload PayloadEntry) bool {
	if payload.Type != "file" || payload.Size < 4 || payload.Size > 64*1024*1024 {
		return false
	}
	if isDesktopEntry(payload.Path, payload.Size) || isAnyIconCandidate(payload.Path, payload.Size) ||
		looksLikeLibrary(payload.Path) {
		return false
	}
	_, skipped := skippedPeekSuffixes[fileSuffix(payload.Path)]
	return !skipped
}

func exclusionRoot(path string) string {
	roots := []string{
		"etc/apt", "usr/share/keyrings", "usr/share/lintian",
		"etc/yum.repos.d", "etc/dnf", "etc/zypp", "etc/pki/rpm-gpg",
		"etc/rpm", "usr/lib/sysimage/rpm", "var/lib/rpm",
	}
	for _, root := range roots {
		if pathHasPrefix(path, root) {
			return root
		}
	}
	return path
}

func isRepositoryKeyPath(path string) bool {
	return strings.HasPrefix(path, "usr/share/keyrings/") ||
		strings.HasPrefix(path, "etc/pki/rpm-gpg/")
}

func shouldInspectRepositoryContents(path string, size int64) bool {
	if size < 0 || size > maxPreviewBytes {
		return false
	}
	return strings.HasPrefix(path, "etc/apt/") ||
		strings.HasPrefix(path, "etc/yum.repos.d/") ||
		strings.HasPrefix(path, "etc/dnf/") ||
		strings.HasPrefix(path, "etc/zypp/") ||
		strings.HasSuffix(path, ".sources") ||
		strings.HasSuffix(path, ".list") ||
		strings.HasSuffix(path, ".repo")
}

func shouldInspectDebContents(path string, size int64) bool {
	if size < 0 || size > maxPreviewBytes {
		return false
	}
	return strings.HasPrefix(path, "etc/apt/") ||
		strings.HasSuffix(path, ".sources") ||
		strings.HasSuffix(path, ".list")
}

func unixMode(mode int64) int64 {
	return mode & 07777
}

func hasSetID(mode int64) bool {
	return mode&06000 != 0
}

func hasExecBit(mode int64) bool {
	return mode&0111 != 0
}

func coversPath(parent, child string) bool {
	return child == parent || strings.HasPrefix(child, parent+"/")
}

func payloadRuleFingerprint(entries []PayloadEntry, path string) string {
	matched := make([]PayloadEntry, 0)
	for _, entry := range entries {
		if coversPath(path, entry.Path) {
			matched = append(matched, entry)
		}
	}
	if len(matched) == 0 {
		return ""
	}
	sort.Slice(matched, func(i, j int) bool { return matched[i].Path < matched[j].Path })
	h := sha256.New()
	nul := []byte{0}
	for _, entry := range matched {
		for _, field := range []string{
			entry.Path, entry.Type, entry.SymlinkTarget,
			strconv.FormatInt(entry.Size, 10), entry.ContentSHA256,
		} {
			h.Write([]byte(field))
			h.Write(nul)
		}
	}
	return hex.EncodeToString(h.Sum(nil))
}

func bindDefaultPayloadRuleFingerprints(analysis *Analysis) {
	if analysis == nil {
		return
	}
	for i := range analysis.PayloadRules {
		rule := &analysis.PayloadRules[i]
		if rule.UserDecision || !rule.Excluded {
			continue
		}
		if rule.Reason == "AI-reviewed payload decision" ||
			rule.Reason == "User-approved AI payload decision" {
			continue
		}
		fingerprint := payloadRuleFingerprint(analysis.Payload, rule.Path)
		if fingerprint == "" {
			continue
		}
		rule.AcknowledgedFingerprint = fingerprint
	}
}
