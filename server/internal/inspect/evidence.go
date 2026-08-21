package inspect

import (
	"bytes"
	"encoding/base64"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var (
	literalAssignment = regexp.MustCompile("(?m)^\\s*(?:export\\s+|readonly\\s+)?([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(?:'([^']*)'|\"([^\"`$]*)\"|([^\\s#;]+))")
	literalVariable   = regexp.MustCompile(`\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)`)
	baseURLLine       = regexp.MustCompile(`(?im)^\s*baseurl\s*=\s*([^\s#;]+)`)
	heredocMarker     = regexp.MustCompile(`<<-?\s*['"]?([A-Za-z_][A-Za-z0-9_]*)['"]?`)
)

// ScriptInspection is the non-executing read of maintainer scripts.
type ScriptInspection struct {
	AptCandidates []AptRepositoryCandidate
	RPMCandidates []RPMRepositoryCandidate
	SigningKeys   []ExtractedSigningKey
	Findings      []ScriptFinding
}

// InspectScripts extracts repository candidates, embedded signing keys, and
// script findings from maintainer scripts without executing them.
func InspectScripts(scripts []MaintainerScript) ScriptInspection {
	evidence := analyzeScriptEvidence(scripts)
	return ScriptInspection{
		AptCandidates: evidence.AptCandidates,
		RPMCandidates: evidence.RPMCandidates,
		SigningKeys:   evidence.SigningKeys,
		Findings:      evidence.Findings,
	}
}

// ScriptEvidence extracts APT/RPM repository candidates and embedded signing
// keys from maintainer scripts without executing them.
func ScriptEvidence(scripts []MaintainerScript) (apt []AptRepositoryCandidate, rpm []RPMRepositoryCandidate, keys []ExtractedSigningKey) {
	evidence := InspectScripts(scripts)
	return evidence.AptCandidates, evidence.RPMCandidates, evidence.SigningKeys
}

func analyzeScriptEvidence(scripts []MaintainerScript) scriptEvidence {
	var result scriptEvidence
	for _, script := range scripts {
		assignments := literalAssignments(script.Contents)
		for name, value := range assignments {
			if !strings.Contains(strings.ToUpper(name), "KEY") || len(value) < 64 {
				continue
			}
			decoded, err := decodeBase64Strict(value)
			if err != nil || len(decoded) == 0 || len(decoded) > maxKeyBytes || !likelyOpenPGP(decoded) {
				continue
			}
			source := sourceLabel(script) + ":" + name
			appendSigningKey(&result.SigningKeys, decoded, source, fingerprint(source, value))
		}
		appendArmoredSigningKeys(&result.SigningKeys, script)
		appendRPMCandidates(&result.RPMCandidates, script, assignments)

		var scriptCandidates []AptRepositoryCandidate
		for _, body := range heredocBodies(script.Contents) {
			appendAptCandidates(&scriptCandidates, parseAptSources([]byte(body), sourceLabel(script)+" heredoc"))
		}
		appendAptCandidates(&scriptCandidates, parseAptSources([]byte(script.Contents), sourceLabel(script)))
		appendAptCandidates(&result.AptCandidates, scriptCandidates)

		lower := strings.ToLower(script.Contents)
		if len(scriptCandidates) > 0 || strings.Contains(lower, "/etc/apt/") ||
			strings.Contains(lower, "sources.list.d") || strings.Contains(lower, "keyring") {
			addFinding(&result.Findings, script, "apt-repository",
				"Vendor APT repository and signing-key setup is handled by PacSmith's update checker.",
				script.Contents, DispositionHandledByPacSmith,
				"Repository configuration is retained for update checks and is not installed as APT configuration on Arch.")
		}
		if len(result.RPMCandidates) > 0 &&
			(strings.Contains(lower, "baseurl=") || strings.Contains(lower, "repoconfig=")) {
			addFinding(&result.Findings, script, "rpm-repository",
				"Vendor RPM repository and signing-key setup is handled by PacSmith's update checker.",
				script.Contents, DispositionHandledByPacSmith,
				"Repository configuration is retained for signed RPM metadata checks and is not installed as Yum/DNF configuration on Arch.")
		}
		if strings.Contains(lower, "update-desktop-database") {
			addFinding(&result.Findings, script, "desktop-database",
				"Desktop database refresh is handled by Arch's ALPM hook.",
				"update-desktop-database", DispositionHandledByArch,
				"Arch packages trigger the desktop database hook from installed desktop files.")
		}
		if strings.Contains(lower, "update-mime-database") ||
			strings.Contains(lower, "gtk-update-icon-cache") ||
			strings.Contains(lower, "glib-compile-schemas") {
			addFinding(&result.Findings, script, "arch-cache-hook",
				"Cache/schema refresh is handled by an Arch ALPM hook.",
				script.Contents, DispositionHandledByArch,
				"The owning Arch package supplies a transaction hook for this cache.")
		}
		if strings.Contains(lower, "systemctl daemon-reload") ||
			strings.Contains(lower, "systemd-sysusers") || strings.Contains(lower, "systemd-tmpfiles") {
			addFinding(&result.Findings, script, "systemd-hook",
				"Systemd metadata refresh is handled by Arch's systemd ALPM hooks.",
				script.Contents, DispositionHandledByArch,
				"Arch's systemd package owns transaction hooks for units, sysusers, and tmpfiles.")
		}
		if strings.Contains(lower, "apparmor") {
			addFinding(&result.Findings, script, "apparmor",
				"AppArmor profile handling depends on the target system and requires an Arch-specific decision.",
				script.Contents, DispositionLifecycleRequired,
				"AppArmor is available on Arch but may not be installed or enabled.")
		}
		hasFinding := false
		for _, finding := range result.Findings {
			if finding.ScriptName == script.Name {
				hasFinding = true
				break
			}
		}
		if !hasFinding {
			addFinding(&result.Findings, script, "unclassified",
				"No safe deterministic translation was found for this script.",
				script.Contents, DispositionUnresolved,
				"The original imported package script remains data and needs user or AI resolution.")
		}
	}
	return result
}

func addFinding(findings *[]ScriptFinding, script MaintainerScript, kind, summary, evidence string, disposition ScriptDisposition, rationale string) {
	evidenceHash := fingerprint(script.Name+"\n"+kind, evidence)
	for _, finding := range *findings {
		if finding.ScriptName == script.Name && finding.Kind == kind {
			return
		}
	}
	if len(evidence) > 4096 {
		evidence = evidence[:4096]
	}
	provenance := deterministicProvenance(evidenceHash, rationale)
	provenance.Timestamp = time.Now().UTC()
	*findings = append(*findings, ScriptFinding{
		ScriptName:          script.Name,
		Kind:                kind,
		Summary:             summary,
		Evidence:            evidence,
		EvidenceFingerprint: evidenceHash,
		Disposition:         disposition,
		Provenance:          provenance,
	})
}

func literalAssignments(script string) map[string]string {
	result := map[string]string{}
	matches := literalAssignment.FindAllStringSubmatchIndex(script, -1)
	for _, loc := range matches {
		name := script[loc[2]:loc[3]]
		var value string
		switch {
		case loc[4] >= 0:
			value = script[loc[4]:loc[5]]
		case loc[6] >= 0:
			value = script[loc[6]:loc[7]]
		case loc[8] >= 0:
			value = script[loc[8]:loc[9]]
		}
		if len(value) <= 4*1024*1024 {
			result[name] = value
		}
	}
	return result
}

func sourceLabel(script MaintainerScript) string {
	if strings.HasPrefix(script.Name, "payload/") {
		return strings.TrimPrefix(script.Name, "payload/")
	}
	return "control/" + script.Name
}

func expandLiteralVariables(value string, assignments map[string]string) string {
	for pass := 0; pass < 8; pass++ {
		changed := false
		offset := 0
		for {
			loc := literalVariable.FindStringSubmatchIndex(value[offset:])
			if loc == nil {
				break
			}
			start := offset + loc[0]
			end := offset + loc[1]
			name := ""
			if loc[2] >= 0 {
				name = value[offset+loc[2] : offset+loc[3]]
			} else if loc[4] >= 0 {
				name = value[offset+loc[4] : offset+loc[5]]
			}
			replacement := assignments[name]
			if replacement == "" {
				offset = end
				continue
			}
			value = value[:start] + replacement + value[end:]
			offset = start + len(replacement)
			changed = true
		}
		if !changed {
			break
		}
	}
	return value
}

func appendRPMCandidate(destination *[]RPMRepositoryCandidate, candidate RPMRepositoryCandidate) {
	parsed, err := url.Parse(candidate.BaseURL)
	if err != nil || parsed.Host == "" || parsed.User != nil ||
		(parsed.Scheme != "https" && parsed.Scheme != "http") {
		return
	}
	path := parsed.Path
	for strings.HasSuffix(path, "/") {
		path = strings.TrimSuffix(path, "/")
	}
	parsed.Path = path
	candidate.BaseURL = parsed.String()
	candidate.KeyURLs = uniqueStrings(candidate.KeyURLs)
	for _, existing := range *destination {
		if existing.BaseURL == candidate.BaseURL && existing.Architecture == candidate.Architecture {
			return
		}
	}
	*destination = append(*destination, candidate)
}

func appendRPMCandidates(destination *[]RPMRepositoryCandidate, script MaintainerScript, assignments map[string]string) {
	var keyURLs []string
	for _, candidate := range URLsFromText(script.Contents) {
		lower := strings.ToLower(candidate)
		if strings.Contains(lower, "gpg") || strings.HasSuffix(lower, ".asc") || strings.HasSuffix(lower, ".key") {
			keyURLs = append(keyURLs, candidate)
		}
	}
	keyURLs = uniqueStrings(keyURLs)

	matches := baseURLLine.FindAllStringSubmatch(script.Contents, -1)
	for _, match := range matches {
		value := strings.TrimSpace(match[1])
		if (strings.HasPrefix(value, "'") && strings.HasSuffix(value, "'")) ||
			(strings.HasPrefix(value, `"`) && strings.HasSuffix(value, `"`)) {
			value = value[1 : len(value)-1]
		}
		value = expandLiteralVariables(value, assignments)
		if strings.Contains(value, "$") {
			continue
		}
		appendRPMCandidate(destination, RPMRepositoryCandidate{
			BaseURL:      value,
			Architecture: assignments["DEFAULT_ARCH"],
			KeyURLs:      append([]string(nil), keyURLs...),
			SourcePath:   sourceLabel(script),
		})
	}

	repository := assignments["REPOCONFIG"]
	architecture := assignments["DEFAULT_ARCH"]
	if repository != "" && strings.Contains(script.Contents, "$REPOCONFIG") {
		value := repository
		if architecture != "" &&
			(strings.Contains(script.Contents, "$REPOCONFIG/$DEFAULT_ARCH") ||
				strings.Contains(script.Contents, "${REPOCONFIG}/${DEFAULT_ARCH}")) {
			value += "/" + architecture
		}
		appendRPMCandidate(destination, RPMRepositoryCandidate{
			BaseURL:      value,
			Architecture: architecture,
			KeyURLs:      append([]string(nil), keyURLs...),
			SourcePath:   sourceLabel(script),
		})
	}
}

func heredocBodies(script string) []string {
	var result []string
	lines := strings.Split(script, "\n")
	for lineIndex := 0; lineIndex < len(lines); lineIndex++ {
		match := heredocMarker.FindStringSubmatch(lines[lineIndex])
		if match == nil {
			continue
		}
		marker := match[1]
		var body strings.Builder
		for lineIndex++; lineIndex < len(lines); lineIndex++ {
			if strings.TrimSpace(lines[lineIndex]) == marker {
				break
			}
			if body.Len() > 4*1024*1024 {
				break
			}
			body.WriteString(lines[lineIndex])
			body.WriteByte('\n')
		}
		if body.Len() > 0 && body.Len() <= 4*1024*1024 {
			result = append(result, body.String())
		}
	}
	return result
}

func likelyOpenPGP(data []byte) bool {
	if bytes.HasPrefix(data, []byte("-----BEGIN PGP PUBLIC KEY BLOCK-----")) {
		return true
	}
	if len(data) < 8 {
		return false
	}
	return data[0]&0x80 != 0
}

func appendSigningKey(keys *[]ExtractedSigningKey, contents []byte, sourcePath, sourceFingerprint string) {
	for _, key := range *keys {
		if bytes.Equal(key.Contents, contents) {
			return
		}
	}
	copied := append([]byte(nil), contents...)
	*keys = append(*keys, ExtractedSigningKey{
		Contents:          copied,
		SourcePath:        sourcePath,
		SourceFingerprint: sourceFingerprint,
	})
}

func appendArmoredSigningKeys(keys *[]ExtractedSigningKey, script MaintainerScript) {
	const beginMarker = "-----BEGIN PGP PUBLIC KEY BLOCK-----"
	const endMarker = "-----END PGP PUBLIC KEY BLOCK-----"
	raw := []byte(script.Contents)
	searchFrom := 0
	keyNumber := 1
	for searchFrom < len(raw) {
		begin := bytes.Index(raw[searchFrom:], []byte(beginMarker))
		if begin < 0 {
			break
		}
		begin += searchFrom
		end := bytes.Index(raw[begin+len(beginMarker):], []byte(endMarker))
		if end < 0 {
			break
		}
		end += begin + len(beginMarker)
		length := end + len(endMarker) - begin
		searchFrom = end + len(endMarker)
		if length <= 0 || length > maxKeyBytes {
			continue
		}
		armored := append([]byte(nil), raw[begin:begin+length]...)
		armored = append(armored, '\n')
		source := sourceLabel(script) + ":armored-openpgp-" + strconv.Itoa(keyNumber)
		keyNumber++
		appendSigningKey(keys, armored, source, fingerprint(source, string(armored)))
	}
}

func decodeBase64Strict(value string) ([]byte, error) {
	decoded, err := base64.StdEncoding.DecodeString(value)
	if err == nil {
		return decoded, nil
	}
	return base64.RawStdEncoding.DecodeString(value)
}

func uniqueStrings(values []string) []string {
	seen := map[string]struct{}{}
	var result []string
	for _, value := range values {
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		result = append(result, value)
	}
	return result
}
