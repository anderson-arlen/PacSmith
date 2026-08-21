package inspect

import (
	"fmt"
	"io"
	"os"
	"regexp"
	"sort"
	"strings"
)

const (
	rpmLeadSize          = 96
	maximumHeaderEntries = 131072
	maximumHeaderStore   = 64 * 1024 * 1024
)

const (
	rpmTypeInt8        = 2
	rpmTypeInt16       = 3
	rpmTypeInt32       = 4
	rpmTypeInt64       = 5
	rpmTypeString      = 6
	rpmTypeBinary      = 7
	rpmTypeStringArray = 8
	rpmTypeI18nString  = 9
)

type rpmHeaderEntry struct {
	tag    uint32
	typ    uint32
	offset uint32
	count  uint32
}

type rpmHeaderSection struct {
	entries   []rpmHeaderEntry
	store     []byte
	endOffset int64
}

func (h rpmHeaderSection) entry(tag uint32) *rpmHeaderEntry {
	for i := range h.entries {
		if h.entries[i].tag == tag {
			return &h.entries[i]
		}
	}
	return nil
}

func analyzeRPMHeader(path string) (rpmHeaderAnalysis, error) {
	file, err := os.Open(path)
	if err != nil {
		return rpmHeaderAnalysis{}, err
	}
	defer file.Close()

	lead := make([]byte, rpmLeadSize)
	if _, err := io.ReadFull(file, lead); err != nil {
		return rpmHeaderAnalysis{}, fmt.Errorf("Source does not have an RPM package lead")
	}
	if lead[0] != 0xed || lead[1] != 0xab || lead[2] != 0xee || lead[3] != 0xdb {
		return rpmHeaderAnalysis{}, fmt.Errorf("Source does not have an RPM package lead")
	}
	signature, err := readRPMHeader(file, rpmLeadSize, "signature")
	if err != nil {
		return rpmHeaderAnalysis{}, err
	}
	mainOffset := (signature.endOffset + 7) & ^int64(7)
	header, err := readRPMHeader(file, mainOffset, "metadata")
	if err != nil {
		return rpmHeaderAnalysis{}, err
	}

	const (
		nameTag                     = 1000
		versionTag                  = 1001
		releaseTag                  = 1002
		epochTag                    = 1003
		summaryTag                  = 1004
		descriptionTag              = 1005
		buildHostTag                = 1007
		installedSizeTag            = 1009
		distributionTag             = 1010
		vendorTag                   = 1011
		licenseTag                  = 1014
		packagerTag                 = 1015
		groupTag                    = 1016
		urlTag                      = 1020
		osTag                       = 1021
		architectureTag             = 1022
		preinTag                    = 1023
		postinTag                   = 1024
		preunTag                    = 1025
		postunTag                   = 1026
		provideNameTag              = 1047
		sourceRpmTag                = 1044
		conflictNameTag             = 1054
		verifyScriptTag             = 1079
		triggerScriptsTag           = 1065
		pretransTag                 = 1151
		posttransTag                = 1152
		preuntransTag               = 5103
		postuntransTag              = 5104
		obsoleteNameTag             = 1090
		preinProgramTag             = 1085
		postinProgramTag            = 1086
		preunProgramTag             = 1087
		postunProgramTag            = 1088
		verifyProgramTag            = 1091
		triggerProgramTag           = 1092
		pretransProgramTag          = 1153
		posttransProgramTag         = 1154
		recommendNameTag            = 5046
		recommendVersionTag         = 5047
		recommendFlagsTag           = 5048
		suggestNameTag              = 5049
		suggestVersionTag           = 5050
		suggestFlagsTag             = 5051
		supplementNameTag           = 5052
		supplementVersionTag        = 5053
		supplementFlagsTag          = 5054
		enhanceNameTag              = 5055
		enhanceVersionTag           = 5056
		enhanceFlagsTag             = 5057
		fileTriggerScriptsTag       = 5066
		fileTriggerProgramsTag      = 5067
		transFileTriggerScriptsTag  = 5076
		transFileTriggerProgramsTag = 5077
		preuntransProgramTag        = 5105
		postuntransProgramTag       = 5106
		sysusersTag                 = 5109
		payloadFormatTag            = 1124
		payloadCompressorTag        = 1125
	)

	var result rpmHeaderAnalysis
	result.Metadata.RawFields = map[string]string{}
	result.Metadata.Package = rpmString(header, nameTag)
	version := rpmString(header, versionTag)
	release := rpmString(header, releaseTag)
	if release != "" {
		version += "-" + release
	}
	epochs := rpmIntegers(header, epochTag)
	if len(epochs) > 0 && epochs[0] > 0 {
		version = fmt.Sprintf("%d:%s", epochs[0], version)
	}
	result.Metadata.Version = version
	result.Metadata.Architecture = rpmString(header, architectureTag)
	result.Metadata.Maintainer = rpmString(header, packagerTag)
	if result.Metadata.Maintainer == "" {
		result.Metadata.Maintainer = rpmString(header, vendorTag)
	}
	result.Metadata.Description = rpmString(header, summaryTag)
	longDescription := rpmString(header, descriptionTag)
	if longDescription != "" && longDescription != result.Metadata.Description {
		result.Metadata.Description += "\n" + longDescription
	}
	result.Metadata.Homepage = rpmString(header, urlTag)
	result.Dependencies = rpmDependencies(header)
	var expressions []string
	for _, dep := range result.Dependencies {
		expressions = append(expressions, dep.RawExpression)
	}
	result.Metadata.Depends = strings.Join(expressions, ", ")
	result.Metadata.Conflicts = rpmJoinStrings(header, conflictNameTag)
	result.Metadata.Provides = rpmJoinStrings(header, provideNameTag)
	result.Metadata.Recommends = rpmJoinVersioned(header, recommendNameTag, recommendVersionTag, recommendFlagsTag)
	result.Metadata.Suggests = rpmJoinVersioned(header, suggestNameTag, suggestVersionTag, suggestFlagsTag)
	put := func(key, value string) { result.Metadata.RawFields[key] = value }
	put("RPM-Name", result.Metadata.Package)
	put("RPM-Version", rpmString(header, versionTag))
	put("RPM-Release", release)
	put("Architecture", result.Metadata.Architecture)
	put("Vendor", rpmString(header, vendorTag))
	put("Packager", rpmString(header, packagerTag))
	put("License", rpmString(header, licenseTag))
	put("Group", rpmString(header, groupTag))
	put("Distribution", rpmString(header, distributionTag))
	put("Build-Host", rpmString(header, buildHostTag))
	put("Operating-System", rpmString(header, osTag))
	put("Source-RPM", rpmString(header, sourceRpmTag))
	installedSizes := rpmIntegers(header, installedSizeTag)
	if len(installedSizes) == 0 {
		put("Installed-Size", "")
	} else {
		put("Installed-Size", fmt.Sprintf("%d", installedSizes[0]))
	}
	put("URL", result.Metadata.Homepage)
	put("Summary", rpmString(header, summaryTag))
	put("Description", longDescription)
	put("Requires", result.Metadata.Depends)
	put("Provides", result.Metadata.Provides)
	put("Conflicts", result.Metadata.Conflicts)
	put("Recommends", result.Metadata.Recommends)
	put("Suggests", result.Metadata.Suggests)
	put("Supplements", rpmJoinVersioned(header, supplementNameTag, supplementVersionTag, supplementFlagsTag))
	put("Enhances", rpmJoinVersioned(header, enhanceNameTag, enhanceVersionTag, enhanceFlagsTag))
	put("Obsoletes", rpmJoinStrings(header, obsoleteNameTag))
	put("Pre-Install-Interpreter", rpmJoinStrings(header, preinProgramTag))
	put("Post-Install-Interpreter", rpmJoinStrings(header, postinProgramTag))
	put("Pre-Uninstall-Interpreter", rpmJoinStrings(header, preunProgramTag))
	put("Post-Uninstall-Interpreter", rpmJoinStrings(header, postunProgramTag))
	put("Verify-Script-Interpreter", rpmJoinStrings(header, verifyProgramTag))
	put("Trigger-Script-Interpreters", rpmJoinStrings(header, triggerProgramTag))
	put("Pre-Transaction-Interpreter", rpmJoinStrings(header, pretransProgramTag))
	put("Post-Transaction-Interpreter", rpmJoinStrings(header, posttransProgramTag))
	put("File-Trigger-Interpreters", rpmJoinStrings(header, fileTriggerProgramsTag))
	put("Transaction-File-Trigger-Interpreters", rpmJoinStrings(header, transFileTriggerProgramsTag))
	put("Pre-Uninstall-Transaction-Interpreter", rpmJoinStrings(header, preuntransProgramTag))
	put("Post-Uninstall-Transaction-Interpreter", rpmJoinStrings(header, postuntransProgramTag))
	put("RPM-Payload-Format", rpmString(header, payloadFormatTag))
	put("RPM-Payload-Compressor", rpmString(header, payloadCompressorTag))
	result.payloadFormat = rpmString(header, payloadFormatTag)
	result.payloadCompressor = rpmString(header, payloadCompressorTag)
	result.payloadOffset = header.endOffset
	result.FileCapabilities = rpmFileCapabilities(header)

	appendRPMScript(&result.MaintainerScripts, header, preinTag, "prein")
	appendRPMScript(&result.MaintainerScripts, header, postinTag, "postin")
	appendRPMScript(&result.MaintainerScripts, header, preunTag, "preun")
	appendRPMScript(&result.MaintainerScripts, header, postunTag, "postun")
	appendRPMScript(&result.MaintainerScripts, header, pretransTag, "pretrans")
	appendRPMScript(&result.MaintainerScripts, header, posttransTag, "posttrans")
	appendRPMScript(&result.MaintainerScripts, header, preuntransTag, "preuntrans")
	appendRPMScript(&result.MaintainerScripts, header, postuntransTag, "postuntrans")
	appendRPMScript(&result.MaintainerScripts, header, verifyScriptTag, "verify")
	triggers := rpmStrings(header, triggerScriptsTag)
	for i, value := range triggers {
		if strings.TrimSpace(value) != "" {
			result.MaintainerScripts = append(result.MaintainerScripts, MaintainerScript{
				Name:     fmt.Sprintf("trigger-%d", i+1),
				Contents: value,
			})
		}
	}
	appendRPMScripts(&result.MaintainerScripts, header, fileTriggerScriptsTag, "file-trigger")
	appendRPMScripts(&result.MaintainerScripts, header, transFileTriggerScriptsTag, "transaction-file-trigger")
	appendRPMScripts(&result.MaintainerScripts, header, sysusersTag, "sysusers")
	result.ScriptFindings = analyzeScriptEvidence(result.MaintainerScripts).Findings
	if result.Metadata.Package == "" || result.Metadata.Version == "" || result.Metadata.Architecture == "" {
		return rpmHeaderAnalysis{}, fmt.Errorf("RPM metadata is missing the package name, version, or architecture")
	}
	return result, nil
}

