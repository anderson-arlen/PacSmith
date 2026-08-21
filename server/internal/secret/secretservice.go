package secret

import (
	"context"
	"fmt"

	"github.com/godbus/dbus/v5"
)

const (
	ssName       = "org.freedesktop.secrets"
	ssPath       = "/org/freedesktop/secrets"
	ssService    = "org.freedesktop.Secret.Service"
	ssCollection = "org.freedesktop.Secret.Collection"
	ssItem       = "org.freedesktop.Secret.Item"
	ssSession    = "org.freedesktop.Secret.Session"
	ssSchema     = "org.pacsmith.DaemonSecrets"
	ssDefault    = "/org/freedesktop/secrets/aliases/default"
)

type ssSecret struct {
	Session     dbus.ObjectPath
	Parameters  []byte
	Value       []byte
	ContentType string
}

type SecretServiceStore struct {
	connect func() (*dbus.Conn, error)
}

func NewSecretServiceStore() *SecretServiceStore {
	return &SecretServiceStore{}
}

func (s *SecretServiceStore) Get(ctx context.Context, name string) ([]byte, error) {
	if err := ValidateName(name); err != nil {
		return nil, err
	}
	var value []byte
	err := s.withSession(ctx, func(conn *dbus.Conn, session dbus.ObjectPath) error {
		item, err := findItem(ctx, conn, name)
		if err != nil {
			return err
		}
		if err := unlockPaths(ctx, conn, []dbus.ObjectPath{item}); err != nil {
			return err
		}
		var secret ssSecret
		if err := conn.Object(ssName, item).CallWithContext(ctx, ssItem+".GetSecret", 0, session).Store(&secret); err != nil {
			return fmt.Errorf("%w: get secret: %s", ErrUnavailable, err.Error())
		}
		value = append([]byte(nil), secret.Value...)
		return nil
	})
	return value, err
}

func (s *SecretServiceStore) Set(ctx context.Context, name string, value []byte) error {
	if err := ValidateName(name); err != nil {
		return err
	}
	return s.withSession(ctx, func(conn *dbus.Conn, session dbus.ObjectPath) error {
		collPath := dbus.ObjectPath(ssDefault)
		if err := unlockPaths(ctx, conn, []dbus.ObjectPath{collPath}); err != nil {
			return err
		}
		secret := ssSecret{
			Session:     session,
			Parameters:  []byte{},
			Value:       value,
			ContentType: "application/octet-stream",
		}
		props := map[string]dbus.Variant{
			ssItem + ".Label":      dbus.MakeVariant("pacsmith:" + name),
			ssItem + ".Attributes": dbus.MakeVariant(ssAttrs(name)),
		}
		var item, prompt dbus.ObjectPath
		coll := conn.Object(ssName, collPath)
		if err := coll.CallWithContext(ctx, ssCollection+".CreateItem", 0, props, secret, true).Store(&item, &prompt); err != nil {
			return fmt.Errorf("%w: create item: %s", ErrUnavailable, err.Error())
		}
		return rejectPrompt(prompt)
	})
}

func (s *SecretServiceStore) Delete(ctx context.Context, name string) error {
	if err := ValidateName(name); err != nil {
		return err
	}
	return s.withSession(ctx, func(conn *dbus.Conn, _ dbus.ObjectPath) error {
		item, err := findItem(ctx, conn, name)
		if err != nil {
			return err
		}
		var prompt dbus.ObjectPath
		if err := conn.Object(ssName, item).CallWithContext(ctx, ssItem+".Delete", 0).Store(&prompt); err != nil {
			return fmt.Errorf("%w: delete: %s", ErrUnavailable, err.Error())
		}
		return rejectPrompt(prompt)
	})
}

