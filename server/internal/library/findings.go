package library

import (
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
)

func debianMetadataJSON(meta inspect.Metadata) map[string]any {
	raw := map[string]any{}
	for key, value := range meta.RawFields {
		raw[key] = value
	}
	return map[string]any{
		"package":      meta.Package,
		"version":      meta.Version,
		"architecture": meta.Architecture,
		"maintainer":   meta.Maintainer,
		"description":  meta.Description,
		"homepage":     meta.Homepage,
		"depends":      meta.Depends,
		"preDepends":   meta.PreDepends,
		"recommends":   meta.Recommends,
		"suggests":     meta.Suggests,
		"conflicts":    meta.Conflicts,
		"provides":     meta.Provides,
		"rawFields":    raw,
	}
}

func scriptFindingsJSON(findings []inspect.ScriptFinding) []map[string]any {
	out := make([]map[string]any, 0, len(findings))
	for _, finding := range findings {
		out = append(out, map[string]any{
			"scriptName":          finding.ScriptName,
			"kind":                finding.Kind,
			"summary":             finding.Summary,
			"evidence":            finding.Evidence,
			"evidenceFingerprint": finding.EvidenceFingerprint,
			"disposition":         finding.Disposition.Name(),
			"provenance":          provenanceJSON(finding.Provenance),
		})
	}
	return out
}

func provenanceJSON(p inspect.FieldProvenance) map[string]any {
	timestamp := ""
	if !p.Timestamp.IsZero() {
		timestamp = p.Timestamp.UTC().Format("2006-01-02T15:04:05.000Z")
	}
	return map[string]any{
		"origin":            p.Origin.Name(),
		"provider":          p.Provider,
		"model":             p.Model,
		"sourceFingerprint": p.SourceFingerprint,
		"rationale":         p.Rationale,
		"timestamp":         timestamp,
		"userApproved":      p.UserApproved,
	}
}

func attachScriptFindings(document map[string]any) {
	if document == nil {
		return
	}
	detected := inspect.InspectScripts(maintainerScriptsFromDocument(document)).Findings
	stored := objectSlice(document["scriptFindings"])
	if len(stored) == 0 {
		if len(detected) > 0 {
			document["scriptFindings"] = scriptFindingsJSON(detected)
		}
		return
	}
	byFingerprint := map[string]inspect.ScriptFinding{}
	byKey := map[string]inspect.ScriptFinding{}
	for _, finding := range detected {
		byFingerprint[finding.EvidenceFingerprint] = finding
		byKey[finding.ScriptName+"\x00"+finding.Kind] = finding
	}
	changed := false
	for _, item := range stored {
		if strings.TrimSpace(stringValue(item, "disposition")) != "" {
			continue
		}
		match, ok := byFingerprint[stringValue(item, "evidenceFingerprint")]
		if !ok {
			match, ok = byKey[stringValue(item, "scriptName")+"\x00"+stringValue(item, "kind")]
		}
		if !ok {
			continue
		}
		item["disposition"] = match.Disposition.Name()
		if _, has := item["provenance"]; !has {
			item["provenance"] = provenanceJSON(match.Provenance)
		}
		changed = true
	}
	if changed {
		document["scriptFindings"] = stored
	}
}

func objectSlice(raw any) []map[string]any {
	switch value := raw.(type) {
	case []map[string]any:
		return value
	case []any:
		out := make([]map[string]any, 0, len(value))
		for _, item := range value {
			object, ok := item.(map[string]any)
			if ok {
				out = append(out, object)
			}
		}
		return out
	default:
		return nil
	}
}
