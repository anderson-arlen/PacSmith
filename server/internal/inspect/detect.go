// Package inspect analyzes untrusted vendor artifacts without executing them.
// Imported binaries, AppRun, maintainer scripts, rpm(1), and FUSE are never run.
package inspect

import (
	"bytes"
	"fmt"
	"io"
	"path/filepath"
	"strings"
)

func Detect(path string) (SourceType, error) {
	file, err := openRegular(path)
	if err != nil {
		return SourceUnknown, err
	}
	defer file.Close()

	magic := make([]byte, 12)
	n, err := io.ReadFull(file, magic)
	if err == io.ErrUnexpectedEOF {
		magic = magic[:n]
	} else if err != nil && err != io.EOF {
		return SourceUnknown, err
	} else if err == io.EOF {
		magic = magic[:n]
	}

	fileName := filepath.Base(path)
	appImageMagic := len(magic) >= 11 && bytesHasELF(magic) &&
		magic[8] == 'A' && magic[9] == 'I' && (magic[10] == 1 || magic[10] == 2)
	if appImageMagic {
		if magic[10] == 2 {
			return SourceAppImage, nil
		}
		return SourceUnknown, fmt.Errorf("Type 1 AppImages are not supported; PacSmith currently decomposes Type 2 SquashFS AppImages only")
	}
	if strings.HasSuffix(strings.ToLower(fileName), ".appimage") {
		return SourceUnknown, fmt.Errorf("The .AppImage extension is present, but the file has no valid AppImage type marker")
	}
	if bytesHasELF(magic) {
		return SourceELF, nil
	}
	if bytes.HasPrefix(magic, []byte("!<arch>\n")) {
		return SourceDebian, nil
	}
	if len(magic) >= 4 && magic[0] == 0xed && magic[1] == 0xab && magic[2] == 0xee && magic[3] == 0xdb {
		return SourceRPM, nil
	}

	archiveDiagnostic := ""
	archPkg, err := archiveContainsPKGINFO(path)
	if err != nil {
		archiveDiagnostic = err.Error()
		return SourceUnknown, fmt.Errorf("Artifact '%s' is not a recognized DEB, RPM, Arch package, Type 2 AppImage, supported archive, or standalone ELF file. Header bytes: %s.%s",
			fileName, spacedHex(magic), archiveSuffix(archiveDiagnostic))
	}
	if archPkg {
		return SourceArchPackage, nil
	}
	return SourceArchive, nil
}

func archiveContainsPKGINFO(path string) (bool, error) {
	found := false
	walk := func(entry walkedEntry, body io.Reader) error {
		normalized, ok := NormalizedArchivePath(entry.RawName)
		if ok && normalized == ".PKGINFO" {
			found = true
		}
		_, _ = io.Copy(io.Discard, body)
		return nil
	}
	if err := walkTarFile(path, path, walk); err == nil {
		return found, nil
	} else {
		tarErr := err
		if zipErr := walkZipFile(path, walk); zipErr == nil {
			return found, nil
		}
		return false, tarErr
	}
}

func spacedHex(data []byte) string {
	if len(data) == 0 {
		return ""
	}
	parts := make([]string, len(data))
	for i, b := range data {
		parts[i] = fmt.Sprintf("%02x", b)
	}
	return strings.Join(parts, " ")
}

func archiveSuffix(diagnostic string) string {
	if diagnostic == "" {
		return ""
	}
	return " archive: " + diagnostic
}

func Analyze(path string) (Analysis, error) {
	sourceType, err := Detect(path)
	if err != nil {
		return Analysis{}, err
	}
	var analysis Analysis
	switch sourceType {
	case SourceDebian:
		analysis, err = analyzeDEB(path)
	case SourceAppImage:
		analysis, err = analyzeAppImage(path)
	case SourceELF:
		analysis, err = analyzeELF(path)
	case SourceRPM:
		analysis, err = analyzeRPM(path)
	case SourceArchPackage:
		analysis, err = analyzeArchive(path, true)
	default:
		analysis, err = analyzeArchive(path, false)
	}
	if err != nil {
		return Analysis{}, err
	}
	bindDefaultPayloadRuleFingerprints(&analysis)
	return analysis, nil
}
