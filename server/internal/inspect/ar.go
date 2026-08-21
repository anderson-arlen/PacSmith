package inspect

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"strconv"
	"strings"
)

const arMagic = "!<arch>\n"

type arHeader struct {
	Name string
	Size int64
}

type arReader struct {
	br          *bufio.Reader
	gnuTable    []byte
	remaining   int64
	pad         bool
	initialized bool
}

func newArReader(r io.Reader) (*arReader, error) {
	br, ok := r.(*bufio.Reader)
	if !ok {
		br = bufio.NewReader(r)
	}
	magic := make([]byte, len(arMagic))
	if _, err := io.ReadFull(br, magic); err != nil {
		return nil, fmt.Errorf("Could not open DEB archive")
	}
	if string(magic) != arMagic {
		return nil, fmt.Errorf("Could not open DEB archive")
	}
	return &arReader{br: br}, nil
}

func (a *arReader) Next() (*arHeader, error) {
	if err := a.discardCurrent(); err != nil {
		return nil, err
	}
	for {
		header := make([]byte, 60)
		_, err := io.ReadFull(a.br, header)
		if err == io.EOF {
			return nil, io.EOF
		}
		if err != nil {
			return nil, fmt.Errorf("Could not finish reading the DEB archive")
		}
		if !bytes.Equal(header[58:60], []byte{'`', '\n'}) {
			return nil, fmt.Errorf("Could not finish reading the DEB archive")
		}
		name := strings.TrimRight(string(header[0:16]), " ")
		sizeField := strings.TrimSpace(string(header[48:58]))
		size, err := strconv.ParseInt(sizeField, 10, 64)
		if err != nil || size < 0 {
			return nil, fmt.Errorf("Could not finish reading the DEB archive")
		}
		dataName := ""
		if strings.HasPrefix(name, "#1/") {
			nameLen, convErr := strconv.Atoi(strings.TrimPrefix(name, "#1/"))
			if convErr != nil || nameLen < 0 || int64(nameLen) > size {
				return nil, fmt.Errorf("Could not finish reading the DEB archive")
			}
			nameBytes := make([]byte, nameLen)
			if _, err := io.ReadFull(a.br, nameBytes); err != nil {
				return nil, fmt.Errorf("Could not finish reading the DEB archive")
			}
			dataName = strings.TrimRight(string(nameBytes), "\x00")
			size -= int64(nameLen)
		} else if name == "//" || name == "/" {
			table := make([]byte, size)
			if _, err := io.ReadFull(a.br, table); err != nil {
				return nil, fmt.Errorf("Could not finish reading the DEB archive")
			}
			if size%2 != 0 {
				if _, err := a.br.Discard(1); err != nil && err != io.EOF {
					return nil, err
				}
			}
			if name == "//" {
				a.gnuTable = table
			}
			continue
		} else if strings.HasPrefix(name, "/") && a.gnuTable != nil {
			offset, convErr := strconv.Atoi(strings.TrimPrefix(strings.TrimRight(name, "/"), "/"))
			if convErr == nil && offset >= 0 && offset < len(a.gnuTable) {
				rest := a.gnuTable[offset:]
				if i := bytes.IndexByte(rest, '\n'); i >= 0 {
					dataName = strings.TrimRight(string(rest[:i]), "/\n")
				} else {
					dataName = strings.TrimRight(string(rest), "/")
				}
			}
		} else {
			dataName = strings.TrimRight(name, "/")
		}
		a.remaining = size
		a.pad = size%2 != 0
		a.initialized = true
		return &arHeader{Name: dataName, Size: size}, nil
	}
}

func (a *arReader) Read(p []byte) (int, error) {
	if a.remaining <= 0 {
		return 0, io.EOF
	}
	if int64(len(p)) > a.remaining {
		p = p[:a.remaining]
	}
	n, err := a.br.Read(p)
	a.remaining -= int64(n)
	if err == io.EOF && a.remaining > 0 {
		return n, fmt.Errorf("Could not read DEB member")
	}
	return n, err
}

func (a *arReader) discardCurrent() error {
	if !a.initialized {
		return nil
	}
	if a.remaining > 0 {
		if _, err := io.CopyN(io.Discard, a.br, a.remaining); err != nil {
			return err
		}
		a.remaining = 0
	}
	if a.pad {
		if _, err := a.br.Discard(1); err != nil && err != io.EOF {
			return err
		}
		a.pad = false
	}
	return nil
}
