package pki

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestLoadOrGenerateRecoversMissingInitializedMaterial(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	store, err := secret.NewFileStore(filepath.Join(root, "secrets"))
	if err != nil {
		t.Fatal(err)
	}

	initial, err := LoadOrGenerate(ctx, db, store)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.InsertClient(ctx, sqlcdb.InsertClientParams{
		ID: "client-1", Name: "old client", CertPem: "old", CertSha256: "old", CreatedAt: "2026-01-01T00:00:00Z",
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.InsertRegistration(ctx, sqlcdb.InsertRegistrationParams{
		ID: "registration-1", Name: "pending", Status: "pending", CsrPem: "old",
		ClientID: sql.NullString{}, CreatedAt: "2026-01-01T00:00:00Z",
		ExpiresAt: "2026-01-02T00:00:00Z", RemoteAddr: "local",
	}); err != nil {
		t.Fatal(err)
	}
	if err := store.Delete(ctx, SecretServerCAKey); err != nil {
		t.Fatal(err)
	}

	recovered, err := LoadOrGenerate(ctx, db, store)
	if err != nil {
		t.Fatal(err)
	}
	if !recovered.Recovered {
		t.Fatal("missing initialized CA material was not reported as recovered")
	}
	if recovered.Full == initial.Full {
		t.Fatal("recovery reused the old server CA")
	}
	clients, err := db.Queries.ListClients(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(clients) != 0 {
		t.Fatalf("old clients survived CA replacement: %+v", clients)
	}
	pending, err := db.Queries.CountPendingRegistrations(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if pending != 0 {
		t.Fatalf("old registrations survived CA replacement: %d", pending)
	}
}
