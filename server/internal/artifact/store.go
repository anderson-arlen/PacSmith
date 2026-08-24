package artifact

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
)

const hexNameLength = 64

var sha256Hex = regexp.MustCompile(`^[0-9a-f]{64}$`)

type Store struct {
	objects string
	tmp     string
}

type Object struct {
	SHA256 string
	Size   int64
}

func New(objectsDir, tmpDir string) (*Store, error) {
	if objectsDir == "" || tmpDir == "" {
		return nil, fmt.Errorf("artifact store directories must be set")
	}
	if err := os.MkdirAll(objectsDir, 0o700); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(tmpDir, 0o700); err != nil {
		return nil, err
	}
	return &Store{objects: objectsDir, tmp: tmpDir}, nil
}

func (s *Store) Path(sha256HexDigest string) (string, error) {
	digest, err := normalizeSHA256(sha256HexDigest)
	if err != nil {
		return "", err
	}
	return filepath.Join(s.objects, digest[:2], digest), nil
}

func (s *Store) Exists(sha256HexDigest string) (bool, error) {
	path, err := s.Path(sha256HexDigest)
	if err != nil {
		return false, err
	}
	info, err := os.Stat(path)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	return info.Mode().IsRegular(), nil
}

func (s *Store) Remove(sha256HexDigest string) error {
	path, err := s.Path(sha256HexDigest)
	if err != nil {
		return err
	}
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func (s *Store) Open(sha256HexDigest string) (*os.File, int64, error) {
	path, err := s.Path(sha256HexDigest)
	if err != nil {
		return nil, 0, err
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, 0, err
	}
	info, err := file.Stat()
	if err != nil {
		_ = file.Close()
		return nil, 0, err
	}
	return file, info.Size(), nil
}

// Ingest streams r to a temporary file, hashes it, fsyncs, and atomically
// installs it into the object store. A crash during ingest may leave a tmp
// file; that is orphan data and is not referenced from SQLite.
func (s *Store) Ingest(r io.Reader) (Object, error) {
	if r == nil {
		return Object{}, fmt.Errorf("missing artifact body")
	}
	tmp, err := os.CreateTemp(s.tmp, "ingest-*.part")
	if err != nil {
		return Object{}, fmt.Errorf("create ingest temp: %w", err)
	}
	tmpName := tmp.Name()
	defer func() {
		_ = tmp.Close()
		_ = os.Remove(tmpName)
	}()
	if err := os.Chmod(tmpName, 0o600); err != nil {
		return Object{}, err
	}

	hash := sha256.New()
	size, err := io.Copy(io.MultiWriter(tmp, hash), r)
	if err != nil {
		return Object{}, fmt.Errorf("write ingest temp: %w", err)
	}
	if err := tmp.Sync(); err != nil {
		return Object{}, fmt.Errorf("fsync ingest temp: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return Object{}, err
	}

	digest := hex.EncodeToString(hash.Sum(nil))
	finalPath, err := s.Path(digest)
	if err != nil {
		return Object{}, err
	}
	if err := os.MkdirAll(filepath.Dir(finalPath), 0o700); err != nil {
		return Object{}, err
	}
	if existing, err := os.Stat(finalPath); err == nil && existing.Mode().IsRegular() {
		return Object{SHA256: digest, Size: existing.Size()}, nil
	} else if err != nil && !os.IsNotExist(err) {
		return Object{}, err
	}
	if err := os.Rename(tmpName, finalPath); err != nil {
		return Object{}, fmt.Errorf("install object: %w", err)
	}
	if err := fsyncDir(filepath.Dir(finalPath)); err != nil {
		return Object{}, err
	}
	if err := os.Chmod(finalPath, 0o600); err != nil {
		return Object{}, err
	}
	return Object{SHA256: digest, Size: size}, nil
}

func fsyncDir(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return err
	}
	defer dir.Close()
	return dir.Sync()
}

func normalizeSHA256(value string) (string, error) {
	if !sha256Hex.MatchString(value) {
		return "", fmt.Errorf("invalid artifact sha256")
	}
	return value, nil
}

func ValidSHA256(value string) bool {
	return sha256Hex.MatchString(value)
}

func HexNameLength() int {
	return hexNameLength
}
