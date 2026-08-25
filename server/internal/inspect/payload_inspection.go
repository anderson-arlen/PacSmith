package inspect

import (
	"bytes"
	"crypto/sha256"
	"debug/elf"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf8"
)

const maxInspectedPayloadBytes = 256 << 20

type ELFInspection struct {
	Class                   string                       `json:"class"`
	Endianness              string                       `json:"endianness"`
	Version                 string                       `json:"version"`
	OSABI                   string                       `json:"osabi"`
	ABIVersion              uint8                        `json:"abi_version"`
	Type                    string                       `json:"type"`
	Machine                 string                       `json:"machine"`
	Entry                   uint64                       `json:"entry"`
	Interpreter             string                       `json:"interpreter,omitempty"`
	Needed                  []string                     `json:"needed"`
	SONAME                  []string                     `json:"soname"`
	RPath                   []string                     `json:"rpath"`
	RunPath                 []string                     `json:"runpath"`
	BuildID                 string                       `json:"build_id,omitempty"`
	PIE                     bool                         `json:"pie"`
	Stripped                bool                         `json:"stripped"`
	RELRO                   bool                         `json:"relro"`
	BindNow                 bool                         `json:"bind_now"`
	ExecutableStack         *bool                        `json:"executable_stack,omitempty"`
	ProgramHeaders          []ELFProgramHeaderInspection `json:"program_headers"`
	ProgramHeadersTruncated bool                         `json:"program_headers_truncated,omitempty"`
	Sections                []ELFSectionInspection       `json:"sections"`
	SectionsTruncated       bool                         `json:"sections_truncated,omitempty"`
}

type ELFProgramHeaderInspection struct {
	Type            string `json:"type"`
	Flags           string `json:"flags"`
	Offset          uint64 `json:"offset"`
	VirtualAddress  uint64 `json:"virtual_address"`
	PhysicalAddress uint64 `json:"physical_address"`
	FileSize        uint64 `json:"file_size"`
	MemorySize      uint64 `json:"memory_size"`
	Align           uint64 `json:"align"`
}

type ELFSectionInspection struct {
	Name    string `json:"name"`
	Type    string `json:"type"`
	Flags   string `json:"flags"`
	Address uint64 `json:"address"`
	Offset  uint64 `json:"offset"`
	Size    uint64 `json:"size"`
}

type PayloadFileInspection struct {
	Path             string         `json:"path"`
	Type             string         `json:"type"`
	Mode             string         `json:"mode"`
	Size             int64          `json:"size"`
	Executable       bool           `json:"executable"`
	SymlinkTarget    string         `json:"symlink_target,omitempty"`
	HardlinkTarget   string         `json:"hardlink_target,omitempty"`
	SHA256           string         `json:"sha256,omitempty"`
	MIME             string         `json:"mime,omitempty"`
	MagicHex         string         `json:"magic_hex,omitempty"`
	Text             string         `json:"text,omitempty"`
	TextTruncated    bool           `json:"text_truncated,omitempty"`
	ELF              *ELFInspection `json:"elf,omitempty"`
	InspectionNotice string         `json:"inspection_notice,omitempty"`
}

// InspectPayloadFile statically examines one already-inspected payload member.
// Package content is never executed or exposed as a temporary client-side artifact.
func InspectPayloadFile(artifactPath, originalFilename, member string) (PayloadFileInspection, error) {
	return inspectPayloadFile(artifactPath, originalFilename, member, map[string]struct{}{})
}

