package inspect

import (
	"bufio"
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"fmt"
	"io"
	"strings"

	"github.com/klauspost/compress/zstd"
	"github.com/pierrec/lz4/v4"
	"github.com/ulikunitz/xz"
	"github.com/ulikunitz/xz/lzma"
)

type readCloser struct {
	io.Reader
	close func() error
}

func (r readCloser) Close() error {
	if r.close != nil {
		return r.close()
	}
	return nil
}

func decompressReader(r io.Reader, hint string) (io.ReadCloser, error) {
	br := bufio.NewReader(r)
	magic, _ := br.Peek(6)
	hint = strings.ToLower(hint)

	switch {
	case bytes.HasPrefix(magic, []byte{0x1f, 0x8b}) || strings.Contains(hint, ".gz") || strings.HasSuffix(hint, ".tgz"):
		gz, err := gzip.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: gz, close: gz.Close}, nil
	case bytes.HasPrefix(magic, []byte{0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00}) || strings.Contains(hint, ".xz"):
		xr, err := xz.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: xr}, nil
	case bytes.HasPrefix(magic, []byte{0x28, 0xb5, 0x2f, 0xfd}) || strings.Contains(hint, ".zst"):
		zr, err := zstd.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: zr, close: func() error { zr.Close(); return nil }}, nil
	case bytes.HasPrefix(magic, []byte{'B', 'Z', 'h'}) || strings.Contains(hint, ".bz2") || strings.HasSuffix(hint, ".tbz2"):
		return readCloser{Reader: bzip2.NewReader(br)}, nil
	case bytes.HasPrefix(magic, []byte{0x04, 0x22, 0x4d, 0x18}) || strings.Contains(hint, ".lz4"):
		return readCloser{Reader: lz4.NewReader(br)}, nil
	case bytes.HasPrefix(magic, []byte{0x5d, 0x00}) || strings.Contains(hint, ".lzma"):
		lr, err := lzma.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: lr}, nil
	default:
		return readCloser{Reader: br}, nil
	}
}

func compressionFromMagic(magic []byte) string {
	switch {
	case bytes.HasPrefix(magic, []byte{0x1f, 0x8b}):
		return "gzip"
	case bytes.HasPrefix(magic, []byte{0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00}):
		return "xz"
	case bytes.HasPrefix(magic, []byte{0x28, 0xb5, 0x2f, 0xfd}):
		return "zstd"
	case bytes.HasPrefix(magic, []byte{'B', 'Z', 'h'}):
		return "bzip2"
	case bytes.HasPrefix(magic, []byte{0x04, 0x22, 0x4d, 0x18}):
		return "lz4"
	case bytes.HasPrefix(magic, []byte{0x5d, 0x00}):
		return "lzma"
	default:
		return ""
	}
}

func openDecompressed(path, hint string) (io.ReadCloser, error) {
	file, err := openRegular(path)
	if err != nil {
		return nil, err
	}
	decoded, err := decompressReader(file, hint)
	if err != nil {
		_ = file.Close()
		return nil, err
	}
	return readCloser{Reader: decoded, close: func() error {
		cerr := decoded.Close()
		ferr := file.Close()
		if cerr != nil {
			return cerr
		}
		return ferr
	}}, nil
}

func decodePayload(r io.Reader, compressor string) (io.ReadCloser, error) {
	br := bufio.NewReader(r)
	magic, _ := br.Peek(6)
	kind := compressor
	if kind == "" || kind == "none" {
		kind = compressionFromMagic(magic)
	}
	switch kind {
	case "", "cpio":
		return readCloser{Reader: br}, nil
	case "gzip", "gz":
		gz, err := gzip.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: gz, close: gz.Close}, nil
	case "xz":
		xr, err := xz.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: xr}, nil
	case "zstd":
		zr, err := zstd.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: zr, close: func() error { zr.Close(); return nil }}, nil
	case "bzip2", "bz2":
		return readCloser{Reader: bzip2.NewReader(br)}, nil
	case "lz4":
		return readCloser{Reader: lz4.NewReader(br)}, nil
	case "lzma":
		lr, err := lzma.NewReader(br)
		if err != nil {
			return nil, err
		}
		return readCloser{Reader: lr}, nil
	default:
		return nil, fmt.Errorf("unsupported RPM payload compressor %q", compressor)
	}
}
