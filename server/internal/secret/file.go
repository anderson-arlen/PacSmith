package secret

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

type FileStore struct {
	dir string
}

func NewFileStore(dir string) (*FileStore, error) {
	if dir == "" {
		return nil, fmt.Errorf("empty secret directory")
	}
	dir = filepath.Clean(dir)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("create secret directory: %w", err)
	}
	if err := os.Chmod(dir, 0o700); err != nil {
		return nil, fmt.Errorf("chmod secret directory: %w", err)
	}
	return &FileStore{dir: dir}, nil
}

func (s *FileStore) Get(ctx context.Context, name string) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	path, err := s.path(name)
	if err != nil {
		return nil, err
	}
	info, err := os.Lstat(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("stat secret %q: %w", name, err)
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("secret %q is not a regular file", name)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read secret %q: %w", name, err)
	}
	return data, nil
}

func (s *FileStore) Set(ctx context.Context, name string, value []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	path, err := s.path(name)
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(s.dir, ".pacsmith-secret-*")
	if err != nil {
		return fmt.Errorf("create secret temp: %w", err)
	}
	tmpName := tmp.Name()
	defer func() {
		_ = tmp.Close()
		_ = os.Remove(tmpName)
	}()
	if err := os.Chmod(tmpName, 0o600); err != nil {
		return err
	}
	if _, err := tmp.Write(value); err != nil {
		return fmt.Errorf("write secret %q: %w", name, err)
	}
	if err := tmp.Sync(); err != nil {
		return fmt.Errorf("fsync secret %q: %w", name, err)
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fmt.Errorf("install secret %q: %w", name, err)
	}
	return os.Chmod(path, 0o600)
}

func (s *FileStore) Delete(ctx context.Context, name string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	path, err := s.path(name)
	if err != nil {
		return err
	}
	if err := os.Remove(path); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return ErrNotFound
		}
		return fmt.Errorf("delete secret %q: %w", name, err)
	}
	return nil
}

func (s *FileStore) Exists(ctx context.Context, name string) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	path, err := s.path(name)
	if err != nil {
		return false, err
	}
	info, err := os.Lstat(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return false, nil
		}
		return false, fmt.Errorf("stat secret %q: %w", name, err)
	}
	return info.Mode().IsRegular(), nil
}

func (s *FileStore) path(name string) (string, error) {
	if err := ValidateName(name); err != nil {
		return "", err
	}
	full := filepath.Join(s.dir, name)
	if filepath.Dir(full) != s.dir {
		return "", fmt.Errorf("%w: %q", ErrInvalidName, name)
	}
	return full, nil
}
