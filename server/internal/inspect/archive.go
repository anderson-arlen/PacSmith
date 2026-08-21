package inspect

import (
	"fmt"
	"io"
	"sort"
	"strings"
)

func analyzeArchive(path string, archPackage bool) (Analysis, error) {
	result, err := walkArchiveStream(path, archPackage, func(fn func(walkedEntry, io.Reader) error) error {
		if tarErr := walkTarFile(path, path, fn); tarErr == nil {
			return nil
		} else {
			if zipErr := walkZipFile(path, fn); zipErr == nil {
				return nil
			}
			return tarErr
		}
	})
	return result, err
}

func walkArchiveStream(path string, archPackage bool, walk func(func(walkedEntry, io.Reader) error) error) (Analysis, error) {
	var result Analysis
	if archPackage {
		result.Type = SourceArchPackage
	} else {
		result.Type = SourceArchive
	}
	var pkginfo []byte
	var installScript []byte
	paths := map[string]struct{}{}
	recognizedRoot := false
	desktopIconReferences := map[string]struct{}{}
	rulePaths := map[string]struct{}{}
	var iconCandidates []iconCandidate
	capturedIconBytes := 0

	err := walk(func(entry walkedEntry, body io.Reader) error {
		normalized, ok := NormalizedArchivePath(entry.RawName)
		if !ok || normalized == "" {
			return fmt.Errorf("Unsafe archive path: %s", entry.RawName)
		}
		if _, exists := paths[normalized]; exists {
			return fmt.Errorf("Duplicate archive path: %s", normalized)
		}
		paths[normalized] = struct{}{}
		if isSpecialKind(entry.Kind) {
			return fmt.Errorf("Special device entry is not permitted: %s", normalized)
		}
		if entry.HardlinkTarget != "" {
			if _, hardOK := NormalizedArchivePath(entry.HardlinkTarget); !hardOK {
				return fmt.Errorf("Unsafe hard link in %s", normalized)
			}
		}
		if normalized == ".PKGINFO" {
			data, err := readLimited(body, maxPKGINFOBytes)
			if err != nil {
				return err
			}
			if len(data) == 0 && entry.Size > 0 {
				return fmt.Errorf("Could not read .PKGINFO")
			}
			pkginfo = data
			return nil
		}
		if normalized == ".INSTALL" {
			data, err := readLimited(body, maxInstallBytes)
			if err != nil {
				return err
			}
			if len(data) == 0 && entry.Size > 0 {
				return fmt.Errorf("Could not read .INSTALL")
			}
			installScript = data
			return nil
		}
		if strings.HasPrefix(normalized, "usr/") || strings.HasPrefix(normalized, "etc/") ||
			strings.HasPrefix(normalized, "opt/") || strings.HasPrefix(normalized, "var/") {
			recognizedRoot = true
		}
		payload := PayloadEntry{
			Path: normalized,
			Size: entry.Size,
			Type: archiveTypeName(entry.Kind),
		}
		if payload.Size < 0 {
			payload.Size = 0
		}
		if entry.Kind == kindSymlink {
			payload.SymlinkTarget = entry.SymlinkTarget
		}
		payload.ReviewReason = ReviewReason(payload.Path)
		if payload.SymlinkTarget != "" {
			payload.ReviewReason = appendReason(payload.ReviewReason, SymlinkReviewReason(payload.Path, payload.SymlinkTarget))
		}
		payload.RequiresReview = payload.ReviewReason != "" && payload.Type != "directory"
		if hasSetID(entry.Mode) {
			payload.ReviewReason = appendReason(payload.ReviewReason, "Set-user-ID or set-group-ID permission requires review")
			payload.RequiresReview = true
		}
		payload.Executable = payload.Type == "file" && hasExecBit(entry.Mode) && !looksLikeLibrary(payload.Path)
		result.Payload = append(result.Payload, payload)
		appendReviewRule(&result, result.Payload[len(result.Payload)-1], rulePaths)

		reviewed := &result.Payload[len(result.Payload)-1]
		switch {
		case reviewed.RequiresReview && reviewed.Type == "file":
			captured, sum, truncated, err := inspectBody(body, maxPreviewBytes, maxReviewFile)
			if err != nil {
				if strings.Contains(err.Error(), "exceeds") {
					return fmt.Errorf("Review-sensitive archive entry exceeds 64 MiB: %s", reviewed.Path)
				}
				return err
			}
			reviewed.ContentSHA256 = sum
			reviewed.PreviewTruncated = truncated
			reviewed.TextPreview = utf8Preview(captured)
			if looksLikeExecutableMagic(captured) {
				reviewed.Executable = true
			}
			if reviewed.TextPreview != "" && !reviewed.PreviewTruncated {
				evidence := analyzeScriptEvidence([]MaintainerScript{{
					Name:     "payload/" + reviewed.Path,
					Contents: reviewed.TextPreview,
				}})
				result.RPMCandidates = append(result.RPMCandidates, evidence.RPMCandidates...)
				result.AptCandidates = append(result.AptCandidates, evidence.AptCandidates...)
				result.SigningKeys = append(result.SigningKeys, evidence.SigningKeys...)
				result.UpdateCandidates = append(result.UpdateCandidates, URLsFromText(reviewed.TextPreview)...)
			}
			if shouldInspectRepositoryContents(reviewed.Path, reviewed.Size) && !reviewed.PreviewTruncated {
				result.AptCandidates = append(result.AptCandidates, parseAptSources(captured, reviewed.Path)...)
			}
			if isRepositoryKeyPath(reviewed.Path) && !reviewed.PreviewTruncated && len(captured) > 0 {
				result.SigningKeys = append(result.SigningKeys, ExtractedSigningKey{
					Contents:          append([]byte(nil), captured...),
					SourcePath:        reviewed.Path,
					SourceFingerprint: reviewed.ContentSHA256,
				})
			}
		case reviewed.Type == "file" && (isDesktopEntry(reviewed.Path, reviewed.Size) || isAnyIconCandidate(reviewed.Path, reviewed.Size)):
			contents, err := readLimited(body, maxIconBytes)
			if err != nil {
				return err
			}
			if len(contents) == 0 && reviewed.Size > 0 {
				return fmt.Errorf("Could not read archive member %s", reviewed.Path)
			}
			if isDesktopEntry(reviewed.Path, reviewed.Size) {
				collectDesktopIconReferences(contents, desktopIconReferences)
				result.Install.DesktopEntries = append(result.Install.DesktopEntries, DesktopEntry{
					ID:                     fileStem(reviewed.Path),
					Enabled:                true,
					SourcePath:             reviewed.Path,
					Destination:            "/usr/share/applications/" + lastPathComponent(reviewed.Path),
					Contents:               bytesToString(contents),
					SourceSHA256:           sha256Hex(contents),
					OriginalContentsSHA256: sha256Hex(contents),
					Provenance:             deterministicProvenance("", "Desktop entry detected in the inspected payload"),
				})
			}
			if isAnyIconCandidate(reviewed.Path, reviewed.Size) {
				maybeKeepIcon(&iconCandidates, &capturedIconBytes, reviewed.Path, contents)
			}
		case shouldPeekExecutable(*reviewed):
			header, err := peekPrefix(body, 64)
			if err != nil {
				return err
			}
			if looksLikeExecutableMagic(header) {
				reviewed.Executable = true
			}
		default:
			_, _ = io.Copy(io.Discard, body)
		}
		return nil
	})
	if err != nil {
		return Analysis{}, err
	}

	if archPackage {
		if len(pkginfo) == 0 {
			return Analysis{}, fmt.Errorf("Arch package does not contain .PKGINFO")
		}
		result.Metadata.Package = firstField(pkginfo, "pkgname")
		fullVersion := firstField(pkginfo, "pkgver")
		dash := strings.LastIndex(fullVersion, "-")
		if dash > 0 {
			result.Metadata.Version = fullVersion[:dash]
			result.UpstreamArchPkgrel = fullVersion[dash+1:]
		} else {
			result.Metadata.Version = fullVersion
			result.UpstreamArchPkgrel = "1"
		}
		result.Metadata.Architecture = firstField(pkginfo, "arch")
		result.Metadata.Description = firstField(pkginfo, "pkgdesc")
		result.Metadata.Homepage = firstField(pkginfo, "url")
		result.Metadata.Maintainer = firstField(pkginfo, "packager")
		result.Metadata.Depends = strings.Join(pkginfoFields(pkginfo, "depend"), ", ")
		result.Metadata.Conflicts = strings.Join(pkginfoFields(pkginfo, "conflict"), ", ")
		result.Metadata.Provides = strings.Join(pkginfoFields(pkginfo, "provides"), ", ")
		result.Metadata.RawFields = map[string]string{}
		for _, line := range strings.Split(string(pkginfo), "\n") {
			separator := strings.Index(line, " = ")
			if separator <= 0 {
				continue
			}
			key := line[:separator]
			value := line[separator+3:]
			if stored := result.Metadata.RawFields[key]; stored != "" {
				result.Metadata.RawFields[key] = stored + "\n" + value
			} else {
				result.Metadata.RawFields[key] = value
			}
		}
		for _, depend := range pkginfoFields(pkginfo, "depend") {
			name := dependencyName(depend)
			if name == "" {
				continue
			}
			result.Dependencies = append(result.Dependencies, Dependency{
				RawExpression: depend,
				ArchPackage:   name,
				Status:        MappingResolved,
				MappingSource: "upstream Arch package metadata",
				Confidence:    1.0,
			})
		}
		if len(installScript) > 0 {
			script := MaintainerScript{Name: ".INSTALL", Contents: bytesToString(installScript)}
			result.MaintainerScripts = append(result.MaintainerScripts, script)
			result.ScriptFindings = append(result.ScriptFindings, ScriptFinding{
				ScriptName:          script.Name,
				Kind:                "arch-install-script",
				Summary:             "Upstream Arch lifecycle script requires review before translation or reuse.",
				EvidenceFingerprint: scriptContentFingerprint(script),
				Disposition:         DispositionUnresolved,
			})
		}
		result.Install.ArchiveLayout = LayoutPreserveRoot
	} else {
		inferNameVersion(path, &result.Metadata)
		if recognizedRoot {
			result.Install.ArchiveLayout = LayoutPreserveRoot
		} else {
			result.Install.ArchiveLayout = LayoutOptBundle
		}
		result.Install.OptDirectory = result.Metadata.Package
		commonPrefix := ""
		sharedPrefix := len(result.Payload) > 0
		for _, payload := range result.Payload {
			slash := strings.IndexByte(payload.Path, '/')
			prefix := payload.Path
			if slash >= 0 {
				prefix = payload.Path[:slash]
			}
			if commonPrefix == "" {
				commonPrefix = prefix
			} else if prefix != commonPrefix {
				sharedPrefix = false
				break
			}
		}
		if sharedPrefix && commonPrefix != "" {
			result.Install.CommonPrefix = commonPrefix
			result.Install.StripCommonPrefix = result.Install.ArchiveLayout == LayoutOptBundle
		}
		InferArchiveLaunchers(&result.Install, result.Payload, result.Metadata.Package)
	}
	if selected := selectBestIcon(iconCandidates, desktopIconReferences, result.Metadata.Package); selected != nil {
		applySelectedIcon(&result, selected, "Best matching icon selected from inspected payload and desktop references")
	}
	if !archPackage && result.Install.ArchiveLayout == LayoutOptBundle {
		command := ""
		if len(result.Install.Launchers) > 0 {
			command = result.Install.Launchers[0].CommandName
		}
		iconName := result.Install.Icon.IconName
		for i := range result.Install.DesktopEntries {
			result.Install.DesktopEntries[i].Contents = normalizedDesktopContents(
				result.Install.DesktopEntries[i].Contents, command, iconName)
		}
	}
	return result, nil
}

