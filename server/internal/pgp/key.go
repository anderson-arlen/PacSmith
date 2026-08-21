package pgp

import (
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
)

const maxKeyBytes = 4 * 1024 * 1024

// Normalize returns binary OpenPGP keyring bytes. Armored input is decoded;
// binary packets are returned unchanged.
func Normalize(contents []byte) ([]byte, error) {
	if len(contents) == 0 {
		return nil, fmt.Errorf("signing key is empty")
	}
	if len(contents) > maxKeyBytes {
		return nil, fmt.Errorf("signing key exceeds the 4 MiB limit")
	}
	trimmed := bytes.TrimSpace(contents)
	if bytes.Contains(trimmed, []byte("-----BEGIN PGP PUBLIC KEY BLOCK-----")) ||
		bytes.Contains(trimmed, []byte("-----BEGIN PGP PUBLIC KEY-----")) {
		decoded, err := decodeArmor(trimmed)
		if err != nil {
			return nil, err
		}
		if len(decoded) == 0 {
			return nil, fmt.Errorf("armored signing key decoded to empty data")
		}
		return decoded, nil
	}
	return append([]byte(nil), contents...), nil
}

// Fingerprints returns uppercase OpenPGP fingerprints for primary keys and
// subkeys in a binary or armored public keyring.
func Fingerprints(contents []byte) ([]string, error) {
	binaryKey, err := Normalize(contents)
	if err != nil {
		return nil, err
	}
	var fingerprints []string
	seen := map[string]struct{}{}
	rest := binaryKey
	for len(rest) > 0 {
		tag, body, next, err := readPacket(rest)
		if err != nil {
			return nil, err
		}
		rest = next
		if tag != 6 && tag != 14 {
			continue
		}
		fingerprint, ok := packetFingerprint(body)
		if !ok {
			continue
		}
		if _, exists := seen[fingerprint]; exists {
			continue
		}
		seen[fingerprint] = struct{}{}
		fingerprints = append(fingerprints, fingerprint)
	}
	if len(fingerprints) == 0 {
		return nil, fmt.Errorf("no OpenPGP public key was found")
	}
	return fingerprints, nil
}

func decodeArmor(data []byte) ([]byte, error) {
	text := string(data)
	begin := strings.Index(text, "-----BEGIN PGP")
	if begin < 0 {
		return nil, fmt.Errorf("missing OpenPGP armor header")
	}
	headerEnd := strings.Index(text[begin:], "\n")
	if headerEnd < 0 {
		return nil, fmt.Errorf("truncated OpenPGP armor header")
	}
	body := text[begin+headerEnd+1:]
	if end := strings.Index(body, "-----END PGP"); end >= 0 {
		body = body[:end]
	}
	if blank := strings.Index(body, "\n\n"); blank >= 0 {
		body = body[blank+2:]
	} else if blank := strings.Index(body, "\r\n\r\n"); blank >= 0 {
		body = body[blank+4:]
	}
	var encoded strings.Builder
	for _, line := range strings.Split(body, "\n") {
		line = strings.TrimSpace(strings.TrimSuffix(line, "\r"))
		if line == "" || strings.HasPrefix(line, "=") || strings.HasPrefix(line, "-") {
			continue
		}
		encoded.WriteString(line)
	}
	decoded, err := base64.StdEncoding.DecodeString(encoded.String())
	if err != nil {
		return nil, fmt.Errorf("decode OpenPGP armor: %w", err)
	}
	return decoded, nil
}

func readPacket(data []byte) (tag int, body, rest []byte, err error) {
	if len(data) < 2 {
		return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet")
	}
	header := data[0]
	if header&0x80 == 0 {
		return 0, nil, nil, fmt.Errorf("invalid OpenPGP packet header")
	}
	if header&0x40 != 0 {
		tag = int(header & 0x3f)
		length, headerSize, err := newFormatLength(data[1:])
		if err != nil {
			return 0, nil, nil, err
		}
		start := 1 + headerSize
		end := start + length
		if end > len(data) {
			return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet body")
		}
		return tag, data[start:end], data[end:], nil
	}
	tag = int((header >> 2) & 0x0f)
	lengthType := header & 0x03
	var length int
	headerSize := 1
	switch lengthType {
	case 0:
		if len(data) < 2 {
			return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet length")
		}
		length = int(data[1])
		headerSize = 2
	case 1:
		if len(data) < 3 {
			return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet length")
		}
		length = int(binary.BigEndian.Uint16(data[1:3]))
		headerSize = 3
	case 2:
		if len(data) < 5 {
			return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet length")
		}
		length = int(binary.BigEndian.Uint32(data[1:5]))
		headerSize = 5
	default:
		return tag, data[1:], nil, nil
	}
	end := headerSize + length
	if end > len(data) {
		return 0, nil, nil, fmt.Errorf("truncated OpenPGP packet body")
	}
	return tag, data[headerSize:end], data[end:], nil
}

func newFormatLength(data []byte) (length, headerSize int, err error) {
	if len(data) == 0 {
		return 0, 0, fmt.Errorf("truncated OpenPGP packet length")
	}
	first := data[0]
	switch {
	case first < 192:
		return int(first), 1, nil
	case first < 224:
		if len(data) < 2 {
			return 0, 0, fmt.Errorf("truncated OpenPGP packet length")
		}
		return int(first-192)*256 + int(data[1]) + 192, 2, nil
	case first == 255:
		if len(data) < 5 {
			return 0, 0, fmt.Errorf("truncated OpenPGP packet length")
		}
		return int(binary.BigEndian.Uint32(data[1:5])), 5, nil
	default:
		return 0, 0, fmt.Errorf("partial OpenPGP packet lengths are not supported")
	}
}

func packetFingerprint(body []byte) (string, bool) {
	if len(body) == 0 {
		return "", false
	}
	switch body[0] {
	case 4:
		if len(body) > 0xffff {
			return "", false
		}
		var prefix [3]byte
		prefix[0] = 0x99
		binary.BigEndian.PutUint16(prefix[1:], uint16(len(body)))
		sum := sha1.Sum(append(prefix[:], body...))
		return strings.ToUpper(hex.EncodeToString(sum[:])), true
	case 5:
		var prefix [5]byte
		prefix[0] = 0x9a
		binary.BigEndian.PutUint32(prefix[1:], uint32(len(body)))
		sum := sha256.Sum256(append(prefix[:], body...))
		return strings.ToUpper(hex.EncodeToString(sum[:])), true
	default:
		return "", false
	}
}