func inspectPayloadFile(
	artifactPath, originalFilename, member string,
	visited map[string]struct{},
) (PayloadFileInspection, error) {
	want, ok := NormalizedArchivePath(member)
	if !ok || want == "" {
		return PayloadFileInspection{}, fmt.Errorf("invalid payload path")
	}
	if _, seen := visited[want]; seen {
		return PayloadFileInspection{}, fmt.Errorf("payload hardlink cycle at %s", want)
	}
	visited[want] = struct{}{}
	sourceType, err := Detect(artifactPath)
	if err != nil {
		return PayloadFileInspection{}, err
	}
	if sourceType == SourceELF {
		name, nameOK := NormalizedArchivePath(filepath.Base(originalFilename))
		if !nameOK || name != want {
			return PayloadFileInspection{}, fmt.Errorf("payload file %s was not found", want)
		}
		info, err := os.Stat(artifactPath)
		if err != nil {
			return PayloadFileInspection{}, err
		}
		if info.Size() > maxInspectedPayloadBytes {
			return PayloadFileInspection{}, fmt.Errorf("payload file exceeds the inspection safety limit")
		}
		data, err := os.ReadFile(artifactPath)
		if err != nil {
			return PayloadFileInspection{}, err
		}
		return inspectPayloadBytes(PayloadFileInspection{
			Path: want, Type: "file", Mode: "0755", Size: info.Size(), Executable: true,
		}, data), nil
	}

	collector := payloadInspectionCollector{want: want}
	switch sourceType {
	case SourceDebian:
		err = walkDebData(artifactPath, collector.walk)
	case SourceRPM:
		err = walkRPMFiles(artifactPath, collector.walk)
	case SourceArchive, SourceArchPackage:
		err = walkArchiveFiles(artifactPath, collector.walk)
	case SourceAppImage:
		err = walkAppImageFiles(artifactPath, collector.walk)
	default:
		return PayloadFileInspection{}, fmt.Errorf("cannot inspect payload files from this artifact type")
	}
	if err != nil && !errors.Is(err, errStopWalk) {
		return PayloadFileInspection{}, err
	}
	if !collector.found {
		return PayloadFileInspection{}, fmt.Errorf("payload file %s was not found", want)
	}
	if collector.result.Type != "file" {
		return collector.result, nil
	}
	if collector.result.HardlinkTarget != "" {
		target, ok := NormalizedArchivePath(collector.result.HardlinkTarget)
		if !ok || target == "" {
			return PayloadFileInspection{}, fmt.Errorf("invalid hardlink target for %s", want)
		}
		targetResult, err := inspectPayloadFile(artifactPath, originalFilename, target, visited)
		if err != nil {
			return PayloadFileInspection{}, err
		}
		collector.result.Size = targetResult.Size
		collector.result.SHA256 = targetResult.SHA256
		collector.result.MIME = targetResult.MIME
		collector.result.MagicHex = targetResult.MagicHex
		collector.result.Text = targetResult.Text
		collector.result.TextTruncated = targetResult.TextTruncated
		collector.result.ELF = targetResult.ELF
		collector.result.InspectionNotice = targetResult.InspectionNotice
		return collector.result, nil
	}
	return inspectPayloadBytes(collector.result, collector.data), nil
}

type payloadInspectionCollector struct {
	want   string
	found  bool
	result PayloadFileInspection
	data   []byte
}

func (c *payloadInspectionCollector) walk(entry walkedEntry, body io.Reader) error {
	path, ok := NormalizedArchivePath(entry.RawName)
	if !ok || path != c.want {
		_, _ = io.Copy(io.Discard, body)
		return nil
	}
	c.found = true
	c.result = PayloadFileInspection{
		Path:           path,
		Type:           entryTypeName(entry.Kind),
		Mode:           fmt.Sprintf("%04o", entry.Mode&07777),
		Size:           max(entry.Size, 0),
		Executable:     hasExecBit(entry.Mode),
		SymlinkTarget:  entry.SymlinkTarget,
		HardlinkTarget: entry.HardlinkTarget,
	}
	if entry.Kind != kindFile {
		_, _ = io.Copy(io.Discard, body)
		return errStopWalk
	}
	if entry.Size > maxInspectedPayloadBytes {
		c.result.InspectionNotice = "File exceeds the 256 MiB static-inspection limit"
		_, _ = io.Copy(io.Discard, body)
		return errStopWalk
	}
	data, err := readLimited(body, maxInspectedPayloadBytes)
	if err != nil {
		return err
	}
	c.data = data
	return errStopWalk
}

func inspectPayloadBytes(result PayloadFileInspection, data []byte) PayloadFileInspection {
	sum := sha256.Sum256(data)
	result.SHA256 = hex.EncodeToString(sum[:])
	if len(data) > 0 {
		magicSize := min(len(data), 32)
		result.MagicHex = hex.EncodeToString(data[:magicSize])
		result.MIME = http.DetectContentType(data[:min(len(data), 512)])
	}
	if elfFile, err := elf.NewFile(bytes.NewReader(data)); err == nil {
		result.Executable = true
		result.ELF = inspectELF(elfFile)
		return result
	}
	if utf8.Valid(data) && !bytes.ContainsRune(data, '\x00') {
		const textLimit = 256 << 10
		preview := data
		if len(preview) > textLimit {
			preview = preview[:textLimit]
			result.TextTruncated = true
		}
		result.Text = string(preview)
	}
	return result
}

