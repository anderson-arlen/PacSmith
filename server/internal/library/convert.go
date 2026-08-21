package library

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/recipe"
)

func sourceTypeName(t inspect.SourceType) string {
	switch t {
	case inspect.SourceDebian:
		return "deb"
	case inspect.SourceRPM:
		return "rpm"
	case inspect.SourceArchPackage:
		return "arch-package"
	case inspect.SourceArchive:
		return "archive"
	case inspect.SourceAppImage:
		return "appimage"
	case inspect.SourceELF:
		return "elf-binary"
	default:
		return "not-inspected"
	}
}

func recipeSourceType(t inspect.SourceType) recipe.SourceType {
	switch t {
	case inspect.SourceDebian:
		return recipe.SourceDebian
	case inspect.SourceRPM:
		return recipe.SourceRPM
	case inspect.SourceArchPackage:
		return recipe.SourceArchPackage
	case inspect.SourceArchive:
		return recipe.SourceArchive
	case inspect.SourceAppImage:
		return recipe.SourceAppImage
	case inspect.SourceELF:
		return recipe.SourceELF
	default:
		return recipe.SourceDebian
	}
}

func mappingStatusName(status inspect.MappingStatus) string {
	switch status {
	case inspect.MappingResolved:
		return "Resolved"
	case inspect.MappingIgnored:
		return "Ignored"
	case inspect.MappingBundled:
		return "Bundled"
	case inspect.MappingProvided:
		return "Provided"
	default:
		return "Unresolved"
	}
}

func recipeFromAnalysis(projectID, releaseID, filename, sha256 string, analysis inspect.Analysis) recipe.Release {
	rel := recipe.Release{
		ID:                     releaseID,
		ProjectID:              projectID,
		DisplayName:            preferredName(analysis),
		ArchPackageName:        recipe.SanitizePackageName(analysis.Metadata.Package) + "-bin",
		SourceType:             recipeSourceType(analysis.Type),
		OriginalSourceFilename: filename,
		SourceSHA256:           sha256,
		ArchPkgrel:             1,
		Debian: recipe.DebianMetadata{
			Package:      analysis.Metadata.Package,
			Version:      analysis.Metadata.Version,
			Architecture: analysis.Metadata.Architecture,
			Description:  analysis.Metadata.Description,
			Homepage:     analysis.Metadata.Homepage,
		},
		Acquisition: recipe.Acquisition{
			Kind:              recipe.AcquisitionLocalFile,
			CanonicalIdentity: "local:" + analysis.Metadata.Package,
		},
	}
	if rel.ArchPackageName == "-bin" {
		rel.ArchPackageName = "vendor-package-bin"
	}
	if analysis.Install.OptDirectory == "" {
		analysis.Install.OptDirectory = strings.TrimSuffix(rel.ArchPackageName, "-bin")
	}
	rel.InstallMapping = convertInstall(analysis.Install)
	for _, dep := range analysis.Dependencies {
		rel.Dependencies = append(rel.Dependencies, recipe.Dependency{
			ArchPackage: dep.ArchPackage,
			Status:      recipe.MappingStatus(mappingStatusName(dep.Status)),
			Ignored:     dep.Ignored,
			Bundled:     dep.Bundled,
			Provided:    dep.Provided,
		})
	}
	for _, rule := range analysis.PayloadRules {
		rel.PayloadRules = append(rel.PayloadRules, recipe.PayloadRule{
			Path:     rule.Path,
			Excluded: rule.Excluded,
		})
	}
	return rel
}

