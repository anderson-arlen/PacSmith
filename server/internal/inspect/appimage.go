package inspect

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

var (
	basenameZero = regexp.MustCompile(`basename\s+(?:--\s+)?["']?\$\{?0\}?`)
	hereBinary   = regexp.MustCompile(`\$(?:HERE|APPDIR)/\$\{?BINARY_NAME`)
)

func analyzeAppImage(path string) (Analysis, error) {
	unsquashfs, err := exec.LookPath("unsquashfs")
	if err != nil {
		return Analysis{}, fmt.Errorf("Type 2 AppImage inspection requires /usr/bin/unsquashfs from Arch's squashfs-tools package")
	}
	offset, err := appImageSquashfsOffset(path)
	if err != nil {
		return Analysis{}, err
	}

	probeCtx, probeCancel := context.WithTimeout(context.Background(), 35*time.Second)
	defer probeCancel()
	probe := exec.CommandContext(probeCtx, unsquashfs, "-s", "-o", fmt.Sprintf("%d", offset), path)
	if out, err := probe.CombinedOutput(); err != nil {
		return Analysis{}, fmt.Errorf("Could not validate the AppImage SquashFS payload: %s", strings.TrimSpace(string(out)))
	}

	directory, err := os.MkdirTemp("", "pacsmith-appimage-*")
	if err != nil {
		return Analysis{}, fmt.Errorf("Could not create a private AppImage inspection directory")
	}
	defer os.RemoveAll(directory)

	extractCtx, extractCancel := context.WithTimeout(context.Background(), 125*time.Second)
	defer extractCancel()
	extract := exec.CommandContext(extractCtx, unsquashfs, "-no-progress", "-no-xattrs",
		"-o", fmt.Sprintf("%d", offset), "-d", directory, path)
	if out, err := extract.CombinedOutput(); err != nil {
		return Analysis{}, fmt.Errorf("Static AppImage extraction failed: %s", strings.TrimSpace(string(out)))
	}

	var result Analysis
	result.Type = SourceAppImage
	inferNameVersion(path, &result.Metadata)
	result.Install.ArchiveLayout = LayoutOptBundle
	result.Install.OptDirectory = result.Metadata.Package
	result.Install.AppImageOffset = offset
	desktopReferences := map[string]struct{}{}
	var icons []iconCandidate
	hasAppRun := false
	var expandedSize int64
	entries := 0

	err = filepath.WalkDir(directory, func(full string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			if os.IsPermission(walkErr) {
				return nil
			}
			return walkErr
		}
		if full == directory {
			return nil
		}
		entries++
		if entries > 100000 {
			return fmt.Errorf("AppImage contains more than 100,000 entries")
		}
		rel, err := filepath.Rel(directory, full)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		safe, ok := NormalizedArchivePath(rel)
		if !ok || safe == "" {
			return fmt.Errorf("Unsafe AppImage path: %s", rel)
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		payload := PayloadEntry{Path: safe}
		mode := info.Mode()
		switch {
		case mode.IsDir():
			payload.Type = "directory"
		case mode&os.ModeSymlink != 0:
			payload.Type = "symlink"
			target, err := os.Readlink(full)
			targetText := filepath.ToSlash(target)
			if err != nil || !SafeAppImageSymlinkTarget(payload.Path, targetText) {
				return fmt.Errorf("Unsafe AppImage symlink: %s -> %s", payload.Path, targetText)
			}
			payload.SymlinkTarget = targetText
		case mode.IsRegular():
			payload.Type = "file"
			payload.Size = info.Size()
			payload.Executable = mode&0o111 != 0
			expandedSize += payload.Size
			if expandedSize > 32*1024*1024*1024 {
				return fmt.Errorf("AppImage expands beyond the 32 GiB safety limit")
			}
		default:
			return fmt.Errorf("AppImage contains unsupported special entry: %s", payload.Path)
		}
		result.Payload = append(result.Payload, payload)
		if payload.Path == "AppRun" {
			result.Install.AppRun.Present = true
			if payload.Type == "symlink" {
				result.Install.AppRun.Script = false
				result.Install.AppRun.ReviewReason = "Symlink AppRun; not a text script"
			}
		}
		if payload.Type != "file" {
			return nil
		}
		if payload.Path == "AppRun" {
			if payload.Executable {
				hasAppRun = true
			}
			if payload.Size <= maxAppRunBytes {
				contents, readErr := os.ReadFile(full)
				if readErr == nil {
					captureAppRun(&result.Install.AppRun, contents)
					stored := &result.Payload[len(result.Payload)-1]
					stored.ContentSHA256 = sha256Hex(contents)
					if result.Install.AppRun.Script {
						stored.TextPreview = result.Install.AppRun.Contents
					}
				}
			}
		}
		if appImageDesktopCandidatePath(payload.Path, payload.Size) || isAnyIconCandidate(payload.Path, payload.Size) {
			file, err := os.Open(full)
			if err == nil {
				contents, _ := io.ReadAll(io.LimitReader(file, maxIconBytes+1))
				_ = file.Close()
				if appImageDesktopCandidatePath(payload.Path, payload.Size) &&
					len(contents) <= maxDesktopBytes && !containsNUL(contents) &&
					appImageApplicationDesktopEntry(bytesToString(contents)) {
					collectDesktopIconReferences(contents, desktopReferences)
					desktop := DesktopEntry{
						ID:                     fileStem(payload.Path),
						Enabled:                !strings.Contains(payload.Path, "/") && !strings.EqualFold(desktopEntryField(bytesToString(contents), "NoDisplay"), "true"),
						SourcePath:             payload.Path,
						Destination:            "/usr/share/applications/" + lastPathComponent(payload.Path),
						Contents:               bytesToString(contents),
						SourceSHA256:           sha256Hex(contents),
						OriginalContentsSHA256: sha256Hex(contents),
						Provenance:             deterministicProvenance("", "Application desktop entry detected at an AppDir integration path"),
					}
					replaced := false
					for i, candidate := range result.Install.DesktopEntries {
						if candidate.Destination != desktop.Destination {
							continue
						}
						if !strings.Contains(desktop.SourcePath, "/") && strings.Contains(candidate.SourcePath, "/") {
							result.Install.DesktopEntries[i] = desktop
						}
						replaced = true
						break
					}
					if !replaced {
						result.Install.DesktopEntries = append(result.Install.DesktopEntries, desktop)
					}
				}
				if len(icons) < maxIconCandidates && validIconContents(payload.Path, contents) {
					icons = append(icons, iconCandidate{path: payload.Path, contents: append([]byte(nil), contents...)})
				}
			}
		}
		return nil
	})
	if err != nil {
		return Analysis{}, err
	}
	if !hasAppRun {
		return Analysis{}, fmt.Errorf("AppImage payload has no executable AppRun entry point")
	}

	anyEnabled := false
	for _, desktop := range result.Install.DesktopEntries {
		if desktop.Enabled {
			anyEnabled = true
			break
		}
	}
	if len(result.Install.DesktopEntries) > 0 && !anyEnabled {
		best := 0
		bestScore := appImageDesktopScore(result.Install.DesktopEntries[0], result.Metadata.Package)
		for i := 1; i < len(result.Install.DesktopEntries); i++ {
			score := appImageDesktopScore(result.Install.DesktopEntries[i], result.Metadata.Package)
			if score > bestScore {
				best = i
				bestScore = score
			}
		}
		result.Install.DesktopEntries[best].Enabled = true
	}
	if selected := selectBestIcon(icons, desktopReferences, result.Metadata.Package); selected != nil {
		applySelectedIcon(&result, selected, "")
	}

	primaryDesktopPath := ""
	bestEnabledScore := math.MinInt
	for _, desktop := range result.Install.DesktopEntries {
		score := math.MinInt
		if desktop.Enabled {
			score = appImageDesktopScore(desktop, result.Metadata.Package)
		}
		if score > bestEnabledScore {
			bestEnabledScore = score
			primaryDesktopPath = desktop.SourcePath
		}
	}
	command := ""
	bestCommandScore := math.MinInt
	for _, desktop := range result.Install.DesktopEntries {
		score := math.MinInt
		if desktopEntryCommand(desktop.Contents) != "" {
			score = appImageDesktopScore(desktop, result.Metadata.Package)
		}
		if score > bestCommandScore {
			bestCommandScore = score
			command = desktopEntryCommand(desktop.Contents)
		}
	}
	if command != "" {
		command = strings.ToLower(command)
	}
	if command == "" {
		command = result.Metadata.Package
	}
	filtered := result.Install.DesktopEntries[:0]
	for _, desktop := range result.Install.DesktopEntries {
		if desktop.SourcePath == primaryDesktopPath {
			filtered = append(filtered, desktop)
			continue
		}
		candidateCommand := desktopEntryCommand(desktop.Contents)
		if candidateCommand != "" && strings.ToLower(candidateCommand) == command {
			filtered = append(filtered, desktop)
		}
	}
	result.Install.DesktopEntries = filtered

	launcher := LauncherMapping{
		Enabled:     true,
		SourcePath:  "AppRun",
		CommandName: command,
		Destination: "/usr/bin/" + command,
		Kind:        LauncherWrapper,
		Provenance:  deterministicProvenance("", "PacSmith host command launches the AppImage-provided AppRun entry point"),
	}
	result.Install.Launchers = []LauncherMapping{launcher}
	result.Install.BinarySourcePath = launcher.SourcePath
	result.Install.BinaryDestination = launcher.Destination
	for i := range result.Install.DesktopEntries {
		result.Install.DesktopEntries[i].Contents = normalizedDesktopContents(
			result.Install.DesktopEntries[i].Contents, command, result.Install.Icon.IconName)
	}
	return result, nil
}

