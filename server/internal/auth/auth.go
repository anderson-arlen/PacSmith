package auth

import "context"

type Kind int

const (
	KindLocalUnix Kind = iota
	KindRemoteClient
	KindEnrollment
)

// Principal is derived from how the connection was authenticated, not from
// JSON in the request body.
type Principal struct {
	Kind     Kind
	ClientID string
}

type contextKey struct{}

func WithPrincipal(ctx context.Context, principal Principal) context.Context {
	return context.WithValue(ctx, contextKey{}, principal)
}

func PrincipalFrom(ctx context.Context) Principal {
	principal, _ := ctx.Value(contextKey{}).(Principal)
	return principal
}

func (p Principal) IsLocalAdmin() bool {
	return p.Kind == KindLocalUnix
}

func LocalUnix() Principal {
	return Principal{Kind: KindLocalUnix}
}