func convertInstall(in inspect.InstallMapping) recipe.InstallMapping {
	out := recipe.InstallMapping{
		ArchiveLayout:     recipe.ArchiveOptBundle,
		OptDirectory:      in.OptDirectory,
		CommonPrefix:      in.CommonPrefix,
		StripCommonPrefix: in.StripCommonPrefix,
		AppImageOffset:    in.AppImageOffset,
		BinaryDestination: in.BinaryDestination,
		Icon: recipe.Icon{
			ProjectPath: in.Icon.ProjectPath,
			SourcePath:  in.Icon.SourcePath,
			SHA256:      in.Icon.SHA256,
			Format:      in.Icon.Format,
			IconName:    in.Icon.IconName,
		},
		AppRun: recipe.AppRun{
			Script:           in.AppRun.Script,
			Contents:         in.AppRun.Contents,
			OriginalContents: in.AppRun.OriginalContents,
			UserModified:     in.AppRun.UserModified,
		},
	}
	if in.ArchiveLayout == inspect.LayoutPreserveRoot {
		out.ArchiveLayout = recipe.ArchivePreserveRoot
	}
	for _, launcher := range in.Launchers {
		kind := recipe.LauncherSymlink
		if launcher.Kind == inspect.LauncherWrapper {
			kind = recipe.LauncherWrapper
		}
		out.Launchers = append(out.Launchers, recipe.Launcher{
			Enabled:     true,
			SourcePath:  launcher.SourcePath,
			CommandName: launcher.CommandName,
			Destination: launcher.Destination,
			Kind:        kind,
			Missing:     launcher.Missing,
		})
		if !launcher.Enabled {
			out.Launchers[len(out.Launchers)-1].Enabled = false
		}
	}
	for _, desktop := range in.DesktopEntries {
		out.DesktopEntries = append(out.DesktopEntries, recipe.DesktopEntry{
			ID:          desktop.ID,
			Enabled:     true,
			SourcePath:  desktop.SourcePath,
			Destination: desktop.Destination,
			Contents:    desktop.Contents,
		})
		if desktop.ID != "" && !desktop.Enabled {
			out.DesktopEntries[len(out.DesktopEntries)-1].Enabled = false
		}
	}
	return out
}

func preferredName(analysis inspect.Analysis) string {
	for _, desktop := range analysis.Install.DesktopEntries {
		if name := desktopField(desktop.Contents, "Name"); name != "" &&
			!strings.Contains(strings.ToLower(name), "\n") {
			return name
		}
	}
	if analysis.Metadata.Package != "" {
		return analysis.Metadata.Package
	}
	return "vendor-package"
}

func desktopField(contents, key string) string {
	for _, line := range strings.Split(contents, "\n") {
		if strings.HasPrefix(line, key+"=") {
			return strings.TrimSpace(strings.TrimPrefix(line, key+"="))
		}
	}
	return ""
}