func readRPMHeader(file *os.File, offset int64, description string) (rpmHeaderSection, error) {
	if _, err := file.Seek(offset, io.SeekStart); err != nil {
		return rpmHeaderSection{}, fmt.Errorf("Could not seek to the RPM %s header", description)
	}
	prefix := make([]byte, 16)
	if _, err := io.ReadFull(file, prefix); err != nil || prefix[0] != 0x8e || prefix[1] != 0xad || prefix[2] != 0xe8 || prefix[3] != 0x01 {
		return rpmHeaderSection{}, fmt.Errorf("Invalid RPM %s header magic", description)
	}
	entryCount, ok1 := readBE32(prefix, 8)
	storeSize, ok2 := readBE32(prefix, 12)
	if !ok1 || !ok2 || entryCount > maximumHeaderEntries || storeSize > maximumHeaderStore {
		return rpmHeaderSection{}, fmt.Errorf("RPM %s header exceeds the safety limit", description)
	}
	indexBytes := uint64(entryCount) * 16
	bodyBytes := indexBytes + uint64(storeSize)
	info, err := file.Stat()
	if err != nil {
		return rpmHeaderSection{}, err
	}
	if offset > info.Size()-16 || bodyBytes > uint64(info.Size()-offset-16) {
		return rpmHeaderSection{}, fmt.Errorf("Truncated RPM %s header", description)
	}
	body := make([]byte, bodyBytes)
	if _, err := io.ReadFull(file, body); err != nil {
		return rpmHeaderSection{}, fmt.Errorf("Could not read the complete RPM %s header", description)
	}
	var result rpmHeaderSection
	result.entries = make([]rpmHeaderEntry, 0, entryCount)
	for index := uint32(0); index < entryCount; index++ {
		position := int(index) * 16
		tag, okTag := readBE32(body, position)
		typ, okType := readBE32(body, position+4)
		valueOffset, okOff := readBE32(body, position+8)
		count, okCount := readBE32(body, position+12)
		if !okTag || !okType || !okOff || !okCount || valueOffset > storeSize || count > maximumHeaderEntries {
			return rpmHeaderSection{}, fmt.Errorf("Invalid RPM %s header index", description)
		}
		result.entries = append(result.entries, rpmHeaderEntry{tag, typ, valueOffset, count})
	}
	result.store = body[indexBytes : indexBytes+uint64(storeSize)]
	result.endOffset = offset + 16 + int64(bodyBytes)
	return result, nil
}