func appImageSquashfsOffset(path string) (int64, error) {
	file, err := os.Open(path)
	if err != nil {
		return 0, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return 0, err
	}
	const scanLimit = int64(128 * 1024 * 1024)
	const chunkSize = int64(1024 * 1024)
	limit := info.Size()
	if limit > scanLimit {
		limit = scanLimit
	}
	var overlap []byte
	var position int64
	marker := []byte("hsqs")
	for position < limit {
		want := chunkSize
		if want > limit-position {
			want = limit - position
		}
		chunk := make([]byte, want)
		n, err := io.ReadFull(file, chunk)
		if err != nil && err != io.ErrUnexpectedEOF && err != io.EOF {
			break
		}
		chunk = chunk[:n]
		if n == 0 {
			break
		}
		combined := append(overlap, chunk...)
		from := 0
		for {
			index := bytes.Index(combined[from:], marker)
			if index < 0 {
				break
			}
			index += from
			candidate := position - int64(len(overlap)) + int64(index)
			if candidate >= 4096 && candidate%4 == 0 {
				return candidate, nil
			}
			from = index + 1
		}
		if len(combined) >= 3 {
			overlap = append([]byte(nil), combined[len(combined)-3:]...)
		} else {
			overlap = append([]byte(nil), combined...)
		}
		position += int64(n)
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			break
		}
	}
	return 0, fmt.Errorf("The Type 2 AppImage marker was present, but no bounded SquashFS payload was found")
}