func analysisDocument(filename, sha256, pkgbuild string, analysis inspect.Analysis) (string, error) {
	payload := make([]map[string]any, 0, len(analysis.Payload))
	for _, entry := range analysis.Payload {
		payload = append(payload, map[string]any{
			"path":             entry.Path,
			"type":             entry.Type,
			"symlinkTarget":    entry.SymlinkTarget,
			"size":             entry.Size,
			"requiresReview":   entry.RequiresReview,
			"reviewReason":     entry.ReviewReason,
			"contentSha256":    entry.ContentSHA256,
			"textPreview":      entry.TextPreview,
			"previewTruncated": entry.PreviewTruncated,
			"executable":       entry.Executable,
		})
	}
	dependencies := make([]map[string]any, 0, len(analysis.Dependencies))
	for _, dep := range analysis.Dependencies {
		alternatives := make([]map[string]any, 0, len(dep.Alternatives))
		for _, alt := range dep.Alternatives {
			alternatives = append(alternatives, map[string]any{
				"packageName":     alt.PackageName,
				"versionOperator": alt.VersionOperator,
				"version":         alt.Version,
			})
		}
		dependencies = append(dependencies, map[string]any{
			"rawExpression": dep.RawExpression,
			"alternatives":  alternatives,
			"archPackage":   dep.ArchPackage,
			"status":        mappingStatusName(dep.Status),
			"mappingSource": dep.MappingSource,
			"confidence":    dep.Confidence,
			"userOverride":  dep.UserOverride,
			"ignored":       dep.Ignored,
			"bundled":       dep.Bundled,
			"provided":      dep.Provided,
		})
	}
	scripts := make([]map[string]any, 0, len(analysis.MaintainerScripts))
	for _, script := range analysis.MaintainerScripts {
		scripts = append(scripts, map[string]any{
			"name":                    script.Name,
			"contents":                script.Contents,
			"acknowledgedFingerprint": script.AcknowledgedFingerprint,
		})
	}
	findings := scriptFindingsJSON(analysis.ScriptFindings)
	rules := make([]map[string]any, 0, len(analysis.PayloadRules))
	for _, rule := range analysis.PayloadRules {
		rules = append(rules, map[string]any{
			"path":                    rule.Path,
			"excluded":                rule.Excluded,
			"reason":                  rule.Reason,
			"userDecision":            rule.UserDecision,
			"acknowledgedFingerprint": rule.AcknowledgedFingerprint,
		})
	}
	document := map[string]any{
		"originalSourceFilename":   filename,
		"sourceSha256":             sha256,
		"sourceType":               sourceTypeName(analysis.Type),
		"debian":                   debianMetadataJSON(analysis.Metadata),
		"dependencies":             dependencies,
		"maintainerScripts":        scripts,
		"scriptFindings":           findings,
		"payload":                  payload,
		"payloadRules":             rules,
		"installMapping":           installMappingJSON(analysis.Install),
		"generatedPkgbuild":        pkgbuild,
		"pkgbuildManuallyModified": false,
		"update":                   updateConfigurationJSON(analysis),
		"identityVariables":        recipe.IdentityVariables(recipeFromAnalysis("", "", filename, sha256, analysis)),
	}
	raw, err := json.Marshal(document)
	if err != nil {
		return "", err
	}
	return string(raw), nil
}

func installMappingJSON(in inspect.InstallMapping) map[string]any {
	layout := "opt-bundle"
	if in.ArchiveLayout == inspect.LayoutPreserveRoot {
		layout = "preserve-root"
	}
	launchers := make([]map[string]any, 0, len(in.Launchers))
	for _, launcher := range in.Launchers {
		kind := "symlink"
		if launcher.Kind == inspect.LauncherWrapper {
			kind = "wrapper"
		}
		launchers = append(launchers, map[string]any{
			"enabled":           launcher.Enabled,
			"sourcePath":        launcher.SourcePath,
			"commandName":       launcher.CommandName,
			"destination":       launcher.Destination,
			"kind":              kind,
			"sourceFingerprint": launcher.SourceFingerprint,
			"missing":           launcher.Missing,
			"provenance":        provenanceJSON(launcher.Provenance),
		})
	}
	desktops := make([]map[string]any, 0, len(in.DesktopEntries))
	for _, desktop := range in.DesktopEntries {
		desktops = append(desktops, map[string]any{
			"id":                     desktop.ID,
			"enabled":                desktop.Enabled,
			"sourcePath":             desktop.SourcePath,
			"destination":            desktop.Destination,
			"contents":               desktop.Contents,
			"sourceSha256":           desktop.SourceSHA256,
			"originalContentsSha256": desktop.OriginalContentsSHA256,
			"generated":              desktop.Generated,
			"userModified":           desktop.UserModified,
			"missing":                desktop.Missing,
			"provenance":             provenanceJSON(desktop.Provenance),
		})
	}
	links := make([]string, 0, len(in.ExecutableLinks))
	links = append(links, in.ExecutableLinks...)
	return map[string]any{
		"archiveLayout":     layout,
		"optDirectory":      in.OptDirectory,
		"commonPrefix":      in.CommonPrefix,
		"stripCommonPrefix": in.StripCommonPrefix,
		"appImageOffset":    strconv.FormatInt(in.AppImageOffset, 10),
		"binarySourcePath":  in.BinarySourcePath,
		"binaryDestination": in.BinaryDestination,
		"executableLinks":   links,
		"launchers":         launchers,
		"desktopEntries":    desktops,
		"icon": map[string]any{
			"sourceKind":  iconSourceKindName(in.Icon.SourceKind),
			"sourcePath":  in.Icon.SourcePath,
			"sourceUrl":   in.Icon.SourceURL,
			"projectPath": in.Icon.ProjectPath,
			"sha256":      in.Icon.SHA256,
			"format":      in.Icon.Format,
			"iconName":    in.Icon.IconName,
			"missing":     in.Icon.Missing,
			"provenance":  provenanceJSON(in.Icon.Provenance),
		},
		"appRun": map[string]any{
			"present":                 in.AppRun.Present,
			"script":                  in.AppRun.Script,
			"contents":                in.AppRun.Contents,
			"originalContents":        in.AppRun.OriginalContents,
			"originalContentsSha256":  in.AppRun.OriginalContentsSHA256,
			"acknowledgedFingerprint": in.AppRun.AcknowledgedFingerprint,
			"userModified":            in.AppRun.UserModified,
			"reviewReason":            in.AppRun.ReviewReason,
			"provenance":              provenanceJSON(in.AppRun.Provenance),
		},
	}
}

