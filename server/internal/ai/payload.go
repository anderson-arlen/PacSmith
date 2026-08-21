package ai

import (
	"crypto/sha256"
	"encoding/hex"
	"sort"
	"strings"
)

type payloadEntry struct {
	Path           string
	Type           string
	SymlinkTarget  string
	Size           string
	RequiresReview bool
	ReviewReason   string
	ContentSHA256  string
	TextPreview    string
}

type payloadRule struct {
	Path                    string
	Excluded                bool
	Reason                  string
	UserDecision            bool
	AcknowledgedFingerprint string
}

type payloadReviewState struct {
	treatment    string
	needsReview  bool
	decisionPath string
}

func decodePayloadEntries(values []any) []payloadEntry {
	out := make([]payloadEntry, 0, len(values))
	for _, value := range values {
		object := asObject(value)
		out = append(out, payloadEntry{
			Path:           objectString(object, "path"),
			Type:           objectString(object, "type"),
			SymlinkTarget:  objectString(object, "symlinkTarget"),
			Size:           asString(object["size"]),
			RequiresReview: objectBool(object, "requiresReview"),
			ReviewReason:   objectString(object, "reviewReason"),
			ContentSHA256:  objectString(object, "contentSha256"),
			TextPreview:    objectString(object, "textPreview"),
		})
	}
	return out
}

func decodePayloadRules(values []any) []payloadRule {
	out := make([]payloadRule, 0, len(values))
	for _, value := range values {
		object := asObject(value)
		out = append(out, payloadRule{
			Path:                    objectString(object, "path"),
			Excluded:                objectBool(object, "excluded"),
			Reason:                  objectString(object, "reason"),
			UserDecision:            objectBool(object, "userDecision"),
			AcknowledgedFingerprint: objectString(object, "acknowledgedFingerprint"),
		})
	}
	return out
}

func covers(parent, child string) bool {
	return child == parent || strings.HasPrefix(child, parent+"/")
}

func applicableRule(rules []payloadRule, entryPath string) *payloadRule {
	var result *payloadRule
	for i := range rules {
		rule := &rules[i]
		if !covers(rule.Path, entryPath) {
			continue
		}
		if result == nil || len(rule.Path) > len(result.Path) {
			result = rule
		}
	}
	return result
}

func payloadFingerprint(entries []payloadEntry, path string) string {
	matched := make([]payloadEntry, 0)
	for _, entry := range entries {
		if covers(path, entry.Path) {
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
		for _, field := range []string{entry.Path, entry.Type, entry.SymlinkTarget, entry.Size, entry.ContentSHA256} {
			h.Write([]byte(field))
			h.Write(nul)
		}
	}
	return hex.EncodeToString(h.Sum(nil))
}

func payloadTreatment(entries []payloadEntry, rules []payloadRule, entry payloadEntry) payloadReviewState {
	rule := applicableRule(rules, entry.Path)
	if !entry.RequiresReview && rule == nil {
		return payloadReviewState{treatment: "keep", needsReview: false, decisionPath: entry.Path}
	}
	if rule == nil {
		return payloadReviewState{treatment: "pending", needsReview: true, decisionPath: entry.Path}
	}
	currentFingerprint := payloadFingerprint(entries, rule.Path)
	disposition := "keep"
	if rule.Excluded {
		disposition = "exclude"
	}
	if !rule.UserDecision {
		if rule.Reason == "AI-reviewed payload decision" ||
			rule.Reason == "User-approved AI payload decision" {
			return payloadReviewState{
				treatment:    disposition,
				needsReview:  currentFingerprint == "" || currentFingerprint != rule.AcknowledgedFingerprint,
				decisionPath: rule.Path,
			}
		}
		stale := currentFingerprint == "" || rule.AcknowledgedFingerprint == "" ||
			currentFingerprint != rule.AcknowledgedFingerprint
		return payloadReviewState{treatment: "exclude", needsReview: stale, decisionPath: rule.Path}
	}
	if currentFingerprint == "" || currentFingerprint != rule.AcknowledgedFingerprint {
		return payloadReviewState{treatment: disposition, needsReview: true, decisionPath: rule.Path}
	}
	return payloadReviewState{treatment: disposition, needsReview: false, decisionPath: rule.Path}
}

func contentFingerprint(name, contents string) string {
	h := sha256.New()
	h.Write([]byte(name))
	h.Write([]byte{0})
	h.Write([]byte(contents))
	return hex.EncodeToString(h.Sum(nil))
}

func appRunNeedsReview(appRun map[string]any) bool {
	if !objectBool(appRun, "present") || !objectBool(appRun, "script") || objectBool(appRun, "userModified") {
		return false
	}
	contents := objectString(appRun, "contents")
	h := sha256.New()
	h.Write([]byte(contents))
	return objectString(appRun, "acknowledgedFingerprint") != hex.EncodeToString(h.Sum(nil))
}
