package secret

import (
	"context"
	"os"
	"strings"
)

const envPrefix = "PACSMITH_SECRET_"

type EnvStore struct{}

func NewEnvStore() *EnvStore {
	return &EnvStore{}
}

func (s *EnvStore) Get(ctx context.Context, name string) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	value, ok := os.LookupEnv(envVar(name))
	if !ok {
		return nil, ErrNotFound
	}
	return []byte(value), nil
}

func (s *EnvStore) Set(ctx context.Context, name string, _ []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := ValidateName(name); err != nil {
		return err
	}
	return ErrReadOnly
}

func (s *EnvStore) Delete(ctx context.Context, name string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := ValidateName(name); err != nil {
		return err
	}
	return ErrReadOnly
}

func (s *EnvStore) Exists(ctx context.Context, name string) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	if err := ValidateName(name); err != nil {
		return false, err
	}
	_, ok := os.LookupEnv(envVar(name))
	return ok, nil
}

func envVar(name string) string {
	return envPrefix + strings.ToUpper(strings.ReplaceAll(name, ".", "_"))
}