// identityVariablesFor rebuilds pacsmith.vars from the current release document.
// The GUI PUT omits identityVariables, so this field cannot be treated as stored
// source of truth.
func identityVariablesFor(rel Release) string {
	return recipe.IdentityVariables(recipeFromDocument(rel))
}

func recipeFromDocument(rel Release) recipe.Release {
	doc := rel.Document
	if doc == nil {
		doc = map[string]any{}
	}
	debian, _ := mapValue(doc, "debian")
	acquisition, _ := mapValue(doc, "acquisition")
	install, _ := mapValue(doc, "installMapping")
	icon, _ := mapValue(install, "icon")
	appRun, _ := mapValue(install, "appRun")
	lifecycle, _ := mapValue(doc, "lifecycleScript")

	out := recipe.Release{
		ID:                     firstNonEmpty(stringValue(doc, "id"), rel.ID),
		ProjectID:              firstNonEmpty(stringValue(doc, "projectId"), rel.ProjectID),
		DisplayName:            stringValue(doc, "displayName"),
		ArchPackageName:        firstNonEmpty(stringValue(doc, "archPackageName"), rel.ArchPackageName),
		SourceType:             recipe.SourceType(firstNonEmpty(stringValue(doc, "sourceType"), rel.SourceType)),
		OriginalSourceFilename: stringValue(doc, "originalSourceFilename"),
		SourceSHA256:           firstNonEmpty(stringValue(doc, "sourceSha256"), rel.SourceSHA256),
		ArchPkgrel:             intValue(doc, "archPkgrel", 1),
		ArchPkgrelOverride:     stringValue(doc, "archPkgrelOverride"),
		Debian: recipe.DebianMetadata{
			Package:      stringValue(debian, "package"),
			Version:      firstNonEmpty(stringValue(debian, "version"), rel.VendorVersion),
			Architecture: stringValue(debian, "architecture"),
			Description:  stringValue(debian, "description"),
			Homepage:     stringValue(debian, "homepage"),
		},
		Acquisition: recipe.Acquisition{
			Kind:              recipe.AcquisitionKind(stringValue(acquisition, "kind")),
			CanonicalIdentity: stringValue(acquisition, "canonicalIdentity"),
		},
		InstallMapping: recipe.InstallMapping{
			ArchiveLayout:     recipeArchiveLayout(stringValue(install, "archiveLayout")),
			OptDirectory:      stringValue(install, "optDirectory"),
			CommonPrefix:      stringValue(install, "commonPrefix"),
			StripCommonPrefix: boolValue(install, "stripCommonPrefix"),
			AppImageOffset:    int64Value(install, "appImageOffset"),
			BinaryDestination: stringValue(install, "binaryDestination"),
			Launchers:         recipeLaunchers(install["launchers"]),
			DesktopEntries:    recipeDesktops(install["desktopEntries"]),
			Icon: recipe.Icon{
				ProjectPath: stringValue(icon, "projectPath"),
				SourcePath:  stringValue(icon, "sourcePath"),
				SHA256:      stringValue(icon, "sha256"),
				Format:      stringValue(icon, "format"),
				IconName:    stringValue(icon, "iconName"),
				Missing:     boolValue(icon, "missing"),
			},
			AppRun: recipe.AppRun{
				Script:           boolValue(appRun, "script"),
				Contents:         stringValue(appRun, "contents"),
				OriginalContents: stringValue(appRun, "originalContents"),
				UserModified:     boolValue(appRun, "userModified"),
			},
		},
		Lifecycle: recipe.LifecycleScript{
			FileName:         stringValue(lifecycle, "fileName"),
			Contents:         stringValue(lifecycle, "contents"),
			ValidationPassed: boolValue(lifecycle, "validationPassed"),
		},
	}
	for _, dep := range objectSlice(doc["dependencies"]) {
		out.Dependencies = append(out.Dependencies, recipe.Dependency{
			ArchPackage: stringValue(dep, "archPackage"),
			Status:      recipe.MappingStatus(stringValue(dep, "status")),
			Ignored:     boolValue(dep, "ignored"),
			Bundled:     boolValue(dep, "bundled"),
			Provided:    boolValue(dep, "provided"),
		})
	}
	for _, rule := range objectSlice(doc["payloadRules"]) {
		out.PayloadRules = append(out.PayloadRules, recipe.PayloadRule{
			Path:     stringValue(rule, "path"),
			Excluded: boolValue(rule, "excluded"),
		})
	}
	return out
}

