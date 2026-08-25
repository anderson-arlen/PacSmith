package library

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"sort"
	"strconv"
	"strings"
)

func carryForwardRelease(previous, next map[string]any) {
	if previous == nil || next == nil {
		return
	}
	carryDependencies(previous, next)
	if metadata, ok := mapValue(previous, "packageMetadata"); ok {
		next["packageMetadata"] = cloneObject(metadata)
	}
	carryMaintainerScripts(previous, next)
	carryScriptFindings(previous, next)
	carryPayloadRules(previous, next)
	carryLifecycle(previous, next)
	carryInstallMapping(previous, next)
	if boolValue(previous, "pkgbuildManuallyModified") {
		custom := stringValue(previous, "customPkgbuild")
		if custom == "" {
			custom = stringValue(previous, "generatedPkgbuild")
		}
		if custom != "" {
			next["customPkgbuild"] = custom
			next["pkgbuildManuallyModified"] = true
		}
		if files, ok := mapValue(previous, "customFiles"); ok {
			next["customFiles"] = cloneObject(files)
		}
	}
	delete(next, "previousManualPkgbuild")
}

func inheritUpdateConfiguration(previous, next map[string]any) {
	prevUpdate, ok := mapValue(previous, "update")
	if !ok || isManualStrategy(stringValue(prevUpdate, "strategy")) {
		return
	}
	incoming, _ := mapValue(next, "update")
	if incoming == nil {
		incoming = map[string]any{}
	}
	merged := cloneObject(prevUpdate)
	if stringValue(prevUpdate, "strategy") == "GitHub releases" &&
		stringValue(incoming, "strategy") == "GitHub releases" {
		copyIfSet(merged, incoming, "githubReleaseId")
		copyIfSet(merged, incoming, "githubAssetId")
		copyIfSet(merged, incoming, "githubTag")
		copyIfSet(merged, incoming, "githubPublisherDigest")
	}
	mergeUpdateCandidateLists(merged, incoming)
	next["update"] = merged
}

func applyAcquisition(body map[string]any, req ImportRequest) {
	acquisition := map[string]any{
		"kind":              firstNonEmpty(strings.TrimSpace(req.AcquisitionKind), "local-file"),
		"canonicalIdentity": strings.TrimSpace(req.CanonicalIdentity),
	}
	if len(req.Acquisition) > 0 && string(req.Acquisition) != "null" {
		var incoming map[string]any
		if jsonUnmarshalObject(req.Acquisition, &incoming) && incoming != nil {
			for key, value := range incoming {
				acquisition[key] = value
			}
		}
	}
	if kind := strings.TrimSpace(req.AcquisitionKind); kind != "" {
		acquisition["kind"] = kind
	}
	if identity := strings.TrimSpace(req.CanonicalIdentity); identity != "" {
		acquisition["canonicalIdentity"] = identity
	}
	body["acquisition"] = acquisition
	if original := stringValue(acquisition, "originalUrl"); original != "" && stringValue(body, "sourceUrl") == "" {
		body["sourceUrl"] = original
	}
}

func applyGitHubImportOptions(body map[string]any, req ImportRequest) {
	update, ok := mapValue(body, "update")
	if !ok {
		update = map[string]any{}
		body["update"] = update
	}
	if regex := strings.TrimSpace(req.GitHubAssetRegex); regex != "" {
		update["githubAssetRegex"] = regex
	}
	if req.GitHubIncludePrereleases {
		update["githubIncludePrereleases"] = true
	}
	acquisition, _ := mapValue(body, "acquisition")
	if stringValue(acquisition, "kind") == "github-release" {
		copyGitHubUpdateFields(update, acquisition)
		update["strategy"] = "GitHub releases"
	}
}

func carryDependencies(previous, next map[string]any) {
	oldByExpr := map[string]map[string]any{}
	for _, dep := range objectSlice(previous["dependencies"]) {
		oldByExpr[stringValue(dep, "rawExpression")] = dep
	}
	current := objectSlice(next["dependencies"])
	for i, dep := range current {
		if old, ok := oldByExpr[stringValue(dep, "rawExpression")]; ok {
			current[i] = old
		}
	}
	if current != nil {
		next["dependencies"] = current
	}
}