func (s *SecretServiceStore) Exists(ctx context.Context, name string) (bool, error) {
	if err := ValidateName(name); err != nil {
		return false, err
	}
	var found bool
	err := s.withConn(ctx, func(conn *dbus.Conn) error {
		unlocked, locked, err := searchItems(ctx, conn, name)
		if err != nil {
			return err
		}
		found = len(unlocked)+len(locked) > 0
		return nil
	})
	return found, err
}

func (s *SecretServiceStore) withConn(ctx context.Context, fn func(*dbus.Conn) error) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	conn, err := s.openBus()
	if err != nil {
		return err
	}
	defer conn.Close()
	return fn(conn)
}

func (s *SecretServiceStore) withSession(ctx context.Context, fn func(*dbus.Conn, dbus.ObjectPath) error) error {
	return s.withConn(ctx, func(conn *dbus.Conn) error {
		session, err := openPlainSession(ctx, conn)
		if err != nil {
			return err
		}
		defer closeSession(ctx, conn, session)
		return fn(conn, session)
	})
}

func (s *SecretServiceStore) openBus() (*dbus.Conn, error) {
	connect := s.connect
	if connect == nil {
		connect = func() (*dbus.Conn, error) {
			return dbus.ConnectSessionBus()
		}
	}
	conn, err := connect()
	if err != nil {
		return nil, fmt.Errorf("%w: session bus: %s", ErrUnavailable, err.Error())
	}
	return conn, nil
}

func openPlainSession(ctx context.Context, conn *dbus.Conn) (dbus.ObjectPath, error) {
	svc := conn.Object(ssName, dbus.ObjectPath(ssPath))
	var output dbus.Variant
	var session dbus.ObjectPath
	if err := svc.CallWithContext(ctx, ssService+".OpenSession", 0, "plain", dbus.MakeVariant("")).Store(&output, &session); err != nil {
		return "", fmt.Errorf("%w: open session: %s", ErrUnavailable, err.Error())
	}
	return session, nil
}

func closeSession(ctx context.Context, conn *dbus.Conn, session dbus.ObjectPath) {
	_ = conn.Object(ssName, session).CallWithContext(ctx, ssSession+".Close", 0).Err
}

func searchItems(ctx context.Context, conn *dbus.Conn, name string) (unlocked, locked []dbus.ObjectPath, err error) {
	svc := conn.Object(ssName, dbus.ObjectPath(ssPath))
	if err := svc.CallWithContext(ctx, ssService+".SearchItems", 0, ssAttrs(name)).Store(&unlocked, &locked); err != nil {
		return nil, nil, fmt.Errorf("%w: search: %s", ErrUnavailable, err.Error())
	}
	return unlocked, locked, nil
}

func findItem(ctx context.Context, conn *dbus.Conn, name string) (dbus.ObjectPath, error) {
	unlocked, locked, err := searchItems(ctx, conn, name)
	if err != nil {
		return "", err
	}
	if len(unlocked) > 0 {
		return unlocked[0], nil
	}
	if len(locked) > 0 {
		return locked[0], nil
	}
	return "", ErrNotFound
}

func unlockPaths(ctx context.Context, conn *dbus.Conn, paths []dbus.ObjectPath) error {
	if len(paths) == 0 {
		return nil
	}
	svc := conn.Object(ssName, dbus.ObjectPath(ssPath))
	var unlocked []dbus.ObjectPath
	var prompt dbus.ObjectPath
	if err := svc.CallWithContext(ctx, ssService+".Unlock", 0, paths).Store(&unlocked, &prompt); err != nil {
		return fmt.Errorf("%w: unlock: %s", ErrUnavailable, err.Error())
	}
	return rejectPrompt(prompt)
}

func rejectPrompt(prompt dbus.ObjectPath) error {
	if prompt != "" && prompt != "/" {
		return fmt.Errorf("%w: secret service requires a prompt", ErrUnavailable)
	}
	return nil
}

func ssAttrs(name string) map[string]string {
	return map[string]string{
		"name":       name,
		"xdg:schema": ssSchema,
	}
}