func updateConfigurationJSON(analysis inspect.Analysis) map[string]any {
	update := map[string]any{
		"strategy":           "Manual",
		"detectedCandidates": append([]string(nil), analysis.UpdateCandidates...),
		"aptCandidates":      aptCandidatesJSON(analysis.AptCandidates),
		"rpmCandidates":      rpmCandidatesJSON(analysis.RPMCandidates),
	}
	populateUpdateFromCandidates(update, nil, analysis.Metadata.Package, analysis.Metadata.Architecture, "")
	return update
}

func aptCandidatesJSON(candidates []inspect.AptRepositoryCandidate) []map[string]any {
	out := make([]map[string]any, 0, len(candidates))
	for _, candidate := range candidates {
		out = append(out, map[string]any{
			"uri":           candidate.URI,
			"suite":         candidate.Suite,
			"components":    nonNilStrings(candidate.Components),
			"architectures": nonNilStrings(candidate.Architectures),
			"signedBy":      candidate.SignedBy,
			"sourcePath":    candidate.SourcePath,
		})
	}
	return out
}

func rpmCandidatesJSON(candidates []inspect.RPMRepositoryCandidate) []map[string]any {
	out := make([]map[string]any, 0, len(candidates))
	for _, candidate := range candidates {
		out = append(out, map[string]any{
			"baseUrl":      candidate.BaseURL,
			"architecture": candidate.Architecture,
			"keyUrls":      nonNilStrings(candidate.KeyURLs),
			"sourcePath":   candidate.SourcePath,
		})
	}
	return out
}

func nonNilStrings(values []string) []string {
	if values == nil {
		return []string{}
	}
	return values
}

func attachInspectedRelease(document map[string]any) {
	attachUpdateConfiguration(document)
	attachScriptFindings(document)
	attachSigningKeys(document)
}

func attachUpdateConfiguration(document map[string]any) {
	if document == nil {
		return
	}
	update, ok := mapValue(document, "update")
	if !ok {
		update = map[string]any{"strategy": "Manual"}
		document["update"] = update
	}
	acquisition, _ := mapValue(document, "acquisition")
	acquisitionKind := stringValue(acquisition, "kind")
	debian, _ := mapValue(document, "debian")
	if jsonSliceLen(update["aptCandidates"]) == 0 || jsonSliceLen(update["rpmCandidates"]) == 0 {
		scripts := maintainerScriptsFromDocument(document)
		apt, rpm, _ := inspect.ScriptEvidence(scripts)
		if jsonSliceLen(update["aptCandidates"]) == 0 && len(apt) > 0 {
			update["aptCandidates"] = aptCandidatesJSON(apt)
		}
		if jsonSliceLen(update["rpmCandidates"]) == 0 && len(rpm) > 0 {
			update["rpmCandidates"] = rpmCandidatesJSON(rpm)
		}
		if jsonSliceLen(update["detectedCandidates"]) == 0 {
			var urls []string
			for _, script := range scripts {
				urls = append(urls, inspect.URLsFromText(script.Contents)...)
			}
			update["detectedCandidates"] = urls
		}
	}
	populateUpdateFromCandidates(update, acquisition, stringValue(debian, "package"),
		stringValue(debian, "architecture"), acquisitionKind)
	stampUpdateProvenance(document, update)
}