func rpmStrings(header rpmHeaderSection, tag uint32) []string {
	entry := header.entry(tag)
	if entry == nil || (entry.typ != rpmTypeString && entry.typ != rpmTypeStringArray && entry.typ != rpmTypeI18nString) ||
		int(entry.offset) >= len(header.store) {
		return nil
	}
	count := entry.count
	if entry.typ == rpmTypeString {
		count = 1
	}
	var result []string
	position := int(entry.offset)
	for index := uint32(0); index < count && position < len(header.store); index++ {
		end := position
		for end < len(header.store) && header.store[end] != 0 {
			end++
		}
		if end >= len(header.store) {
			return nil
		}
		result = append(result, string(header.store[position:end]))
		position = end + 1
	}
	return result
}

func rpmString(header rpmHeaderSection, tag uint32) string {
	values := rpmStrings(header, tag)
	if len(values) == 0 {
		return ""
	}
	return values[0]
}

func rpmIntegers(header rpmHeaderSection, tag uint32) []uint64 {
	entry := header.entry(tag)
	if entry == nil || int(entry.offset) > len(header.store) {
		return nil
	}
	var width int
	switch entry.typ {
	case rpmTypeInt8:
		width = 1
	case rpmTypeInt16:
		width = 2
	case rpmTypeInt32:
		width = 4
	case rpmTypeInt64:
		width = 8
	default:
		return nil
	}
	required := uint64(entry.count) * uint64(width)
	if required > uint64(len(header.store))-uint64(entry.offset) {
		return nil
	}
	result := make([]uint64, 0, entry.count)
	position := int(entry.offset)
	for index := uint32(0); index < entry.count; index++ {
		var value uint64
		for b := 0; b < width; b++ {
			value = value<<8 | uint64(header.store[position+b])
		}
		result = append(result, value)
		position += width
	}
	return result
}

