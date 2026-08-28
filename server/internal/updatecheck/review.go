package updatecheck

import (
	"crypto/sha256"
	"encoding/hex"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

func automaticReviewBlockers(previous, next map[string]any) []string {
	var blockers []string
	appendBlocker := func(message string) {
		for _, existing := range blockers {
			if existing == message {
				return
			}
		}
		blockers = append(blockers, message)
	}
	if stringValue(previous, "buildStatus") != "succeeded" && len(stringValues(previous["builtArtifactIds"])) == 0 {
		appendBlocker("previous package configuration has no successful build")
	}
	if len(releaseReviewIssues(previous)) > 0 {
		appendBlocker("previous package configuration still has review issues")
	}
	if stringValue(previous, "sourceType") != stringValue(next, "sourceType") {
		appendBlocker("vendor package format changed")
	}
	if !sameStrings(dependencySurface(previous), dependencySurface(next)) {
		appendBlocker("vendor dependency declarations changed")
	}
	if !sameStrings(maintainerScriptSurface(previous), maintainerScriptSurface(next)) {
		appendBlocker("vendor lifecycle scripts changed")
	}
	if lifecycleSurface(previous) != lifecycleSurface(next) {
		appendBlocker("generated Arch lifecycle behavior changed")
	}
	for _, issue := range releaseReviewIssues(next) {
		appendBlocker(issue)
	}
	return blockers
}

func releaseReviewIssues(document map[string]any) []string {
	var issues []string
	add := func(message string) { issues = append(issues, message) }
	install := object(document["installMapping"])
	appRun := object(install["appRun"])
	if boolValue(appRun, "present") && boolValue(appRun, "script") &&
		!boolValue(appRun, "userModified") &&
		stringValue(appRun, "acknowledgedFingerprint") != sha256Text(rawStringValue(appRun, "contents")) {
		add("extracted AppImage AppRun has not been reviewed")
	}
	if boolValue(object(install["icon"]), "missing") {
		add("configured application icon is missing")
	}
	if stringValue(document, "sourceType") == "archive" && archiveDesktopCommandUnmapped(install) {
		add("enabled desktop entry invokes a command that PacSmith does not expose")
	}
	for _, launcher := range objects(install["launchers"]) {
		if boolValue(launcher, "enabled") && boolValue(launcher, "missing") {
			add("enabled launcher source is missing from the inspected payload")
		}
	}
	for _, desktop := range objects(install["desktopEntries"]) {
		if boolValue(desktop, "enabled") && boolValue(desktop, "missing") {
			add("enabled desktop-entry source is missing from the inspected payload")
		}
	}
	for _, dependency := range objects(document["dependencies"]) {
		if strings.EqualFold(stringValue(dependency, "status"), "unresolved") {
			add("vendor dependency has no reviewed Arch treatment")
		}
	}
	if stringValue(document, "sourceType") != "appimage" {
		for _, entry := range objects(document["payload"]) {
			if boolValue(entry, "requiresReview") && payloadNeedsReview(document, entry) {
				add("payload item needs an explicit keep/exclude decision")
			}
		}
	}
	lifecycle := object(document["lifecycleScript"])
	if rawStringValue(lifecycle, "contents") != "" {
		fingerprint := namedContentFingerprint(stringValue(lifecycle, "fileName"),
			rawStringValue(lifecycle, "contents"))
		if !boolValue(lifecycle, "validationPassed") {
			add("Arch lifecycle script failed validation")
		} else if stringValue(lifecycle, "acknowledgedFingerprint") != fingerprint {
			add("Arch lifecycle script has not been acknowledged")
		}
	}
	scripts := map[string]map[string]any{}
	for _, script := range objects(document["maintainerScripts"]) {
		scripts[stringValue(script, "name")] = script
	}
	sources := stringSet(lifecycle["sourceFingerprints"])
	for _, finding := range objects(document["scriptFindings"]) {
		if script := scripts[stringValue(finding, "scriptName")]; script != nil &&
			stringValue(script, "acknowledgedFingerprint") == scriptFingerprint(script) {
			continue
		}
		switch stringValue(finding, "disposition") {
		case "unresolved":
			add("vendor script finding is unresolved")
		case "lifecycle-required":
			if !boolValue(lifecycle, "validationPassed") {
				add("vendor lifecycle responsibility is not represented by a valid script")
			} else if _, ok := sources[stringValue(finding, "evidenceFingerprint")]; !ok {
				add("vendor lifecycle responsibility is absent from the Arch lifecycle script")
			}
		}
	}
	return issues
}

func dependencySurface(document map[string]any) []string {
	result := make([]string, 0)
	for _, dependency := range objects(document["dependencies"]) {
		result = append(result, strings.TrimSpace(stringValue(dependency, "rawExpression")))
	}
	sort.Slice(result, func(i, j int) bool { return strings.ToLower(result[i]) < strings.ToLower(result[j]) })
	return result
}

func maintainerScriptSurface(document map[string]any) []string {
	result := make([]string, 0)
	for _, script := range objects(document["maintainerScripts"]) {
		result = append(result, stringValue(script, "name")+":"+scriptFingerprint(script))
	}
	sort.Strings(result)
	return result
}

func lifecycleSurface(document map[string]any) string {
	lifecycle := object(document["lifecycleScript"])
	if rawStringValue(lifecycle, "contents") == "" {
		return ""
	}
	return stringValue(lifecycle, "fileName") + "\n" +
		namedContentFingerprint(stringValue(lifecycle, "fileName"), rawStringValue(lifecycle, "contents"))
}

func scriptFingerprint(script map[string]any) string {
	return namedContentFingerprint(stringValue(script, "name"), rawStringValue(script, "contents"))
}

func namedContentFingerprint(name, contents string) string {
	hash := sha256.New()
	hash.Write([]byte(name))
	hash.Write([]byte{0})
	hash.Write([]byte(contents))
	return hex.EncodeToString(hash.Sum(nil))
}

func sha256Text(contents string) string {
	digest := sha256.Sum256([]byte(contents))
	return hex.EncodeToString(digest[:])
}

func payloadNeedsReview(document, entry map[string]any) bool {
	path := stringValue(entry, "path")
	var rule map[string]any
	for _, candidate := range objects(document["payloadRules"]) {
		candidatePath := stringValue(candidate, "path")
		if covers(candidatePath, path) && (rule == nil || len(candidatePath) > len(stringValue(rule, "path"))) {
			rule = candidate
		}
	}
	if rule == nil {
		return true
	}
	fingerprint := payloadFingerprint(document, stringValue(rule, "path"))
	acknowledged := stringValue(rule, "acknowledgedFingerprint")
	return fingerprint == "" || acknowledged == "" || fingerprint != acknowledged
}

func payloadFingerprint(document map[string]any, parent string) string {
	entries := make([]map[string]any, 0)
	for _, entry := range objects(document["payload"]) {
		if covers(parent, stringValue(entry, "path")) {
			entries = append(entries, entry)
		}
	}
	if len(entries) == 0 {
		return ""
	}
	sort.Slice(entries, func(i, j int) bool {
		return stringValue(entries[i], "path") < stringValue(entries[j], "path")
	})
	hash := sha256.New()
	for _, entry := range entries {
		for _, field := range []string{stringValue(entry, "path"), stringValue(entry, "type"),
			stringValue(entry, "symlinkTarget"), jsonString(entry["size"]),
			stringValue(entry, "contentSha256")} {
			hash.Write([]byte(field))
			hash.Write([]byte{0})
		}
	}
	return hex.EncodeToString(hash.Sum(nil))
}

func archiveDesktopCommandUnmapped(install map[string]any) bool {
	exposed := map[string]struct{}{}
	for _, launcher := range objects(install["launchers"]) {
		if !boolValue(launcher, "enabled") || boolValue(launcher, "missing") {
			continue
		}
		if command := strings.ToLower(stringValue(launcher, "commandName")); command != "" {
			exposed[command] = struct{}{}
		}
		if destination := stringValue(launcher, "destination"); destination != "" {
			exposed[strings.ToLower(filepath.Base(destination))] = struct{}{}
		}
	}
	for _, desktop := range objects(install["desktopEntries"]) {
		if !boolValue(desktop, "enabled") {
			continue
		}
		command := desktopCommand(rawStringValue(desktop, "contents"))
		if command != "" {
			if _, ok := exposed[strings.ToLower(command)]; !ok {
				return true
			}
		}
	}
	return false
}

func desktopCommand(contents string) string {
	for _, line := range strings.Split(contents, "\n") {
		if !strings.HasPrefix(strings.TrimSpace(line), "Exec=") {
			continue
		}
		exec := strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "Exec="))
		if exec == "" {
			return ""
		}
		var executable string
		if exec[0] == '"' {
			closing := strings.Index(exec[1:], "\"")
			if closing < 1 {
				return ""
			}
			executable = exec[1 : closing+1]
		} else {
			executable = strings.Fields(exec)[0]
		}
		command := filepath.Base(executable)
		lower := strings.ToLower(command)
		if lower == "env" || lower == "sh" || lower == "bash" || lower == "gio" ||
			lower == "gapplication" || strings.HasPrefix(lower, "dbus-") || strings.HasPrefix(lower, "python") {
			return ""
		}
		return command
	}
	return ""
}

func covers(parent, child string) bool {
	return parent != "" && (child == parent || strings.HasPrefix(child, strings.TrimSuffix(parent, "/")+"/"))
}

func stringValues(value any) []string {
	result := make([]string, 0)
	switch raw := value.(type) {
	case []any:
		for _, item := range raw {
			if text, ok := item.(string); ok {
				result = append(result, text)
			}
		}
	case []string:
		result = append(result, raw...)
	}
	return result
}

func stringSet(value any) map[string]struct{} {
	result := map[string]struct{}{}
	for _, item := range stringValues(value) {
		result[item] = struct{}{}
	}
	return result
}

func sameStrings(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}

func jsonString(value any) string {
	switch typed := value.(type) {
	case string:
		return typed
	case float64:
		return strconv.FormatInt(int64(typed), 10)
	case int64:
		return strconv.FormatInt(typed, 10)
	case int:
		return strconv.Itoa(typed)
	default:
		return ""
	}
}