func populateUpdateFromCandidates(update, acquisition map[string]any, packageName, architecture, acquisitionKind string) {
	if update == nil {
		return
	}
	if acquisitionKind == "" && acquisition != nil {
		acquisitionKind = stringValue(acquisition, "kind")
	}
	if acquisitionKind == "github-release" {
		update["strategy"] = "GitHub releases"
		copyGitHubUpdateFields(update, acquisition)
		return
	}
	if acquisitionKind == "direct-url" && strings.TrimSpace(stringValue(update, "url")) == "" && acquisition != nil {
		if original := strings.TrimSpace(stringValue(acquisition, "originalUrl")); original != "" {
			if isManualStrategy(stringValue(update, "strategy")) {
				update["strategy"] = "Direct URL"
			}
			update["url"] = original
		}
	}
	strategy := stringValue(update, "strategy")
	url := strings.TrimSpace(stringValue(update, "url"))
	if !isManualStrategy(strategy) && url != "" {
		return
	}
	if uri, suite, component, arch, ok := firstAptCandidate(update["aptCandidates"]); ok {
		if isManualStrategy(strategy) || strategy == "" {
			update["strategy"] = "APT repository"
		}
		if url == "" {
			update["url"] = uri
		}
		if stringValue(update, "aptSuite") == "" {
			update["aptSuite"] = suite
		}
		if stringValue(update, "aptComponent") == "" && component != "" {
			update["aptComponent"] = component
		}
		if stringValue(update, "aptArchitecture") == "" {
			if arch != "" {
				update["aptArchitecture"] = arch
			} else {
				update["aptArchitecture"] = architecture
			}
		}
		if stringValue(update, "aptPackageName") == "" {
			update["aptPackageName"] = packageName
		}
		return
	}
	if baseURL, arch, ok := firstRPMCandidate(update["rpmCandidates"]); ok {
		if isManualStrategy(strategy) || strategy == "" {
			update["strategy"] = "RPM repository"
		}
		if url == "" {
			update["url"] = baseURL
		}
		if stringValue(update, "rpmArchitecture") == "" {
			if arch != "" {
				update["rpmArchitecture"] = arch
			} else {
				update["rpmArchitecture"] = architecture
			}
		}
		if stringValue(update, "rpmPackageName") == "" {
			update["rpmPackageName"] = packageName
		}
	}
}

func copyGitHubUpdateFields(update, acquisition map[string]any) {
	if update == nil {
		return
	}
	if acquisition == nil {
		acquisition = map[string]any{}
	}
	setIfEmpty := func(key string, value string) {
		if strings.TrimSpace(stringValue(update, key)) == "" && strings.TrimSpace(value) != "" {
			update[key] = value
		}
	}
	owner := firstNonEmpty(stringValue(update, "githubOwner"), stringValue(acquisition, "githubOwner"))
	repo := firstNonEmpty(stringValue(update, "githubRepository"), stringValue(acquisition, "githubRepository"))
	if owner == "" || repo == "" {
		if identity := stringValue(acquisition, "canonicalIdentity"); strings.HasPrefix(identity, "github:") {
			parts := strings.SplitN(strings.TrimPrefix(identity, "github:"), "/", 2)
			if len(parts) == 2 {
				if owner == "" {
					owner = parts[0]
				}
				if repo == "" {
					repo = parts[1]
				}
			}
		}
	}
	setIfEmpty("githubOwner", owner)
	setIfEmpty("githubRepository", repo)
	setIfEmpty("url", stringValue(acquisition, "originalUrl"))
	setIfEmpty("githubTag", stringValue(acquisition, "githubTag"))
	setIfEmpty("githubPublisherDigest", firstNonEmpty(stringValue(acquisition, "githubPublisherDigest"),
		stringValue(acquisition, "publisherDigest")))
	if int64Value(update, "githubReleaseId") == 0 {
		if id := int64Value(acquisition, "githubReleaseId"); id > 0 {
			update["githubReleaseId"] = strconv.FormatInt(id, 10)
		}
	}
	if int64Value(update, "githubAssetId") == 0 {
		if id := int64Value(acquisition, "githubAssetId"); id > 0 {
			update["githubAssetId"] = strconv.FormatInt(id, 10)
		}
	}
}

