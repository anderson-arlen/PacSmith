package inspect

import "strings"

func parseParagraphs(data []byte) []map[string]string {
	text := bytesToString(data)
	lines := strings.Split(text, "\n")
	var paragraphs []map[string]string
	fields := map[string]string{}
	currentField := ""

	finish := func() {
		if len(fields) == 0 {
			return
		}
		copied := make(map[string]string, len(fields))
		for key, value := range fields {
			copied[key] = value
		}
		paragraphs = append(paragraphs, copied)
		fields = map[string]string{}
		currentField = ""
	}

	for _, line := range lines {
		if strings.HasSuffix(line, "\r") {
			line = line[:len(line)-1]
		}
		if line == "" {
			finish()
			continue
		}
		if (strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t")) && currentField != "" {
			continuation := line[1:]
			if continuation == "." {
				continuation = ""
			}
			fields[currentField] += "\n" + continuation
			continue
		}
		separator := strings.IndexByte(line, ':')
		if separator <= 0 {
			currentField = ""
			continue
		}
		currentField = strings.TrimSpace(line[:separator])
		fields[currentField] = strings.TrimSpace(line[separator+1:])
	}
	finish()
	return paragraphs
}

func parseControlPackage(data []byte) Metadata {
	var result Metadata
	paragraphs := parseParagraphs(data)
	if len(paragraphs) == 0 {
		result.RawFields = map[string]string{}
		return result
	}
	result.RawFields = paragraphs[0]
	field := func(name string) string { return result.RawFields[name] }
	result.Package = field("Package")
	result.Version = field("Version")
	result.Architecture = field("Architecture")
	result.Maintainer = field("Maintainer")
	result.Description = field("Description")
	result.Homepage = field("Homepage")
	result.Depends = field("Depends")
	result.PreDepends = field("Pre-Depends")
	result.Recommends = field("Recommends")
	result.Suggests = field("Suggests")
	result.Conflicts = field("Conflicts")
	result.Provides = field("Provides")
	return result
}
