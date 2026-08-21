package inspect

import (
	_ "embed"
	"encoding/json"
	"regexp"
	"strings"
	"sync"
)

//go:embed dependency-mappings.json
var verifiedMappingsJSON []byte

var (
	archRestriction = regexp.MustCompile(`\s*\[[^\]]*\]\s*`)
	buildProfile    = regexp.MustCompile(`\s*<[^>]*>\s*`)
	debianDep       = regexp.MustCompile(`^\s*([a-zA-Z0-9][a-zA-Z0-9+.-]*)(?::[a-zA-Z0-9-]+)?\s*(?:\((<<|<=|=|>=|>>)\s*([^)]+)\))?\s*$`)
	mappingsOnce    sync.Once
	verifiedMap     map[string]string
)

func splitTopLevel(input string, separator rune) []string {
	var result []string
	start := 0
	parentheses := 0
	brackets := 0
	for index, character := range input {
		switch character {
		case '(':
			parentheses++
		case ')':
			if parentheses > 0 {
				parentheses--
			}
		case '[':
			brackets++
		case ']':
			if brackets > 0 {
				brackets--
			}
		default:
			if character == separator && parentheses == 0 && brackets == 0 {
				result = append(result, strings.TrimSpace(input[start:index]))
				start = index + len(string(separator))
			}
		}
	}
	result = append(result, strings.TrimSpace(input[start:]))
	filtered := result[:0]
	for _, item := range result {
		if item != "" {
			filtered = append(filtered, item)
		}
	}
	return filtered
}

func parseAlternative(input string) DependencyAlternative {
	input = archRestriction.ReplaceAllString(input, "")
	input = buildProfile.ReplaceAllString(input, "")
	match := debianDep.FindStringSubmatch(input)
	if match == nil {
		return DependencyAlternative{PackageName: strings.TrimSpace(input)}
	}
	return DependencyAlternative{
		PackageName:     match[1],
		VersionOperator: match[2],
		Version:         strings.TrimSpace(match[3]),
	}
}

func ParseDependencies(declarations string) []Dependency {
	if strings.TrimSpace(declarations) == "" {
		return nil
	}
	var result []Dependency
	for _, group := range splitTopLevel(declarations, ',') {
		dep := Dependency{RawExpression: group}
		for _, alternative := range splitTopLevel(group, '|') {
			dep.Alternatives = append(dep.Alternatives, parseAlternative(alternative))
		}
		result = append(result, dep)
	}
	return result
}

func ApplyVerifiedMappings(dependencies []Dependency, mappings map[string]string) bool {
	changed := false
	for i := range dependencies {
		dep := &dependencies[i]
		meaningfulUserOverride := dep.UserOverride &&
			(dep.ArchPackage != "" || dep.Status != MappingUnresolved)
		if meaningfulUserOverride || len(dep.Alternatives) == 0 {
			continue
		}
		for _, alternative := range dep.Alternatives {
			mapped, ok := mappings[alternative.PackageName]
			if !ok {
				continue
			}
			dep.ArchPackage = mapped
			dep.Status = MappingResolved
			dep.MappingSource = "verified built-in mapping"
			dep.Confidence = 1.0
			dep.UserOverride = false
			dep.Ignored = false
			dep.Bundled = false
			dep.Provided = false
			changed = true
			break
		}
	}
	return changed
}

func LoadVerifiedMappings() map[string]string {
	mappingsOnce.Do(func() {
		verifiedMap = map[string]string{}
		_ = json.Unmarshal(verifiedMappingsJSON, &verifiedMap)
	})
	copied := make(map[string]string, len(verifiedMap))
	for key, value := range verifiedMap {
		copied[key] = value
	}
	return copied
}