func carryMaintainerScripts(previous, next map[string]any) {
	oldByName := map[string]map[string]any{}
	for _, script := range objectSlice(previous["maintainerScripts"]) {
		oldByName[stringValue(script, "name")+"\x00"+scriptContentFingerprint(script)] = script
	}
	current := objectSlice(next["maintainerScripts"])
	for i, script := range current {
		key := stringValue(script, "name") + "\x00" + scriptContentFingerprint(script)
		if old, ok := oldByName[key]; ok {
			script["acknowledgedFingerprint"] = old["acknowledgedFingerprint"]
			current[i] = script
		}
	}
	if current != nil {
		next["maintainerScripts"] = current
	}
}

func carryScriptFindings(previous, next map[string]any) {
	oldByFP := map[string]map[string]any{}
	for _, finding := range objectSlice(previous["scriptFindings"]) {
		oldByFP[stringValue(finding, "evidenceFingerprint")] = finding
	}
	current := objectSlice(next["scriptFindings"])
	for i, finding := range current {
		if old, ok := oldByFP[stringValue(finding, "evidenceFingerprint")]; ok {
			current[i] = old
		}
	}
	if current != nil {
		next["scriptFindings"] = current
	}
}

func carryPayloadRules(previous, next map[string]any) {
	oldRules := objectSlice(previous["payloadRules"])
	current := objectSlice(next["payloadRules"])
	byPath := map[string]int{}
	for i, rule := range current {
		byPath[stringValue(rule, "path")] = i
	}
	for _, rule := range oldRules {
		path := stringValue(rule, "path")
		oldFP := payloadPathFingerprint(previous, path)
		newFP := payloadPathFingerprint(next, path)
		if oldFP == "" || newFP == "" || stringValue(rule, "acknowledgedFingerprint") != oldFP {
			continue
		}
		if index, ok := byPath[path]; ok {
			current[index] = rule
		} else {
			current = append(current, rule)
			byPath[path] = len(current) - 1
		}
	}
	next["payloadRules"] = current
}

func carryLifecycle(previous, next map[string]any) {
	oldLife, ok := mapValue(previous, "lifecycleScript")
	if !ok || stringValue(oldLife, "contents") == "" {
		return
	}
	oldSources := stringSlice(oldLife["sourceFingerprints"])
	var newSources []string
	for _, finding := range objectSlice(next["scriptFindings"]) {
		if stringValue(finding, "disposition") == "lifecycle-required" {
			newSources = append(newSources, stringValue(finding, "evidenceFingerprint"))
		}
	}
	if sameStringSet(oldSources, newSources) {
		next["lifecycleScript"] = cloneObject(oldLife)
	}
}

func carryInstallMapping(previous, next map[string]any) {
	prevInstall, _ := mapValue(previous, "installMapping")
	nextInstall, ok := mapValue(next, "installMapping")
	if !ok {
		nextInstall = map[string]any{}
		next["installMapping"] = nextInstall
	}
	prevType := stringValue(previous, "sourceType")
	nextType := stringValue(next, "sourceType")
	if prevType == nextType {
		if nextType == "archive" {
			copyIfSet(nextInstall, prevInstall, "archiveLayout")
			copyIfSet(nextInstall, prevInstall, "optDirectory")
			copyIfSet(nextInstall, prevInstall, "binaryDestination")
			if links := prevInstall["executableLinks"]; links != nil {
				nextInstall["executableLinks"] = links
			}
			copyIfSet(nextInstall, prevInstall, "commonPrefix")
			if _, has := prevInstall["stripCommonPrefix"]; has {
				nextInstall["stripCommonPrefix"] = prevInstall["stripCommonPrefix"]
			}
			if payloadHasPath(next, stringValue(prevInstall, "binarySourcePath")) {
				nextInstall["binarySourcePath"] = prevInstall["binarySourcePath"]
			}
		} else if nextType == "elf-binary" {
			copyIfSet(nextInstall, prevInstall, "binaryDestination")
		}
	}
	nextInstall["launchers"] = carryLaunchers(prevInstall, nextInstall, next, nextType)
	nextInstall["desktopEntries"] = carryDesktops(prevInstall, nextInstall, next)
	carryIcon(prevInstall, nextInstall, next)
	carryAppRun(prevInstall, nextInstall)
}

