package secret

import "context"

// LockedStore pins a single backend at construction. If the inner store fails
// with ErrUnavailable, the error is returned as-is; there is no fallback.
type LockedStore struct {
	backend string
	inner   Store
}

func NewLockedStore(backend string, inner Store) *LockedStore {
	return &LockedStore{backend: backend, inner: inner}
}

func (s *LockedStore) Backend() string {
	return s.backend
}

func (s *LockedStore) Get(ctx context.Context, name string) ([]byte, error) {
	return s.inner.Get(ctx, name)
}

func (s *LockedStore) Set(ctx context.Context, name string, value []byte) error {
	return s.inner.Set(ctx, name, value)
}

func (s *LockedStore) Delete(ctx context.Context, name string) error {
	return s.inner.Delete(ctx, name)
}

func (s *LockedStore) Exists(ctx context.Context, name string) (bool, error) {
	return s.inner.Exists(ctx, name)
}
