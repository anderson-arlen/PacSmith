package inspect

import (
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
)

var (
	platformSuffix = regexp.MustCompile(`(?i)[_-](?:linux[_-])?(?:x86_64|x64|amd64|aarch64|arm64)$`)
	simpleNameVer  = regexp.MustCompile(`(?i)^(.+?)[-_]v?([0-9][A-Za-z0-9.+~_-]*)$`)
	invalidPkgChar = regexp.MustCompile(`[^a-z0-9@._+\-]+`)
	repeatDash     = regexp.MustCompile(`-+`)
	dependName     = regexp.MustCompile(`^\s*([A-Za-z0-9@._+\-]+)`)
	desktopField   = func(key string) *regexp.Regexp {
		return regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(key) + `=(.*)$`)
	}
	desktopWhitespace = regexp.MustCompile(`\s`)
	safeCommandName   = regexp.MustCompile(`^[A-Za-z0-9@._+\-]+$`)
)

var archiveSuffixes = []string{
	".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz", ".tar.zst",
	".tar.xz", ".tar.bz2", ".tar.lz4", ".tar.gz",
	".tbz2", ".tgz", ".tar", ".zip", ".7z",
}

func sanitizePackageName(name string) string {
	result := strings.ToLower(strings.TrimSpace(name))
	result = invalidPkgChar.ReplaceAllString(result, "-")
	result = repeatDash.ReplaceAllString(result, "-")
	for strings.HasPrefix(result, ".") || strings.HasPrefix(result, "-") {
		result = result[1:]
	}
	for strings.HasSuffix(result, ".") || strings.HasSuffix(result, "-") {
		result = result[:len(result)-1]
	}
	if result == "" {
		return "vendor-package-bin"
	}
	return result
}

func strippedFilename(name string) string {
	if strings.HasSuffix(strings.ToLower(name), ".appimage") {
		return name[:len(name)-len(".AppImage")]
	}
	lower := strings.ToLower(name)
	for _, suffix := range archiveSuffixes {
		if strings.HasSuffix(lower, suffix) {
			return name[:len(name)-len(suffix)]
		}
	}
	return name
}

func inferNameVersion(path string, metadata *Metadata) {
	base := strippedFilename(filepath.Base(path))
	base = platformSuffix.ReplaceAllString(base, "")
	match := simpleNameVer.FindStringSubmatch(base)
	if match != nil {
		metadata.Package = sanitizePackageName(match[1])
		metadata.Version = match[2]
	} else {
		metadata.Package = sanitizePackageName(base)
		metadata.Version = "1.0.0"
	}
	if runtime.GOARCH == "amd64" {
		metadata.Architecture = "amd64"
	} else {
		metadata.Architecture = runtime.GOARCH
	}
	metadata.Description = metadata.Package
}

func dependencyName(expression string) string {
	match := dependName.FindStringSubmatch(expression)
	if match == nil {
		return ""
	}
	return match[1]
}

func desktopEntryField(contents, key string) string {
	match := desktopField(key).FindStringSubmatch(contents)
	if match == nil {
		return ""
	}
	return strings.TrimSpace(match[1])
}

func desktopEntryCommand(contents string) string {
	exec := desktopEntryField(contents, "Exec")
	if exec == "" {
		return ""
	}
	var executable string
	if strings.HasPrefix(exec, `"`) {
		closing := strings.IndexByte(exec[1:], '"')
		if closing < 0 {
			return ""
		}
		executable = exec[1 : 1+closing]
	} else {
		loc := desktopWhitespace.FindStringIndex(exec)
		if loc == nil {
			executable = exec
		} else {
			executable = exec[:loc[0]]
		}
	}
	command := lastPathComponent(executable)
	if !safeCommandName.MatchString(command) {
		return ""
	}
	lower := strings.ToLower(command)
	if lower == "env" || lower == "sh" || lower == "bash" || lower == "gio" ||
		lower == "gapplication" || strings.HasPrefix(lower, "dbus-") ||
		strings.HasPrefix(lower, "python") {
		return ""
	}
	return command
}

func normalizedDesktopContents(contents, command, iconName string) string {
	contents = strings.ReplaceAll(contents, "\r\n", "\n")
	lines := strings.Split(contents, "\n")
	hasExec := false
	hasIcon := false
	for i, line := range lines {
		if strings.HasPrefix(line, "Exec=") && command != "" {
			value := strings.TrimSpace(line[5:])
			loc := desktopWhitespace.FindStringIndex(value)
			arguments := ""
			if loc != nil {
				arguments = value[loc[0]:]
			}
			lines[i] = "Exec=" + command + arguments
			hasExec = true
		} else if strings.HasPrefix(line, "Icon=") && iconName != "" {
			lines[i] = "Icon=" + iconName
			hasIcon = true
		}
	}
	if !hasExec && command != "" {
		lines = append(lines, "Exec="+command)
	}
	if !hasIcon && iconName != "" {
		lines = append(lines, "Icon="+iconName)
	}
	return strings.Join(lines, "\n")
}