func inspectELF(file *elf.File) *ELFInspection {
	result := &ELFInspection{
		Class:      file.Class.String(),
		Endianness: file.Data.String(),
		Version:    file.Version.String(),
		OSABI:      file.OSABI.String(),
		ABIVersion: file.ABIVersion,
		Type:       file.Type.String(),
		Machine:    file.Machine.String(),
		Entry:      file.Entry,
		Needed:     elfDynamicStrings(file, elf.DT_NEEDED),
		SONAME:     elfDynamicStrings(file, elf.DT_SONAME),
		RPath:      elfDynamicStrings(file, elf.DT_RPATH),
		RunPath:    elfDynamicStrings(file, elf.DT_RUNPATH),
		Stripped:   file.Section(".symtab") == nil,
	}
	const maximumProgramHeaders = 512
	for _, program := range file.Progs {
		if len(result.ProgramHeaders) < maximumProgramHeaders {
			result.ProgramHeaders = append(result.ProgramHeaders, ELFProgramHeaderInspection{
				Type: program.Type.String(), Flags: program.Flags.String(), Offset: program.Off,
				VirtualAddress: program.Vaddr, PhysicalAddress: program.Paddr,
				FileSize: program.Filesz, MemorySize: program.Memsz, Align: program.Align,
			})
		} else {
			result.ProgramHeadersTruncated = true
		}
		switch program.Type {
		case elf.PT_INTERP:
			body, err := io.ReadAll(io.LimitReader(program.Open(), 4096))
			if err == nil {
				result.Interpreter = strings.TrimRight(string(body), "\x00")
			}
		case elf.PT_GNU_RELRO:
			result.RELRO = true
		case elf.PT_GNU_STACK:
			executable := program.Flags&elf.PF_X != 0
			result.ExecutableStack = &executable
		}
	}
	const maximumSections = 2048
	for _, section := range file.Sections {
		if len(result.Sections) >= maximumSections {
			result.SectionsTruncated = true
			break
		}
		result.Sections = append(result.Sections, ELFSectionInspection{
			Name: section.Name, Type: section.Type.String(), Flags: section.Flags.String(),
			Address: section.Addr, Offset: section.Offset, Size: section.Size,
		})
	}
	result.BindNow = elfDynamicFlag(file, elf.DT_FLAGS, uint64(elf.DF_BIND_NOW)) ||
		elfDynamicFlag(file, elf.DT_FLAGS_1, uint64(elf.DF_1_NOW))
	result.PIE = file.Type == elf.ET_DYN && result.Interpreter != ""
	result.BuildID = elfBuildID(file)
	return result
}

func elfDynamicFlag(file *elf.File, tag elf.DynTag, flag uint64) bool {
	values, err := file.DynValue(tag)
	return err == nil && len(values) > 0 && values[0]&flag != 0
}

func elfDynamicStrings(file *elf.File, tag elf.DynTag) []string {
	values, err := file.DynString(tag)
	if err != nil || values == nil {
		return []string{}
	}
	return values
}

func elfBuildID(file *elf.File) string {
	section := file.Section(".note.gnu.build-id")
	if section == nil {
		return ""
	}
	data, err := section.Data()
	if err != nil || len(data) < 16 {
		return ""
	}
	order := file.ByteOrder
	namesz := int(order.Uint32(data[0:4]))
	descsz := int(order.Uint32(data[4:8]))
	noteType := order.Uint32(data[8:12])
	nameEnd := 12 + namesz
	descStart := 12 + align4(namesz)
	descEnd := descStart + descsz
	if noteType != 3 || namesz < 3 || nameEnd > len(data) || descEnd > len(data) ||
		!bytes.HasPrefix(data[12:nameEnd], []byte("GNU")) {
		return ""
	}
	return hex.EncodeToString(data[descStart:descEnd])
}

func align4(value int) int { return (value + 3) &^ 3 }
