package inspect

import (
	"net/url"
	"regexp"
	"sort"
	"strings"
)

var windowsDrive = regexp.MustCompile(`^[A-Za-z]:[\\/]`)
var urlExpression = regexp.MustCompile(`(?i)https?://[^\s'"<>)]+`)

var packageSymlinkRoots = []string{"/opt", "/usr", "/bin", "/sbin", "/lib", "/lib64"}

var appImageSymlinkRoots = []string{
	"/bin", "/sbin", "/lib", "/lib64",
	"/usr/bin", "/usr/sbin", "/usr/lib", "/usr/lib64",
	"/usr/libexec",
}

// NormalizedArchivePath rejects NUL, absolute, Windows-drive, and ".." paths.
// A present empty string is the archive root (for example ".").
func NormalizedArchivePath(path string) (string, bool) {
	if strings.ContainsRune(path, 0) || strings.HasPrefix(path, "/") || windowsDrive.MatchString(path) {
		return "", false
	}
	normalized := fromNativeSeparators(path)
	for strings.HasPrefix(normalized, "./") {
		normalized = normalized[2:]
	}
	var safeParts []string
	for _, part := range splitPath(normalized) {
		if part == ".." {
			return "", false
		}
		if part != "." {
			safeParts = append(safeParts, part)
		}
	}
	return strings.Join(safeParts, "/"), true
}

func SafeSymlinkTarget(entryPath, target string) bool {
	if target == "" || strings.HasPrefix(target, "/") || strings.ContainsRune(target, 0) {
		return false
	}
	entry, ok := NormalizedArchivePath(entryPath)
	if !ok {
		return false
	}
	resolved := parentParts(entry)
	for _, part := range splitPath(fromNativeSeparators(target)) {
		if part == ".." {
			if len(resolved) == 0 {
				return false
			}
			resolved = resolved[:len(resolved)-1]
		} else if part != "." {
			resolved = append(resolved, part)
		}
	}
	return true
}

func SafePackageSymlinkTarget(entryPath, target string) bool {
	if SafeSymlinkTarget(entryPath, target) {
		return true
	}
	return absoluteTargetInRoots(target, packageSymlinkRoots)
}

func SafeAppImageSymlinkTarget(entryPath, target string) bool {
	if SafeSymlinkTarget(entryPath, target) {
		return true
	}
	return absoluteTargetInRoots(target, appImageSymlinkRoots)
}

func SymlinkReviewReason(entryPath, target string) string {
	if SafePackageSymlinkTarget(entryPath, target) {
		return ""
	}
	return "Symbolic link target '" + target + "' leaves the package tree or points outside " +
		"conventional package install roots. It is excluded from the Arch package " +
		"until you keep it."
}

func IsDebianSpecificPath(path string) bool {
	normalized := strings.TrimPrefix(path, "/")
	return pathHasPrefix(normalized, "etc/apt") ||
		pathHasPrefix(normalized, "usr/share/keyrings") ||
		pathHasPrefix(normalized, "usr/share/lintian")
}

func IsForeignPackageManagerPath(path string) bool {
	normalized := strings.TrimPrefix(path, "/")
	return IsDebianSpecificPath(normalized) ||
		pathHasPrefix(normalized, "etc/yum.repos.d") ||
		pathHasPrefix(normalized, "etc/dnf") ||
		pathHasPrefix(normalized, "etc/zypp") ||
		pathHasPrefix(normalized, "etc/pki/rpm-gpg") ||
		pathHasPrefix(normalized, "etc/rpm") ||
		pathHasPrefix(normalized, "usr/lib/sysimage/rpm") ||
		pathHasPrefix(normalized, "var/lib/rpm")
}

