package inspect

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

var launcherOperation = regexp.MustCompile(`(?:ln[\s\S]{0,80}-s|update-alternatives[\s\S]{0,80}--install)`)

var maintainerScriptNames = map[string]struct{}{
	"preinst": {}, "postinst": {}, "prerm": {}, "postrm": {}, "config": {},
}

func analyzeDEB(path string) (Analysis, error) {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return Analysis{}, fmt.Errorf("DEB file does not exist or is not a regular file: %s", path)
	}

	file, err := os.Open(path)
	if err != nil {
		return Analysis{}, err
	}
	defer file.Close()

	reader, err := newArReader(file)
	if err != nil {
		return Analysis{}, err
	}

	tmpDir, err := os.MkdirTemp("", "pacsmith-deb-*")
	if err != nil {
		return Analysis{}, fmt.Errorf("Could not create temporary archive files")
	}
	defer os.RemoveAll(tmpDir)

	controlPath := filepath.Join(tmpDir, "control.tar")
	dataPath := filepath.Join(tmpDir, "data.tar")
	foundControl := false
	foundData := false
	validMarker := false

	for {
		hdr, err := reader.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return Analysis{}, err
		}
		name := strings.TrimSuffix(hdr.Name, "/")
		switch {
		case name == "debian-binary":
			marker, readErr := readLimited(reader, 64)
			if readErr != nil {
				return Analysis{}, readErr
			}
			validMarker = string(bytes.TrimSpace(marker)) == "2.0"
		case strings.HasPrefix(name, "control.tar") && !foundControl:
			if err := writeMember(controlPath, reader); err != nil {
				return Analysis{}, err
			}
			foundControl = true
		case strings.HasPrefix(name, "data.tar") && !foundData:
			if err := writeMember(dataPath, reader); err != nil {
				return Analysis{}, err
			}
			foundData = true
		}
	}
	if !validMarker || !foundControl || !foundData {
		return Analysis{}, fmt.Errorf("Not a supported Debian binary package (missing debian-binary, control.tar.*, or data.tar.*)")
	}

	var result Analysis
	result.Type = SourceDebian
	var controlData []byte
	if err := walkTarFile(controlPath, "control.tar", func(entry walkedEntry, body io.Reader) error {
		safePath, ok := NormalizedArchivePath(entry.RawName)
		if !ok {
			return fmt.Errorf("Unsafe path in control archive: %s", entry.RawName)
		}
		name := lastPathComponent(safePath)
		if entry.Kind != kindFile {
			_, _ = io.Copy(io.Discard, body)
			return nil
		}
		if name == "control" {
			contents, err := readLimited(body, maxScriptBytes)
			if err != nil {
				return err
			}
			controlData = contents
			return nil
		}
		if _, isScript := maintainerScriptNames[name]; isScript {
			contents, err := readLimited(body, maxScriptBytes)
			if err != nil {
				return err
			}
			result.MaintainerScripts = append(result.MaintainerScripts, MaintainerScript{
				Name:     name,
				Contents: bytesToString(contents),
			})
			return nil
		}
		_, _ = io.Copy(io.Discard, body)
		return nil
	}); err != nil {
		if strings.Contains(err.Error(), "Unsafe") || strings.Contains(err.Error(), "exceeds") {
			return Analysis{}, err
		}
		return Analysis{}, fmt.Errorf("Could not read control archive: %w", err)
	}
	if len(controlData) == 0 {
		return Analysis{}, fmt.Errorf("The DEB control archive has no control metadata file")
	}
	result.Metadata = parseControlPackage(controlData)
	if result.Metadata.Package == "" || result.Metadata.Version == "" {
		return Analysis{}, fmt.Errorf("The DEB control metadata is missing Package or Version")
	}
	declarations := result.Metadata.PreDepends
	if declarations != "" && result.Metadata.Depends != "" {
		declarations += ", "
	}
	declarations += result.Metadata.Depends
	result.Dependencies = ParseDependencies(declarations)
	_ = ApplyVerifiedMappings(result.Dependencies, LoadVerifiedMappings())

	candidates := map[string]struct{}{}
	evidence := analyzeScriptEvidence(result.MaintainerScripts)
	result.ScriptFindings = evidence.Findings
	result.SigningKeys = evidence.SigningKeys
	result.AptCandidates = evidence.AptCandidates
	for _, script := range result.MaintainerScripts {
		for _, u := range URLsFromText(script.Contents) {
			candidates[u] = struct{}{}
		}
	}

	rulePaths := map[string]struct{}{}
	desktopIconReferences := map[string]struct{}{}
	var iconCandidates []iconCandidate
	capturedIconBytes := 0
	if err := walkTarFile(dataPath, "data.tar", func(entry walkedEntry, body io.Reader) error {
		safePath, ok := NormalizedArchivePath(entry.RawName)
		if !ok {
			return fmt.Errorf("Unsafe path in data archive: %s", entry.RawName)
		}
		if safePath == "" {
			_, _ = io.Copy(io.Discard, body)
			return nil
		}
		payload := PayloadEntry{
			Path: safePath,
			Type: entryTypeName(entry.Kind),
			Size: entry.Size,
		}
		if payload.Size < 0 {
			payload.Size = 0
		}
		payload.SymlinkTarget = entry.SymlinkTarget
		if entry.HardlinkTarget != "" {
			if _, hardOK := NormalizedArchivePath(entry.HardlinkTarget); !hardOK {
				return fmt.Errorf("Unsafe hardlink in data archive: %s", safePath)
			}
		}
		payload.ReviewReason = ReviewReason(safePath)
		if payload.SymlinkTarget != "" {
			payload.ReviewReason = appendReason(payload.ReviewReason, SymlinkReviewReason(safePath, payload.SymlinkTarget))
		}
		payload.RequiresReview = payload.ReviewReason != "" && entry.Kind != kindDir
		if hasSetID(entry.Mode) {
			payload.ReviewReason = appendReason(payload.ReviewReason, "Set-user-ID or set-group-ID permission requires review")
			payload.RequiresReview = true
		}
		special := isSpecialKind(entry.Kind)
		if special {
			payload.ReviewReason = appendReason(payload.ReviewReason, "Special filesystem entry is excluded by default")
			payload.RequiresReview = true
		}
		if (entry.Kind == kindFile || entry.Kind == kindSymlink) && hasExecBit(entry.Mode) && debLikelyUserCommand(safePath) {
			launcher := LauncherMapping{
				Enabled:     !isDirectOptApplication(safePath),
				SourcePath:  safePath,
				CommandName: lastPathComponent(safePath),
				Destination: "/usr/bin/" + lastPathComponent(safePath),
			}
			if launcher.Enabled {
				launcher.Provenance = deterministicProvenance("", "Command detected in the inspected Debian payload")
			} else {
				launcher.Provenance = deterministicProvenance("", "Direct /opt application executable detected; enable it explicitly to expose a command")
			}
			result.Install.Launchers = append(result.Install.Launchers, launcher)
			if result.Install.BinarySourcePath == "" {
				result.Install.BinarySourcePath = launcher.SourcePath
				result.Install.BinaryDestination = launcher.Destination
			}
		}
		if IsDebianSpecificPath(safePath) {
			aptPath := pathHasPrefix(safePath, "etc/apt")
			keyringPath := pathHasPrefix(safePath, "usr/share/keyrings")
			lintianPath := pathHasPrefix(safePath, "usr/share/lintian")
			exclusionPath := safePath
			switch {
			case aptPath:
				exclusionPath = "etc/apt"
			case keyringPath:
				exclusionPath = "usr/share/keyrings"
			case lintianPath:
				exclusionPath = "usr/share/lintian"
			}
			shouldExclude := aptPath || keyringPath || lintianPath || entry.Kind != kindDir
			if shouldExclude {
				if _, exists := rulePaths[exclusionPath]; !exists {
					result.PayloadRules = append(result.PayloadRules, PayloadRule{
						Path:     exclusionPath,
						Excluded: true,
						Reason:   payload.ReviewReason,
					})
					rulePaths[exclusionPath] = struct{}{}
				}
			}
		}
		if special {
			if _, exists := rulePaths[safePath]; !exists {
				result.PayloadRules = append(result.PayloadRules, PayloadRule{
					Path:     safePath,
					Excluded: true,
					Reason:   "Special filesystem entry is excluded by default",
				})
				rulePaths[safePath] = struct{}{}
			}
		}
		if payload.SymlinkTarget != "" && !SafePackageSymlinkTarget(safePath, payload.SymlinkTarget) {
			if _, exists := rulePaths[safePath]; !exists {
				result.PayloadRules = append(result.PayloadRules, PayloadRule{
					Path:     safePath,
					Excluded: true,
					Reason:   payload.ReviewReason,
				})
				rulePaths[safePath] = struct{}{}
			}
		}
		desktopEntry := entry.Kind == kindFile && isDebDesktopEntry(safePath, payload.Size)
		iconCandidate := entry.Kind == kindFile && isDebIconCandidate(safePath, payload.Size)
		if entry.Kind == kindFile && (payload.RequiresReview || shouldInspectDebContents(safePath, payload.Size) || desktopEntry || iconCandidate) {
			captureLimit := maxPreviewBytes
			if iconCandidate {
				captureLimit = maxIconBytes
			}
			captured, sum, truncated, inspectErr := inspectBody(body, captureLimit, 0)
			if inspectErr != nil {
				return inspectErr
			}
			payload.ContentSHA256 = sum
			payload.PreviewTruncated = truncated
			payload.TextPreview = utf8Preview(captured)
			if payload.TextPreview != "" && !truncated {
				payloadEvidence := analyzeScriptEvidence([]MaintainerScript{{
					Name:     "payload/" + safePath,
					Contents: payload.TextPreview,
				}})
				for _, candidate := range payloadEvidence.RPMCandidates {
					duplicate := false
					for _, existing := range result.RPMCandidates {
						if existing.BaseURL == candidate.BaseURL && existing.Architecture == candidate.Architecture {
							duplicate = true
							break
						}
					}
					if !duplicate {
						result.RPMCandidates = append(result.RPMCandidates, candidate)
					}
				}
				for _, key := range payloadEvidence.SigningKeys {
					duplicate := false
					for _, existing := range result.SigningKeys {
						if bytes.Equal(existing.Contents, key.Contents) {
							duplicate = true
							break
						}
					}
					if !duplicate {
						result.SigningKeys = append(result.SigningKeys, key)
					}
				}
				for _, u := range URLsFromText(payload.TextPreview) {
					candidates[u] = struct{}{}
				}
			}
			if shouldInspectDebContents(safePath, payload.Size) && !truncated {
				for _, u := range URLsFromText(bytesToString(captured)) {
					candidates[u] = struct{}{}
				}
				result.AptCandidates = append(result.AptCandidates, parseAptSources(captured, safePath)...)
			}
			if desktopEntry && !truncated {
				collectDesktopIconReferences(captured, desktopIconReferences)
				result.Install.DesktopEntries = append(result.Install.DesktopEntries, DesktopEntry{
					ID:                     fileStem(safePath),
					Enabled:                true,
					SourcePath:             safePath,
					Destination:            "/usr/share/applications/" + lastPathComponent(safePath),
					Contents:               bytesToString(captured),
					SourceSHA256:           sum,
					OriginalContentsSHA256: sum,
					Provenance:             deterministicProvenance("", "Desktop entry detected in the inspected Debian payload"),
				})
			}
			if iconCandidate && !truncated {
				maybeKeepIcon(&iconCandidates, &capturedIconBytes, safePath, captured)
			}
			if strings.HasPrefix(safePath, "usr/share/keyrings/") && !truncated && len(captured) > 0 {
				result.SigningKeys = append(result.SigningKeys, ExtractedSigningKey{
					Contents:          append([]byte(nil), captured...),
					SourcePath:        safePath,
					SourceFingerprint: sum,
				})
			}
		} else {
			_, _ = io.Copy(io.Discard, body)
		}
		result.Payload = append(result.Payload, payload)
		return nil
	}); err != nil {
		if strings.Contains(err.Error(), "Unsafe") || strings.Contains(err.Error(), "exceeds") {
			return Analysis{}, err
		}
		return Analysis{}, fmt.Errorf("Could not read data archive: %w", err)
	}

	for i := range result.Install.Launchers {
		launcher := &result.Install.Launchers[i]
		if launcher.Enabled || !isDirectOptApplication(launcher.SourcePath) {
			continue
		}
		source := "/" + launcher.SourcePath
		destination := launcher.Destination
		if destination == "" {
			destination = "/usr/bin/" + launcher.CommandName
		}
		declared := false
		for _, script := range result.MaintainerScripts {
			if strings.Contains(script.Contents, source) &&
				strings.Contains(script.Contents, destination) &&
				launcherOperation.MatchString(script.Contents) {
				declared = true
				break
			}
		}
		if !declared {
			continue
		}
		launcher.Enabled = true
		launcher.Provenance.Rationale = "The Debian maintainer script explicitly exposes this inspected /opt executable at " + destination
	}

	if selected := selectBestIcon(iconCandidates, desktopIconReferences, result.Metadata.Package); selected != nil {
		applySelectedIcon(&result, selected, "Best matching icon selected from inspected Debian payload and desktop references")
	}
	for candidate := range candidates {
		result.UpdateCandidates = append(result.UpdateCandidates, candidate)
	}
	sort.Strings(result.UpdateCandidates)
	return result, nil
}

func writeMember(path string, r io.Reader) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(file, r)
	closeErr := file.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}
