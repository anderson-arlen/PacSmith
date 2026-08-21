package inspect

import (
	"encoding/binary"
	"fmt"
	"io"
	"path/filepath"
)

func analyzeELF(path string) (Analysis, error) {
	file, err := openRegular(path)
	if err != nil {
		return Analysis{}, err
	}
	defer file.Close()

	header := make([]byte, 64)
	n, err := io.ReadFull(file, header)
	if err != nil && err != io.ErrUnexpectedEOF && err != io.EOF {
		return Analysis{}, err
	}
	header = header[:n]
	if len(header) < 20 || !bytesHasELF(header) {
		return Analysis{}, fmt.Errorf("Source is not a usable ELF executable")
	}
	littleEndian := header[5] == 1
	var machine uint16
	if littleEndian {
		machine = binary.LittleEndian.Uint16(header[18:20])
	} else {
		machine = binary.BigEndian.Uint16(header[18:20])
	}
	architecture := "unknown"
	switch machine {
	case 62:
		architecture = "amd64"
	case 183:
		architecture = "arm64"
	}
	if architecture == "unknown" {
		return Analysis{}, fmt.Errorf("Unsupported ELF machine type %d", machine)
	}

	info, err := file.Stat()
	if err != nil {
		return Analysis{}, err
	}

	var result Analysis
	result.Type = SourceELF
	inferNameVersion(path, &result.Metadata)
	result.Metadata.Architecture = architecture
	filename := filepath.Base(path)
	result.Install.BinarySourcePath = filename
	result.Install.BinaryDestination = "/usr/bin/" + result.Metadata.Package
	result.Install.Launchers = []LauncherMapping{{
		Enabled:           true,
		SourcePath:        filename,
		CommandName:       result.Metadata.Package,
		Destination:       result.Install.BinaryDestination,
		SourceFingerprint: sha256Hex(header),
		Provenance:        deterministicProvenance("", "Standalone executable selected as the package command"),
	}}
	result.Payload = []PayloadEntry{{
		Path: filename,
		Type: "file",
		Size: info.Size(),
	}}
	return result, nil
}