func appendReviewRule(result *Analysis, entry PayloadEntry, rulePaths map[string]struct{}) {
	if !entry.RequiresReview {
		return
	}
	excluded := IsForeignPackageManagerPath(entry.Path) ||
		(entry.SymlinkTarget != "" && !SafePackageSymlinkTarget(entry.Path, entry.SymlinkTarget))
	path := entry.Path
	if excluded {
		path = exclusionRoot(entry.Path)
	}
	if _, exists := rulePaths[path]; exists {
		return
	}
	result.PayloadRules = append(result.PayloadRules, PayloadRule{
		Path:     path,
		Reason:   entry.ReviewReason,
		Excluded: excluded,
	})
	rulePaths[path] = struct{}{}
}

func pkginfoFields(contents []byte, name string) []string {
	prefix := []byte(name + " = ")
	var result []string
	for _, line := range strings.Split(string(contents), "\n") {
		if strings.HasPrefix(line, string(prefix)) {
			result = append(result, strings.TrimSpace(line[len(prefix):]))
		}
	}
	return result
}

func firstField(contents []byte, name string) string {
	values := pkginfoFields(contents, name)
	if len(values) == 0 {
		return ""
	}
	return values[0]
}

func deduplicateEvidence(result *Analysis) {
	var apt []AptRepositoryCandidate
	aptSeen := map[string]struct{}{}
	for _, candidate := range result.AptCandidates {
		identity := aptDisplayText(candidate) + "\n" + candidate.SourcePath
		if _, ok := aptSeen[identity]; ok {
			continue
		}
		aptSeen[identity] = struct{}{}
		apt = append(apt, candidate)
	}
	result.AptCandidates = apt

	var rpm []RPMRepositoryCandidate
	rpmSeen := map[string]struct{}{}
	for _, candidate := range result.RPMCandidates {
		identity := candidate.BaseURL + "\n" + candidate.Architecture
		if _, ok := rpmSeen[identity]; ok {
			continue
		}
		rpmSeen[identity] = struct{}{}
		rpm = append(rpm, candidate)
	}
	result.RPMCandidates = rpm

	var keys []ExtractedSigningKey
	keySeen := map[string]struct{}{}
	for _, key := range result.SigningKeys {
		identity := sha256Hex(key.Contents)
		if _, ok := keySeen[identity]; ok {
			continue
		}
		keySeen[identity] = struct{}{}
		keys = append(keys, key)
	}
	result.SigningKeys = keys

	result.UpdateCandidates = uniqueStrings(result.UpdateCandidates)
	sort.Strings(result.UpdateCandidates)
}
