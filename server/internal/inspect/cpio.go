package inspect

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"strings"
)

func bytesNewReader(b []byte) *bytes.Reader {
	return bytes.NewReader(b)
}

func walkCPIO(r io.Reader, fn func(walkedEntry, io.Reader) error) error {
	for {
		hdr := make([]byte, 110)
		if _, err := io.ReadFull(r, hdr); err != nil {
			if err == io.EOF {
				return nil
			}
			return fmt.Errorf("truncated RPM cpio header")
		}
		magic := string(hdr[0:6])
		if magic != "070701" && magic != "070702" {
			return fmt.Errorf("unsupported RPM cpio magic %q", magic)
		}
		namesize, err := parseHex(hdr[94:102])
		if err != nil || namesize == 0 || namesize > 4096 {
			return fmt.Errorf("invalid RPM cpio name size")
		}
		filesize, err := parseHex(hdr[54:62])
		if err != nil {
			return fmt.Errorf("invalid RPM cpio file size")
		}
		mode, err := parseHex(hdr[14:22])
		if err != nil {
			return fmt.Errorf("invalid RPM cpio mode")
		}
		nameBytes := make([]byte, namesize)
		if _, err := io.ReadFull(r, nameBytes); err != nil {
			return fmt.Errorf("truncated RPM cpio name")
		}
		name := strings.TrimRight(string(nameBytes), "\x00")
		if err := skipPad(r, 110+int(namesize), 4); err != nil {
			return err
		}
		if name == "TRAILER!!!" {
			_, _ = io.CopyN(io.Discard, r, int64(filesize))
			return nil
		}
		entry := walkedEntry{
			RawName: strings.TrimPrefix(name, "./"),
			Size:    int64(filesize),
			Mode:    int64(mode) & 07777,
		}
		fileType := mode & 0170000
		switch fileType {
		case 0040000:
			entry.Kind = kindDir
		case 0120000:
			entry.Kind = kindSymlink
		case 0020000:
			entry.Kind = kindChar
		case 0060000:
			entry.Kind = kindBlock
		case 0010000:
			entry.Kind = kindFIFO
		case 0140000:
			entry.Kind = kindSocket
		default:
			entry.Kind = kindFile
		}
		body := io.LimitReader(r, int64(filesize))
		if entry.Kind == kindSymlink {
			target, err := io.ReadAll(body)
			if err != nil {
				return err
			}
			entry.SymlinkTarget = string(target)
			body = bytes.NewReader(nil)
		}
		if err := fn(entry, body); err != nil {
			return err
		}
		if _, err := io.Copy(io.Discard, body); err != nil {
			return err
		}
		if err := skipPad(r, int(filesize), 4); err != nil {
			return err
		}
	}
}

func parseHex(field []byte) (uint64, error) {
	var value uint64
	for _, b := range bytes.TrimSpace(field) {
		switch {
		case b >= '0' && b <= '9':
			value = value<<4 | uint64(b-'0')
		case b >= 'a' && b <= 'f':
			value = value<<4 | uint64(b-'a'+10)
		case b >= 'A' && b <= 'F':
			value = value<<4 | uint64(b-'A'+10)
		default:
			return 0, fmt.Errorf("invalid hex")
		}
	}
	return value, nil
}

func skipPad(r io.Reader, used, align int) error {
	if align <= 0 {
		return nil
	}
	pad := (align - (used % align)) % align
	if pad == 0 {
		return nil
	}
	_, err := io.CopyN(io.Discard, r, int64(pad))
	if err == io.EOF {
		return nil
	}
	return err
}

func readBE32(data []byte, offset int) (uint32, bool) {
	if offset < 0 || offset+4 > len(data) {
		return 0, false
	}
	return binary.BigEndian.Uint32(data[offset : offset+4]), true
}
