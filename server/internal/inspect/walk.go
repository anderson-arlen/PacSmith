package inspect

import (
	"archive/tar"
	"archive/zip"
	"fmt"
	"io"
	"os"
	"strings"
)

type archiveKind int

const (
	kindFile archiveKind = iota
	kindDir
	kindSymlink
	kindChar
	kindBlock
	kindFIFO
	kindSocket
	kindOther
)

type walkedEntry struct {
	RawName        string
	Size           int64
	Mode           int64
	Kind           archiveKind
	SymlinkTarget  string
	HardlinkTarget string
}

func openRegular(path string) (*os.File, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file", path)
	}
	return os.Open(path)
}

func walkTarReader(r io.Reader, fn func(walkedEntry, io.Reader) error) error {
	tr := tar.NewReader(r)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		entry := walkedEntry{
			RawName: hdr.Name,
			Size:    hdr.Size,
			Mode:    hdr.Mode,
		}
		switch hdr.Typeflag {
		case tar.TypeDir:
			entry.Kind = kindDir
		case tar.TypeSymlink:
			entry.Kind = kindSymlink
			entry.SymlinkTarget = hdr.Linkname
		case tar.TypeLink:
			entry.Kind = kindFile
			entry.HardlinkTarget = hdr.Linkname
		case tar.TypeChar:
			entry.Kind = kindChar
		case tar.TypeBlock:
			entry.Kind = kindBlock
		case tar.TypeFifo:
			entry.Kind = kindFIFO
		case tar.TypeReg, tar.TypeRegA:
			entry.Kind = kindFile
		default:
			entry.Kind = kindOther
		}
		if err := fn(entry, tr); err != nil {
			return err
		}
	}
}

func walkTarFile(path, hint string, fn func(walkedEntry, io.Reader) error) error {
	decoded, err := openDecompressed(path, hint)
	if err != nil {
		return err
	}
	defer decoded.Close()
	return walkTarReader(decoded, fn)
}

func walkZipFile(path string, fn func(walkedEntry, io.Reader) error) error {
	reader, err := zip.OpenReader(path)
	if err != nil {
		return err
	}
	defer reader.Close()
	for _, file := range reader.File {
		mode := file.Mode()
		entry := walkedEntry{
			RawName: file.Name,
			Size:    int64(file.UncompressedSize64),
			Mode:    int64(mode.Perm()),
		}
		if mode&os.ModeSetuid != 0 {
			entry.Mode |= 04000
		}
		if mode&os.ModeSetgid != 0 {
			entry.Mode |= 02000
		}
		switch {
		case mode&os.ModeDir != 0 || strings.HasSuffix(file.Name, "/"):
			entry.Kind = kindDir
		case mode&os.ModeSymlink != 0:
			entry.Kind = kindSymlink
			body, err := file.Open()
			if err != nil {
				return err
			}
			target, err := io.ReadAll(io.LimitReader(body, 64*1024))
			_ = body.Close()
			if err != nil {
				return err
			}
			entry.SymlinkTarget = string(target)
			if err := fn(entry, bytesNewReader(nil)); err != nil {
				return err
			}
			continue
		case mode&os.ModeDevice != 0 && mode&os.ModeCharDevice != 0:
			entry.Kind = kindChar
		case mode&os.ModeDevice != 0:
			entry.Kind = kindBlock
		case mode&os.ModeNamedPipe != 0:
			entry.Kind = kindFIFO
		case mode&os.ModeSocket != 0:
			entry.Kind = kindSocket
		default:
			entry.Kind = kindFile
		}
		body, err := file.Open()
		if err != nil {
			return err
		}
		err = fn(entry, body)
		_ = body.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

func isSpecialKind(kind archiveKind) bool {
	return kind == kindChar || kind == kindBlock || kind == kindFIFO || kind == kindSocket
}

func entryTypeName(kind archiveKind) string {
	switch kind {
	case kindFile:
		return "file"
	case kindDir:
		return "directory"
	case kindSymlink:
		return "symlink"
	case kindChar:
		return "character device"
	case kindBlock:
		return "block device"
	case kindFIFO:
		return "fifo"
	case kindSocket:
		return "socket"
	default:
		return "other"
	}
}

func archiveTypeName(kind archiveKind) string {
	switch kind {
	case kindDir:
		return "directory"
	case kindSymlink:
		return "symlink"
	default:
		return "file"
	}
}