func carryLaunchers(prevInstall, nextInstall, next map[string]any, sourceType string) []map[string]any {
	current := objectSlice(nextInstall["launchers"])
	byPath := map[string]int{}
	for i, launcher := range current {
		byPath[stringValue(launcher, "sourcePath")] = i
	}
	for _, prior := range objectSlice(prevInstall["launchers"]) {
		sourcePath := stringValue(prior, "sourcePath")
		if sourceType == "appimage" && sourcePath != "AppRun" {
			continue
		}
		carried := cloneObject(prior)
		carried["missing"] = !payloadHasPath(next, sourcePath)
		if index, ok := byPath[sourcePath]; ok {
			current[index] = carried
		} else {
			current = append(current, carried)
		}
	}
	return current
}

func carryDesktops(prevInstall, nextInstall, next map[string]any) []map[string]any {
	current := objectSlice(nextInstall["desktopEntries"])
	byPath := map[string]int{}
	for i, desktop := range current {
		if path := stringValue(desktop, "sourcePath"); path != "" {
			byPath[path] = i
		}
	}
	for _, prior := range objectSlice(prevInstall["desktopEntries"]) {
		sourcePath := stringValue(prior, "sourcePath")
		index, exists := byPath[sourcePath]
		if exists && sourcePath != "" && !boolValue(prior, "userModified") {
			current[index]["enabled"] = prior["enabled"]
			current[index]["destination"] = prior["destination"]
			current[index]["missing"] = false
			continue
		}
		carried := cloneObject(prior)
		carried["missing"] = sourcePath != "" && !payloadHasPath(next, sourcePath)
		if exists {
			current[index] = carried
		} else {
			current = append(current, carried)
		}
	}
	return current
}

func carryIcon(prevInstall, nextInstall, next map[string]any) {
	prior, ok := mapValue(prevInstall, "icon")
	if !ok || stringValue(prior, "sourceKind") == "" || stringValue(prior, "sourceKind") == "none" {
		return
	}
	payloadStillExists := stringValue(prior, "sourceKind") != "payload" ||
		payloadHasPath(next, stringValue(prior, "sourcePath"))
	provenance, _ := mapValue(prior, "provenance")
	if stringValue(provenance, "origin") == "user" || stringValue(prior, "sourceKind") != "payload" || payloadStillExists {
		icon := cloneObject(prior)
		icon["missing"] = !payloadStillExists
		nextInstall["icon"] = icon
	}
}

func carryAppRun(prevInstall, nextInstall map[string]any) {
	prior, ok := mapValue(prevInstall, "appRun")
	if !ok {
		return
	}
	current, _ := mapValue(nextInstall, "appRun")
	if current == nil {
		current = map[string]any{}
		nextInstall["appRun"] = current
	}
	if boolValue(prior, "userModified") {
		carried := cloneObject(prior)
		carried["originalContents"] = current["originalContents"]
		carried["originalContentsSha256"] = current["originalContentsSha256"]
		carried["present"] = boolValue(current, "present") || boolValue(carried, "present")
		nextInstall["appRun"] = carried
		return
	}
	if boolValue(current, "present") &&
		stringValue(prior, "acknowledgedFingerprint") != "" &&
		stringValue(prior, "acknowledgedFingerprint") == appRunFingerprint(current) {
		current["acknowledgedFingerprint"] = prior["acknowledgedFingerprint"]
	}
}

func payloadHasPath(document map[string]any, path string) bool {
	if path == "" {
		return false
	}
	for _, entry := range objectSlice(document["payload"]) {
		if stringValue(entry, "path") == path {
			return true
		}
	}
	return false
}

