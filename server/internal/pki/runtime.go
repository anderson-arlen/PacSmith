package pki

import (
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"net"
	"os"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const (
	SecretServerCACert = "pki.server-ca.cert"
	SecretServerCAKey  = "pki.server-ca.key"
	SecretClientCACert = "pki.client-ca.cert"
	SecretClientCAKey  = "pki.client-ca.key"
)

type Runtime struct {
	Material  Material
	Leaves    *LeafCache
	Abbrev    string
	Full      string
	Recovered bool
}

func LoadOrGenerate(ctx context.Context, db *sqlite.DB, secrets secret.Store) (*Runtime, error) {
	state, err := db.Queries.GetServerState(ctx)
	if err != nil {
		return nil, err
	}
	if state.PkiReady != 0 {
		material, err := loadMaterial(ctx, secrets)
		if err != nil {
			if !errors.Is(err, secret.ErrNotFound) {
				return nil, fmt.Errorf("load initialized CA material: %w", err)
			}
			return recoverMaterial(ctx, db, secrets, state.SecretBackend)
		}
		runtime, err := newRuntime(material)
		if err == nil {
			return runtime, nil
		}
		return recoverMaterial(ctx, db, secrets, state.SecretBackend)
	}
	material, err := GenerateCAs()
	if err != nil {
		return nil, err
	}
	if err := storeMaterial(ctx, secrets, material); err != nil {
		return nil, err
	}
	if err := db.Queries.UpdateServerBackend(ctx, sqlcdb.UpdateServerBackendParams{
		SecretBackend: state.SecretBackend,
		PkiReady:      1,
	}); err != nil {
		return nil, err
	}
	return newRuntime(material)
}

func recoverMaterial(ctx context.Context, db *sqlite.DB, secrets secret.Store, backend string) (*Runtime, error) {
	tx, err := db.SQL.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	queries := db.Queries.WithTx(tx)
	if err := queries.DeleteAllRegistrations(ctx); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if err := queries.DeleteAllClients(ctx); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if err := queries.UpdateServerBackend(ctx, sqlcdb.UpdateServerBackendParams{
		SecretBackend: backend,
		PkiReady:      0,
	}); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}

	material, err := GenerateCAs()
	if err != nil {
		return nil, err
	}
	if err := storeMaterial(ctx, secrets, material); err != nil {
		return nil, err
	}
	if err := db.Queries.UpdateServerBackend(ctx, sqlcdb.UpdateServerBackendParams{
		SecretBackend: backend,
		PkiReady:      1,
	}); err != nil {
		return nil, err
	}
	runtime, err := newRuntime(material)
	if err != nil {
		return nil, err
	}
	runtime.Recovered = true
	return runtime, nil
}

func newRuntime(material Material) (*Runtime, error) {
	abbrev, full, err := ServerFingerprint(material.ServerCACert)
	if err != nil {
		return nil, err
	}
	return &Runtime{Material: material, Leaves: NewLeafCache(), Abbrev: abbrev, Full: full}, nil
}

func loadMaterial(ctx context.Context, secrets secret.Store) (Material, error) {
	var material Material
	var err error
	if material.ServerCACert, err = secrets.Get(ctx, SecretServerCACert); err != nil {
		return Material{}, err
	}
	if material.ServerCAKey, err = secrets.Get(ctx, SecretServerCAKey); err != nil {
		return Material{}, err
	}
	if material.ClientCACert, err = secrets.Get(ctx, SecretClientCACert); err != nil {
		return Material{}, err
	}
	if material.ClientCAKey, err = secrets.Get(ctx, SecretClientCAKey); err != nil {
		return Material{}, err
	}
	return material, nil
}

func storeMaterial(ctx context.Context, secrets secret.Store, material Material) error {
	pairs := []struct {
		name  string
		value []byte
	}{
		{SecretServerCACert, material.ServerCACert},
		{SecretServerCAKey, material.ServerCAKey},
		{SecretClientCACert, material.ClientCACert},
		{SecretClientCAKey, material.ClientCAKey},
	}
	for _, pair := range pairs {
		if err := secrets.Set(ctx, pair.name, pair.value); err != nil {
			return err
		}
	}
	return nil
}

func (rt *Runtime) TLSConfig() (*tls.Config, error) {
	if rt == nil {
		return nil, fmt.Errorf("pki runtime is not ready")
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(rt.Material.ClientCACert) {
		return nil, fmt.Errorf("client CA is not a valid certificate")
	}
	caBlock, _ := pem.Decode(rt.Material.ServerCACert)
	if caBlock == nil {
		return nil, fmt.Errorf("server CA is not a valid certificate")
	}
	caDER := append([]byte(nil), caBlock.Bytes...)
	return &tls.Config{
		MinVersion: tls.VersionTLS13,
		ClientCAs:  pool,
		ClientAuth: tls.VerifyClientCertIfGiven,
		GetCertificate: func(hello *tls.ClientHelloInfo) (*tls.Certificate, error) {
			var ip net.IP
			if hello.Conn != nil {
				if addr, ok := hello.Conn.LocalAddr().(*net.TCPAddr); ok {
					ip = addr.IP
				}
			}
			certPEM, keyPEM, err := LeafForHello(rt.Material.ServerCACert, rt.Material.ServerCAKey, rt.Leaves, hello.ServerName, ip)
			if err != nil {
				return nil, err
			}
			cert, err := tls.X509KeyPair(certPEM, keyPEM)
			if err != nil {
				return nil, err
			}
			cert.Certificate = append(cert.Certificate, caDER)
			return &cert, nil
		},
	}, nil
}

func CertSHA256(cert *x509.Certificate) string {
	if cert == nil {
		return ""
	}
	sum := sha256.Sum256(cert.Raw)
	return hex.EncodeToString(sum[:])
}

func (rt *Runtime) SignCSR(csrPEM []byte, clientID string) ([]byte, error) {
	return SignClientCSR(rt.Material.ClientCACert, rt.Material.ClientCAKey, csrPEM, clientID)
}

func WriteCAFile(path string, pem []byte) error {
	if err := os.WriteFile(path, pem, 0o600); err != nil {
		return err
	}
	return os.Chmod(path, 0o600)
}

func MissingCAError() error {
	return errors.New("missing CA material")
}
