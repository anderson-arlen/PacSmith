package inspect

import (
	"regexp"
	"strings"
)

var (
	whitespace  = regexp.MustCompile(`\s+`)
	deb822Types = regexp.MustCompile(`(?i)(?:^|\n)Types\s*:`)
)

func parseAptSources(data []byte, sourcePath string) []AptRepositoryCandidate {
	text := bytesToString(data)
	looksDeb822 := strings.HasSuffix(sourcePath, ".sources") || deb822Types.MatchString(text)
	if looksDeb822 {
		return parseDeb822(data, sourcePath)
	}
	return parseOneLine(text, sourcePath)
}

func words(value string) []string {
	value = strings.TrimSpace(value)
	if value == "" {
		return nil
	}
	return whitespace.Split(value, -1)
}

func parseDeb822(data []byte, sourcePath string) []AptRepositoryCandidate {
	var result []AptRepositoryCandidate
	seen := map[string]struct{}{}
	for _, fields := range parseParagraphs(data) {
		if !containsWord(words(fields["Types"]), "deb") {
			continue
		}
		uris := words(fields["URIs"])
		suites := words(fields["Suites"])
		components := words(fields["Components"])
		architectures := words(fields["Architectures"])
		for _, removed := range words(fields["Architectures-Remove"]) {
			architectures = removeAll(architectures, removed)
		}
		for _, uri := range uris {
			for _, suite := range suites {
				appendUniqueApt(&result, seen, AptRepositoryCandidate{
					URI:           uri,
					Suite:         suite,
					Components:    append([]string(nil), components...),
					Architectures: append([]string(nil), architectures...),
					SignedBy:      fields["Signed-By"],
					SourcePath:    sourcePath,
				})
			}
		}
	}
	return result
}

func parseOneLine(text, sourcePath string) []AptRepositoryCandidate {
	var result []AptRepositoryCandidate
	seen := map[string]struct{}{}
	for _, raw := range strings.Split(text, "\n") {
		line := raw
		if strings.HasSuffix(line, "\r") {
			line = line[:len(line)-1]
		}
		if comment := strings.IndexByte(line, '#'); comment >= 0 {
			line = line[:comment]
		}
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "deb ") && !strings.HasPrefix(line, "deb\t") {
			continue
		}
		line = strings.TrimSpace(line[3:])
		options := ""
		if strings.HasPrefix(line, "[") {
			closing := strings.IndexByte(line, ']')
			if closing < 0 {
				continue
			}
			options = line[1:closing]
			line = strings.TrimSpace(line[closing+1:])
		}
		fields := words(line)
		if len(fields) < 2 {
			continue
		}
		var architectures []string
		signedBy := ""
		for _, option := range words(options) {
			if strings.HasPrefix(option, "arch=") {
				architectures = splitComma(option[5:])
			} else if strings.HasPrefix(option, "signed-by=") {
				signedBy = option[10:]
			}
		}
		appendUniqueApt(&result, seen, AptRepositoryCandidate{
			URI:           fields[0],
			Suite:         fields[1],
			Components:    append([]string(nil), fields[2:]...),
			Architectures: architectures,
			SignedBy:      signedBy,
			SourcePath:    sourcePath,
		})
	}
	return result
}

func appendUniqueApt(result *[]AptRepositoryCandidate, seen map[string]struct{}, candidate AptRepositoryCandidate) {
	if !usableHTTPURI(candidate.URI) || candidate.Suite == "" {
		return
	}
	for strings.HasSuffix(candidate.URI, "/") {
		candidate.URI = strings.TrimSuffix(candidate.URI, "/")
	}
	key := candidate.URI + "\x00" + candidate.Suite + "\x00" +
		strings.Join(candidate.Components, " ") + "\x00" + strings.Join(candidate.Architectures, ",")
	if _, ok := seen[key]; ok {
		return
	}
	seen[key] = struct{}{}
	*result = append(*result, candidate)
}

func appendAptCandidates(destination *[]AptRepositoryCandidate, source []AptRepositoryCandidate) {
	for _, candidate := range source {
		duplicate := false
		for _, existing := range *destination {
			if existing.URI == candidate.URI && existing.Suite == candidate.Suite &&
				joinEq(existing.Components, candidate.Components) &&
				joinEq(existing.Architectures, candidate.Architectures) {
				duplicate = true
				break
			}
		}
		if !duplicate {
			*destination = append(*destination, candidate)
		}
	}
}

func containsWord(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func removeAll(values []string, removed string) []string {
	out := values[:0]
	for _, value := range values {
		if value != removed {
			out = append(out, value)
		}
	}
	return out
}

func splitComma(value string) []string {
	parts := strings.Split(value, ",")
	var result []string
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part != "" {
			result = append(result, part)
		}
	}
	return result
}

func joinEq(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func aptDisplayText(candidate AptRepositoryCandidate) string {
	result := candidate.URI
	if candidate.Suite != "" {
		result += "  " + candidate.Suite
	}
	if len(candidate.Components) > 0 {
		result += "  " + strings.Join(candidate.Components, " ")
	}
	return result
}