func payloadPathFingerprint(document map[string]any, path string) string {
	type fields struct {
		path, kind, target, size, sha string
	}
	var entries []fields
	for _, entry := range objectSlice(document["payload"]) {
		entryPath := stringValue(entry, "path")
		if entryPath == path || strings.HasPrefix(entryPath, strings.TrimSuffix(path, "/")+"/") {
			entries = append(entries, fields{
				path:   entryPath,
				kind:   stringValue(entry, "type"),
				target: stringValue(entry, "symlinkTarget"),
				size:   stringifyJSON(entry["size"]),
				sha:    stringValue(entry, "contentSha256"),
			})
		}
	}
	if len(entries) == 0 {
		return ""
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].path < entries[j].path })
	h := sha256.New()
	for _, entry := range entries {
		for _, field := range []string{entry.path, entry.kind, entry.target, entry.size, entry.sha} {
			h.Write([]byte(field))
			h.Write([]byte{0})
		}
	}
	return hex.EncodeToString(h.Sum(nil))
}

func scriptContentFingerprint(script map[string]any) string {
	h := sha256.New()
	h.Write([]byte(stringValue(script, "name")))
	h.Write([]byte{0})
	h.Write([]byte(stringValue(script, "contents")))
	return hex.EncodeToString(h.Sum(nil))
}

func appRunFingerprint(appRun map[string]any) string {
	h := sha256.New()
	h.Write([]byte(stringValue(appRun, "contents")))
	return hex.EncodeToString(h.Sum(nil))
}

func mergeUpdateCandidateLists(into, from map[string]any) {
	if from == nil {
		return
	}
	into["detectedCandidates"] = uniqueStrings(append(stringSlice(into["detectedCandidates"]), stringSlice(from["detectedCandidates"])...))
	into["aptCandidates"] = mergeObjectLists(into["aptCandidates"], from["aptCandidates"], func(item map[string]any) string {
		return strings.Join([]string{stringValue(item, "uri"), stringValue(item, "suite"), strings.Join(stringSlice(item["components"]), " ")}, " ")
	})
	into["rpmCandidates"] = mergeObjectLists(into["rpmCandidates"], from["rpmCandidates"], func(item map[string]any) string {
		return stringValue(item, "baseUrl") + " " + stringValue(item, "architecture")
	})
}

func mergeObjectLists(left, right any, key func(map[string]any) string) []map[string]any {
	out := objectSlice(left)
	seen := map[string]struct{}{}
	for _, item := range out {
		seen[key(item)] = struct{}{}
	}
	for _, item := range objectSlice(right) {
		id := key(item)
		if _, ok := seen[id]; ok {
			continue
		}
		seen[id] = struct{}{}
		out = append(out, item)
	}
	return out
}

func uniqueStrings(values []string) []string {
	seen := map[string]struct{}{}
	out := make([]string, 0, len(values))
	for _, value := range values {
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

func sameStringSet(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	l := append([]string(nil), left...)
	r := append([]string(nil), right...)
	sort.Strings(l)
	sort.Strings(r)
	for i := range l {
		if l[i] != r[i] {
			return false
		}
	}
	return true
}

func cloneObject(in map[string]any) map[string]any {
	out := make(map[string]any, len(in))
	for key, value := range in {
		out[key] = value
	}
	return out
}

func copyIfSet(dst, src map[string]any, key string) {
	if src == nil || dst == nil {
		return
	}
	value, ok := src[key]
	if !ok {
		return
	}
	switch typed := value.(type) {
	case string:
		if strings.TrimSpace(typed) == "" {
			return
		}
	case nil:
		return
	}
	dst[key] = value
}

func stringifyJSON(value any) string {
	switch typed := value.(type) {
	case string:
		return typed
	case float64:
		if typed == float64(int64(typed)) {
			return strconv.FormatInt(int64(typed), 10)
		}
		return strconv.FormatFloat(typed, 'f', -1, 64)
	case int:
		return strconv.Itoa(typed)
	case int64:
		return strconv.FormatInt(typed, 10)
	default:
		return ""
	}
}

func jsonUnmarshalObject(raw []byte, dest *map[string]any) bool {
	if dest == nil {
		return false
	}
	if err := json.Unmarshal(raw, dest); err != nil || *dest == nil {
		return false
	}
	return true
}