func rpmRelation(flags uint64) string {
	const less = 1 << 1
	const greater = 1 << 2
	const equal = 1 << 3
	switch {
	case flags&greater != 0 && flags&equal != 0:
		return ">="
	case flags&less != 0 && flags&equal != 0:
		return "<="
	case flags&greater != 0:
		return ">"
	case flags&less != 0:
		return "<"
	case flags&equal != 0:
		return "="
	default:
		return ""
	}
}

var rpmPackageName = regexp.MustCompile(`^[A-Za-z0-9@._+\-]+$`)
var rpmRichOr = regexp.MustCompile(`\s+or\s+`)

func rpmDependencies(header rpmHeaderSection) []Dependency {
	const requireNameTag = 1049
	const requireFlagsTag = 1048
	const requireVersionTag = 1050
	names := rpmStrings(header, requireNameTag)
	versions := rpmStrings(header, requireVersionTag)
	flags := rpmIntegers(header, requireFlagsTag)
	const scriptRequirementMask = (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13)
	var result []Dependency
	seen := map[string]struct{}{}
	for index, name := range names {
		name = strings.TrimSpace(name)
		var requirementFlags uint64
		if index < len(flags) {
			requirementFlags = flags[index]
		}
		if name == "" || strings.HasPrefix(name, "rpmlib(") || strings.HasPrefix(name, "/") ||
			requirementFlags&scriptRequirementMask != 0 {
			continue
		}
		version := ""
		if index < len(versions) {
			version = strings.TrimSpace(versions[index])
		}
		comparison := rpmRelation(requirementFlags)
		raw := name
		if comparison != "" && version != "" {
			raw = fmt.Sprintf("%s (%s %s)", name, comparison, version)
		}
		if _, ok := seen[raw]; ok {
			continue
		}
		seen[raw] = struct{}{}
		dep := Dependency{RawExpression: raw}
		alternativeText := name
		if strings.HasPrefix(alternativeText, "(") && strings.HasSuffix(alternativeText, ")") {
			alternativeText = alternativeText[1 : len(alternativeText)-1]
		}
		for _, alternative := range rpmRichOr.Split(alternativeText, -1) {
			candidate := strings.TrimSpace(alternative)
			if rpmPackageName.MatchString(candidate) {
				dep.Alternatives = append(dep.Alternatives, DependencyAlternative{
					PackageName:     candidate,
					VersionOperator: comparison,
					Version:         version,
				})
			}
		}
		if len(dep.Alternatives) == 0 {
			dep.Alternatives = append(dep.Alternatives, DependencyAlternative{
				PackageName:     name,
				VersionOperator: comparison,
				Version:         version,
			})
		}
		result = append(result, dep)
	}
	_ = ApplyVerifiedMappings(result, LoadVerifiedMappings())
	return result
}