func appImageDesktopCandidatePath(path string, size int64) bool {
	if !isDesktopEntry(path, size) {
		return false
	}
	if !strings.Contains(path, "/") {
		return true
	}
	const prefix = "usr/share/applications/"
	return strings.HasPrefix(path, prefix) && !strings.Contains(path[len(prefix):], "/")
}

func appImageApplicationDesktopEntry(contents string) bool {
	return desktopEntryField(contents, "Type") == "Application" &&
		desktopEntryField(contents, "Exec") != ""
}

func appRunLooksLikeScript(contents []byte) bool {
	return len(contents) > 0 && len(contents) <= maxAppRunBytes &&
		!containsNUL(contents) && bytes.HasPrefix(contents, []byte("#!"))
}

func appRunDispatchesOnLaunchName(contents string) bool {
	return strings.Contains(contents, "BINARY_NAME") ||
		basenameZero.MatchString(contents) ||
		hereBinary.MatchString(contents) ||
		(strings.Contains(contents, "APPIMAGE") && strings.Contains(strings.ToLower(contents), "basename"))
}

func captureAppRun(appRun *AppRunConfiguration, contents []byte) {
	appRun.Present = true
	appRun.OriginalContentsSHA256 = sha256Hex(contents)
	if !appRunLooksLikeScript(contents) {
		appRun.Script = false
		if bytesHasELF(contents) {
			appRun.ReviewReason = "Compiled ELF entry point"
		} else {
			appRun.ReviewReason = "Non-script AppRun"
		}
		return
	}
	appRun.Script = true
	appRun.Contents = bytesToString(contents)
	appRun.OriginalContents = appRun.Contents
	appRun.Provenance = deterministicProvenance("", "Vendor AppRun script captured from the extracted AppDir")
	if appRunDispatchesOnLaunchName(appRun.Contents) {
		appRun.ReviewReason = "Selects a binary from APPIMAGE or basename($0)"
	} else {
		appRun.ReviewReason = "AppDir entry point"
	}
}

func appImageDesktopScore(desktop DesktopEntry, packageName string) int {
	topLevel := !strings.Contains(desktop.SourcePath, "/")
	noDisplay := strings.EqualFold(desktopEntryField(desktop.Contents, "NoDisplay"), "true")
	command := desktopEntryCommand(desktop.Contents)
	icon := desktopEntryField(desktop.Contents, "Icon")
	name := desktopEntryField(desktop.Contents, "Name")
	score := 100
	if topLevel {
		score = 10000
	}
	if noDisplay {
		score -= 5000
	} else {
		score += 1000
	}
	for _, value := range []string{desktop.ID, command, icon, name} {
		if packageName != "" && strings.Contains(strings.ToLower(value), strings.ToLower(packageName)) {
			score += 500
		}
	}
	if command != "" {
		score += 250
	}
	return score
}