func stampUpdateProvenance(document, update map[string]any) {
	if document == nil || update == nil {
		return
	}
	strategy := stringValue(update, "strategy")
	if isManualStrategy(strategy) {
		return
	}
	fingerprint := stringValue(update, "url") + "\n" + stringValue(update, "aptSuite") + "\n" +
		stringValue(update, "aptComponent")
	if fingerprint == "\n\n" {
		return
	}
	provenance, ok := mapValue(document, "fieldProvenance")
	if !ok {
		provenance = map[string]any{}
		document["fieldProvenance"] = provenance
	}
	stamp := provenanceJSON(inspect.FieldProvenance{
		Origin:            inspect.OriginDeterministic,
		SourceFingerprint: inspectFingerprint(fingerprint),
		Rationale:         "Extracted from vendor package evidence.",
	})
	fields := []string{"update.url", "update.aptSuite", "update.aptComponent",
		"update.aptArchitecture", "update.aptPackageName", "update.rpmArchitecture",
		"update.rpmPackageName"}
	if strategy == "GitHub releases" {
		fields = []string{"update.url", "update.githubOwner", "update.githubRepository",
			"update.githubAssetRegex"}
	}
	for _, field := range fields {
		if existing, ok := mapValue(provenance, field); ok && stringValue(existing, "origin") != "" &&
			stringValue(existing, "origin") != "unknown" {
			continue
		}
		if strings.TrimSpace(stringValue(update, strings.TrimPrefix(field, "update."))) == "" {
			continue
		}
		provenance[field] = stamp
	}
}

func inspectFingerprint(value string) string {
	sum := sha256.Sum256([]byte(value))
	return hex.EncodeToString(sum[:])
}

func isManualStrategy(strategy string) bool {
	strategy = strings.TrimSpace(strategy)
	return strategy == "" || strings.EqualFold(strategy, "manual")
}

func firstAptCandidate(raw any) (uri, suite, component, arch string, ok bool) {
	first, ok := firstObject(raw)
	if !ok {
		return "", "", "", "", false
	}
	uri = stringValue(first, "uri")
	suite = stringValue(first, "suite")
	if uri == "" || suite == "" {
		return "", "", "", "", false
	}
	if components := stringSlice(first["components"]); len(components) > 0 {
		component = components[0]
	}
	if architectures := stringSlice(first["architectures"]); len(architectures) > 0 {
		arch = architectures[0]
	}
	return uri, suite, component, arch, true
}

func firstRPMCandidate(raw any) (baseURL, arch string, ok bool) {
	first, ok := firstObject(raw)
	if !ok {
		return "", "", false
	}
	baseURL = stringValue(first, "baseUrl")
	if baseURL == "" {
		return "", "", false
	}
	return baseURL, stringValue(first, "architecture"), true
}

func firstObject(raw any) (map[string]any, bool) {
	switch value := raw.(type) {
	case []map[string]any:
		if len(value) == 0 {
			return nil, false
		}
		return value[0], true
	case []any:
		if len(value) == 0 {
			return nil, false
		}
		object, ok := value[0].(map[string]any)
		return object, ok
	default:
		return nil, false
	}
}