func appendRPMScript(scripts *[]MaintainerScript, header rpmHeaderSection, tag uint32, name string) {
	contents := rpmString(header, tag)
	if strings.TrimSpace(contents) != "" {
		*scripts = append(*scripts, MaintainerScript{Name: name, Contents: contents})
	}
}

func appendRPMScripts(scripts *[]MaintainerScript, header rpmHeaderSection, tag uint32, prefix string) {
	values := rpmStrings(header, tag)
	for i, value := range values {
		if strings.TrimSpace(value) != "" {
			*scripts = append(*scripts, MaintainerScript{
				Name:     fmt.Sprintf("%s-%d", prefix, i+1),
				Contents: value,
			})
		}
	}
}

func rpmJoinVersioned(header rpmHeaderSection, nameTag, versionTag, flagsTag uint32) string {
	names := rpmStrings(header, nameTag)
	versions := rpmStrings(header, versionTag)
	flags := rpmIntegers(header, flagsTag)
	var result []string
	for index, name := range names {
		name = strings.TrimSpace(name)
		if name == "" {
			continue
		}
		version := ""
		if index < len(versions) {
			version = strings.TrimSpace(versions[index])
		}
		var flag uint64
		if index < len(flags) {
			flag = flags[index]
		}
		comparison := rpmRelation(flag)
		if comparison == "" || version == "" {
			result = append(result, name)
		} else {
			result = append(result, fmt.Sprintf("%s (%s %s)", name, comparison, version))
		}
	}
	return strings.Join(result, ", ")
}

func rpmJoinStrings(header rpmHeaderSection, tag uint32) string {
	return strings.Join(rpmStrings(header, tag), ", ")
}

