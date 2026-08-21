package inspect

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
)

func readLimited(r io.Reader, max int) ([]byte, error) {
	if max < 0 {
		max = 0
	}
	var result []byte
	buf := make([]byte, 16*1024)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			if max-n < len(result) {
				return nil, fmt.Errorf("Archive metadata member exceeds the safety limit")
			}
			result = append(result, buf[:n]...)
		}
		if err == io.EOF {
			return result, nil
		}
		if err != nil {
			return nil, err
		}
	}
}

func inspectBody(r io.Reader, captureLimit int, maxTotal int64) (captured []byte, sum string, truncated bool, err error) {
	h := sha256.New()
	buf := make([]byte, 64*1024)
	var total int64
	for {
		n, readErr := r.Read(buf)
		if n > 0 {
			total += int64(n)
			if maxTotal > 0 && total > maxTotal {
				return nil, "", false, fmt.Errorf("review file exceeds the safety limit")
			}
			h.Write(buf[:n])
			available := captureLimit - len(captured)
			if available > 0 {
				take := n
				if take > available {
					take = available
				}
				captured = append(captured, buf[:take]...)
			}
			if n > available {
				truncated = true
			}
		}
		if readErr == io.EOF {
			return captured, hex.EncodeToString(h.Sum(nil)), truncated, nil
		}
		if readErr != nil {
			return nil, "", false, readErr
		}
	}
}

func peekPrefix(r io.Reader, n int) ([]byte, error) {
	if n <= 0 {
		_, _ = io.Copy(io.Discard, r)
		return nil, nil
	}
	buf := make([]byte, n)
	got, err := io.ReadFull(r, buf)
	if err == io.EOF || err == io.ErrUnexpectedEOF {
		buf = buf[:got]
		_, _ = io.Copy(io.Discard, r)
		return buf, nil
	}
	if err != nil {
		return nil, err
	}
	_, _ = io.Copy(io.Discard, r)
	return buf, nil
}

func drainReader(r io.Reader) error {
	_, err := io.Copy(io.Discard, r)
	return err
}

func copyLimited(dst io.Writer, src io.Reader) error {
	_, err := io.Copy(dst, src)
	return err
}