func jsonSliceLen(raw any) int {
	switch value := raw.(type) {
	case []any:
		return len(value)
	case []map[string]any:
		return len(value)
	case []string:
		return len(value)
	default:
		return 0
	}
}

func stringSlice(raw any) []string {
	switch value := raw.(type) {
	case []string:
		return value
	case []any:
		out := make([]string, 0, len(value))
		for _, item := range value {
			text, ok := item.(string)
			if ok && text != "" {
				out = append(out, text)
			}
		}
		return out
	default:
		return nil
	}
}

func maintainerScriptsFromDocument(document map[string]any) []inspect.MaintainerScript {
	scripts := make([]inspect.MaintainerScript, 0)
	for _, object := range objectSlice(document["maintainerScripts"]) {
		scripts = append(scripts, inspect.MaintainerScript{
			Name:     stringValue(object, "name"),
			Contents: stringValue(object, "contents"),
		})
	}
	return scripts
}

func mergeImportedUpdate(body map[string]any, raw json.RawMessage) {
	var incoming map[string]any
	if err := json.Unmarshal(raw, &incoming); err != nil || incoming == nil {
		return
	}
	if detected, ok := mapValue(body, "update"); ok {
		if jsonSliceLen(incoming["aptCandidates"]) == 0 {
			incoming["aptCandidates"] = detected["aptCandidates"]
		}
		if jsonSliceLen(incoming["rpmCandidates"]) == 0 {
			incoming["rpmCandidates"] = detected["rpmCandidates"]
		}
		if jsonSliceLen(incoming["detectedCandidates"]) == 0 {
			incoming["detectedCandidates"] = detected["detectedCandidates"]
		}
	}
	body["update"] = incoming
}

func iconSourceKindName(kind inspect.IconSourceKind) string {
	switch kind {
	case inspect.IconPayload:
		return "payload"
	case inspect.IconLocalFile:
		return "local-file"
	case inspect.IconRemoteURL:
		return "remote-url"
	default:
		return "none"
	}
}

func recipeArchiveLayout(name string) recipe.ArchiveLayout {
	if name == "preserve-root" {
		return recipe.ArchivePreserveRoot
	}
	return recipe.ArchiveOptBundle
}

func recipeLaunchers(raw any) []recipe.Launcher {
	var out []recipe.Launcher
	for _, item := range objectSlice(raw) {
		kind := recipe.LauncherSymlink
		if stringValue(item, "kind") == "wrapper" {
			kind = recipe.LauncherWrapper
		}
		out = append(out, recipe.Launcher{
			Enabled:     boolValueDefault(item, "enabled", true),
			SourcePath:  stringValue(item, "sourcePath"),
			CommandName: stringValue(item, "commandName"),
			Destination: stringValue(item, "destination"),
			Kind:        kind,
			Missing:     boolValue(item, "missing"),
		})
	}
	return out
}

func recipeDesktops(raw any) []recipe.DesktopEntry {
	var out []recipe.DesktopEntry
	for _, item := range objectSlice(raw) {
		out = append(out, recipe.DesktopEntry{
			ID:          stringValue(item, "id"),
			Enabled:     boolValueDefault(item, "enabled", true),
			SourcePath:  stringValue(item, "sourcePath"),
			Destination: stringValue(item, "destination"),
			Contents:    stringValue(item, "contents"),
		})
	}
	return out
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}

func intValue(document map[string]any, key string, fallback int) int {
	if document == nil {
		return fallback
	}
	if _, ok := document[key]; !ok {
		return fallback
	}
	return int(int64Value(document, key))
}

func int64Value(document map[string]any, key string) int64 {
	if document == nil {
		return 0
	}
	switch value := document[key].(type) {
	case float64:
		return int64(value)
	case int:
		return int64(value)
	case int64:
		return value
	case json.Number:
		n, _ := value.Int64()
		return n
	case string:
		n, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)
		if err != nil {
			return 0
		}
		return n
	default:
		return 0
	}
}