func ReviewReason(path string) string {
	normalized := strings.TrimPrefix(path, "/")
	switch {
	case pathHasPrefix(normalized, "etc/apparmor.d"):
		return "AppArmor policy. Arch supports AppArmor but does not enable it by default. Recommended: keep the vendor profile for compatibility if AppArmor is enabled; it is inert when AppArmor is disabled. Exclude it only if you intentionally want no vendor policy under /etc"
	case pathHasPrefix(normalized, "etc/apt"):
		return "Debian/APT configuration is excluded by default"
	case pathHasPrefix(normalized, "usr/share/keyrings"):
		return "Repository keyring may be Debian/APT-specific and is excluded by default"
	case pathHasPrefix(normalized, "usr/share/lintian"):
		return "Debian Lintian package-checker metadata has no function in an Arch package and is excluded by default"
	case pathHasPrefix(normalized, "etc/yum.repos.d"),
		pathHasPrefix(normalized, "etc/dnf"),
		pathHasPrefix(normalized, "etc/zypp"):
		return "RPM repository configuration is used only as update-source evidence and is excluded from the Arch package by default"
	case pathHasPrefix(normalized, "etc/pki/rpm-gpg"):
		return "RPM repository signing key is used only as update-source evidence and is excluded from the Arch package by default"
	case pathHasPrefix(normalized, "etc/rpm"),
		pathHasPrefix(normalized, "usr/lib/sysimage/rpm"),
		pathHasPrefix(normalized, "var/lib/rpm"):
		return "RPM package-manager state or configuration is incompatible with pacman and is excluded by default"
	case strings.HasPrefix(normalized, "usr/lib/systemd/"):
		return "Systemd unit should be reviewed for Arch compatibility"
	case pathHasPrefix(normalized, "etc"):
		return "System configuration should be reviewed"
	default:
		return ""
	}
}

func URLsFromText(text string) []string {
	unique := map[string]struct{}{}
	for _, candidate := range urlExpression.FindAllString(text, -1) {
		for strings.HasSuffix(candidate, ";") || strings.HasSuffix(candidate, ",") {
			candidate = candidate[:len(candidate)-1]
		}
		parsed, err := url.Parse(candidate)
		if err != nil || parsed.Host == "" {
			continue
		}
		unique[candidate] = struct{}{}
	}
	result := make([]string, 0, len(unique))
	for candidate := range unique {
		result = append(result, candidate)
	}
	sort.Strings(result)
	return result
}

func pathHasPrefix(path, root string) bool {
	return path == root || strings.HasPrefix(path, root+"/")
}

func absoluteTargetInRoots(target string, roots []string) bool {
	if target == "" || !strings.HasPrefix(target, "/") || strings.ContainsRune(target, 0) {
		return false
	}
	normalized := cleanAbsPath(fromNativeSeparators(target))
	if !strings.HasPrefix(normalized, "/") {
		return false
	}
	for _, root := range roots {
		if normalized == root || strings.HasPrefix(normalized, root+"/") {
			return true
		}
	}
	return false
}

func fromNativeSeparators(path string) string {
	return strings.ReplaceAll(path, "\\", "/")
}

func splitPath(path string) []string {
	if path == "" {
		return nil
	}
	raw := strings.Split(path, "/")
	parts := make([]string, 0, len(raw))
	for _, part := range raw {
		if part != "" {
			parts = append(parts, part)
		}
	}
	return parts
}

func parentParts(normalized string) []string {
	if normalized == "" {
		return nil
	}
	i := strings.LastIndex(normalized, "/")
	if i < 0 {
		return nil
	}
	return splitPath(normalized[:i])
}

func cleanAbsPath(path string) string {
	if !strings.HasPrefix(path, "/") {
		return path
	}
	var out []string
	for _, part := range strings.Split(path, "/") {
		if part == "" || part == "." {
			continue
		}
		if part == ".." {
			if len(out) > 0 {
				out = out[:len(out)-1]
			}
			continue
		}
		out = append(out, part)
	}
	if len(out) == 0 {
		return "/"
	}
	return "/" + strings.Join(out, "/")
}

func lastPathComponent(path string) string {
	path = strings.TrimRight(path, "/")
	if path == "" {
		return ""
	}
	if i := strings.LastIndex(path, "/"); i >= 0 {
		return path[i+1:]
	}
	return path
}

func fileStem(path string) string {
	name := lastPathComponent(path)
	if i := strings.LastIndex(name, "."); i > 0 {
		return name[:i]
	}
	return name
}

func fileSuffix(path string) string {
	name := lastPathComponent(path)
	if i := strings.LastIndex(name, "."); i > 0 && i < len(name)-1 {
		return strings.ToLower(name[i+1:])
	}
	return ""
}

func appendReason(existing, extra string) string {
	if extra == "" {
		return existing
	}
	if existing == "" {
		return extra
	}
	return existing + "; " + extra
}