func rpmFileCapabilities(header rpmHeaderSection) map[string]string {
	const directoryIndexesTag = 1116
	const baseNamesTag = 1117
	const directoryNamesTag = 1118
	const capabilitiesTag = 5010
	directoryIndexes := rpmIntegers(header, directoryIndexesTag)
	baseNames := rpmStrings(header, baseNamesTag)
	directoryNames := rpmStrings(header, directoryNamesTag)
	capabilities := rpmStrings(header, capabilitiesTag)
	count := min(len(directoryIndexes), len(baseNames), len(capabilities))
	result := map[string]string{}
	for index := 0; index < count; index++ {
		capability := strings.TrimSpace(capabilities[index])
		directoryIndex := directoryIndexes[index]
		if capability == "" || directoryIndex >= uint64(len(directoryNames)) {
			continue
		}
		path := directoryNames[directoryIndex] + baseNames[index]
		path = strings.TrimPrefix(path, "/")
		if safe, ok := NormalizedArchivePath(path); ok && safe != "" {
			result[safe] = capability
		}
	}
	return result
}

func analyzeRPM(path string) (Analysis, error) {
	header, err := analyzeRPMHeader(path)
	if err != nil {
		return Analysis{}, err
	}
	result, payloadErr := walkRPMPayload(path, header)
	if payloadErr != nil {
		info, _ := os.Stat(path)
		if info != nil && header.payloadOffset >= info.Size() {
			result = Analysis{}
		} else if isEmptyPayload(path, header.payloadOffset) {
			result = Analysis{}
		} else {
			return Analysis{}, payloadErr
		}
	}
	result.Type = SourceRPM
	result.Metadata = header.Metadata
	result.Dependencies = header.Dependencies
	result.MaintainerScripts = header.MaintainerScripts
	result.ScriptFindings = header.ScriptFindings
	for i := range result.Payload {
		entry := &result.Payload[i]
		capability := header.FileCapabilities[entry.Path]
		if capability == "" {
			continue
		}
		entry.ReviewReason = appendReason(entry.ReviewReason, "Linux file capabilities '"+capability+"' require review")
		entry.RequiresReview = true
		exists := false
		for _, rule := range result.PayloadRules {
			if rule.Path == entry.Path {
				exists = true
				break
			}
		}
		if !exists {
			result.PayloadRules = append(result.PayloadRules, PayloadRule{
				Path:   entry.Path,
				Reason: entry.ReviewReason,
			})
		}
	}
	scriptEvidence := analyzeScriptEvidence(result.MaintainerScripts)
	result.RPMCandidates = append(result.RPMCandidates, scriptEvidence.RPMCandidates...)
	result.AptCandidates = append(result.AptCandidates, scriptEvidence.AptCandidates...)
	result.SigningKeys = append(result.SigningKeys, scriptEvidence.SigningKeys...)
	for _, script := range result.MaintainerScripts {
		result.UpdateCandidates = append(result.UpdateCandidates, URLsFromText(script.Contents)...)
	}
	result.Install.ArchiveLayout = LayoutPreserveRoot
	result.UpdateCandidates = uniqueStrings(result.UpdateCandidates)
	sort.Strings(result.UpdateCandidates)
	deduplicateEvidence(&result)
	return result, nil
}

func isEmptyPayload(path string, offset int64) bool {
	info, err := os.Stat(path)
	if err != nil {
		return false
	}
	return offset >= info.Size()
}

func walkRPMPayload(path string, header rpmHeaderAnalysis) (Analysis, error) {
	file, err := os.Open(path)
	if err != nil {
		return Analysis{}, err
	}
	defer file.Close()
	if header.payloadOffset > 0 {
		if _, err := file.Seek(header.payloadOffset, io.SeekStart); err != nil {
			return Analysis{}, err
		}
	}
	decoded, err := decodePayload(file, header.payloadCompressor)
	if err != nil {
		return Analysis{}, err
	}
	defer decoded.Close()
	format := strings.ToLower(header.payloadFormat)
	switch format {
	case "", "cpio":
		return walkArchiveStream(path, false, func(fn func(walkedEntry, io.Reader) error) error {
			return walkCPIO(decoded, fn)
		})
	case "tar":
		return walkArchiveStream(path, false, func(fn func(walkedEntry, io.Reader) error) error {
			return walkTarReader(decoded, fn)
		})
	default:
		return walkArchiveStream(path, false, func(fn func(walkedEntry, io.Reader) error) error {
			return walkCPIO(decoded, fn)
		})
	}
}
