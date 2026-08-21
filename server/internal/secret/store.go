package secret

import (
	"context"
	"errors"
	"fmt"
	"regexp"
)

const (
	BackendFile          = "file"
	BackendEnv           = "env"
	BackendSecretService = "secret-service"
)

var (
	ErrUnavailable = errors.New("secret backend unavailable")
	ErrNotFound    = errors.New("secret not found")
	ErrReadOnly    = errors.New("secret backend is read-only")
	ErrInvalidName = errors.New("invalid secret name")
)

var namePattern = regexp.MustCompile(`^[a-z0-9._-]+$`)

type Store interface {
	Get(ctx context.Context, name string) ([]byte, error)
	Set(ctx context.Context, name string, value []byte) error
	Delete(ctx context.Context, name string) error
	Exists(ctx context.Context, name string) (bool, error)
}

func ValidateName(name string) error {
	if name == "" || name == "." || name == ".." || !namePattern.MatchString(name) {
		return fmt.Errorf("%w: %q", ErrInvalidName, name)
	}
	return nil
}
