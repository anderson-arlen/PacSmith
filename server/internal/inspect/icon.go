package inspect

import (
	"bytes"
	"encoding/binary"
	"strings"
)

const (
	maxIconBytes      = 4 * 1024 * 1024
	maxTotalIconBytes = 32 * 1024 * 1024
	maxIconCandidates = 128
	maxPreviewBytes   = 1024 * 1024
	maxDesktopBytes   = 256 * 1024
	maxKeyBytes       = 4 * 1024 * 1024
	maxScriptBytes    = 16 * 1024 * 1024
	maxPKGINFOBytes   = 1024 * 1024
	maxInstallBytes   = 4 * 1024 * 1024
	maxReviewFile     = 64 * 1024 * 1024
	maxAppRunBytes    = 256 * 1024
)

var pngSignature = []byte{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}

func validIconContents(path string, contents []byte) bool {
	switch fileSuffix(path) {
	case "png":
		if len(contents) < 24 || !bytes.HasPrefix(contents, pngSignature) {
			return false
		}
		width := binary.BigEndian.Uint32(contents[16:20])
		height := binary.BigEndian.Uint32(contents[20:24])
		return width > 0 && height > 0 && width <= 2048 && height <= 2048 &&
			uint64(width)*uint64(height) <= 4*1024*1024
	case "xpm":
		prefix := contents
		if len(prefix) > 256 {
			prefix = prefix[:256]
		}
		return len(contents) <= 1024*1024 && bytes.Contains(prefix, []byte("/* XPM */"))
	case "svg":
		prefix := strings.ToLower(strings.TrimSpace(string(contents[:min(len(contents), 4096)])))
		return len(contents) <= 2*1024*1024 && strings.Contains(prefix, "<svg") &&
			!strings.Contains(prefix, "<!entity") && !strings.Contains(prefix, "<!doctype")
	default:
		return false
	}
}

func collectDesktopIconReferences(contents []byte, references map[string]struct{}) {
	for _, line := range bytes.Split(contents, []byte("\n")) {
		line = bytes.TrimSpace(line)
		if !bytes.HasPrefix(line, []byte("Icon=")) {
			continue
		}
		value := strings.TrimSpace(bytesToString(line[5:]))
		if strings.HasPrefix(value, "/") {
			value = value[1:]
		}
		safe, ok := NormalizedArchivePath(value)
		if strings.Contains(value, "/") && !ok {
			continue
		}
		if ok {
			value = safe
		}
		if value == "" {
			continue
		}
		references[value] = struct{}{}
		references[lastPathComponent(value)] = struct{}{}
		references[fileStem(value)] = struct{}{}
	}
}

func iconScore(candidate iconCandidate, references map[string]struct{}, packageName string) int {
	fileName := lastPathComponent(candidate.path)
	stem := fileStem(candidate.path)
	score := 0
	if _, ok := references[candidate.path]; ok {
		score += 100000
	} else if _, ok := references[fileName]; ok {
		score += 100000
	} else if _, ok := references[stem]; ok {
		score += 100000
	}
	if strings.HasPrefix(candidate.path, "usr/share/icons/") {
		score += 10000
	}
	switch fileSuffix(candidate.path) {
	case "png":
		score += 3000
	case "svg":
		score += 2000
	default:
		score += 1000
	}
	if match := iconDimension.FindStringSubmatch(candidate.path); match != nil {
		width := atoi(match[1])
		height := atoi(match[2])
		if height < width {
			width = height
		}
		if width > 512 {
			width = 512
		}
		score += width
	}
	if packageName != "" && strings.Contains(strings.ToLower(stem), strings.ToLower(packageName)) {
		score += 500
	}
	return score
}

func selectBestIcon(candidates []iconCandidate, references map[string]struct{}, packageName string) *iconCandidate {
	if len(candidates) == 0 {
		return nil
	}
	best := 0
	bestScore := iconScore(candidates[0], references, packageName)
	for i := 1; i < len(candidates); i++ {
		score := iconScore(candidates[i], references, packageName)
		if score > bestScore {
			best = i
			bestScore = score
		}
	}
	return &candidates[best]
}

func applySelectedIcon(result *Analysis, selected *iconCandidate, rationale string) {
	if selected == nil {
		return
	}
	copied := append([]byte(nil), selected.contents...)
	result.Icon = &ExtractedIcon{SourcePath: selected.path, Contents: copied}
	result.Install.Icon.SourceKind = IconPayload
	result.Install.Icon.SourcePath = selected.path
	result.Install.Icon.ProjectPath = "files/integration/icon." + fileSuffix(selected.path)
	result.Install.Icon.SHA256 = sha256Hex(selected.contents)
	result.Install.Icon.Format = fileSuffix(selected.path)
	result.Install.Icon.IconName = result.Metadata.Package
	result.Install.Icon.Provenance = deterministicProvenance("", rationale)
}

func isDebIconCandidate(path string, size int64) bool {
	if !isAnyIconCandidate(path, size) {
		return false
	}
	return (strings.HasPrefix(path, "usr/share/icons/") && strings.Contains(path, "/apps/")) ||
		strings.HasPrefix(path, "usr/share/pixmaps/")
}

func isAnyIconCandidate(path string, size int64) bool {
	if size <= 0 || size > maxIconBytes {
		return false
	}
	switch fileSuffix(path) {
	case "png", "svg", "xpm":
		return true
	default:
		return false
	}
}

func isDesktopEntry(path string, size int64) bool {
	return size >= 0 && size <= maxDesktopBytes && strings.HasSuffix(path, ".desktop")
}

func isDebDesktopEntry(path string, size int64) bool {
	return isDesktopEntry(path, size) && strings.HasPrefix(path, "usr/share/applications/")
}

func maybeKeepIcon(candidates *[]iconCandidate, capturedBytes *int, path string, contents []byte) {
	if len(*candidates) >= maxIconCandidates {
		return
	}
	if len(contents) > maxTotalIconBytes-*capturedBytes {
		return
	}
	if !validIconContents(path, contents) {
		return
	}
	*capturedBytes += len(contents)
	copied := append([]byte(nil), contents...)
	*candidates = append(*candidates, iconCandidate{path: path, contents: copied})
}
