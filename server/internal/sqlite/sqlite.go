package sqlite

import (
	"context"
	"database/sql"
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"

	_ "modernc.org/sqlite"
)

//go:embed migrations/*.sql
var migrationFiles embed.FS

type DB struct {
	SQL     *sql.DB
	Queries *sqlcdb.Queries
	path    string
}

func Open(ctx context.Context, path string) (*DB, error) {
	if path == "" {
		return nil, fmt.Errorf("empty database path")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("create database directory: %w", err)
	}
	sqlDB, err := sql.Open("sqlite", dsn(path))
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}
	sqlDB.SetMaxOpenConns(1)
	sqlDB.SetConnMaxLifetime(0)
	if err := applyPragmas(ctx, sqlDB); err != nil {
		_ = sqlDB.Close()
		return nil, err
	}
	if err := migrate(ctx, sqlDB, migrationFiles); err != nil {
		_ = sqlDB.Close()
		return nil, err
	}
	db := &DB{SQL: sqlDB, Queries: sqlcdb.New(sqlDB), path: path}
	if err := db.ensureServerState(ctx); err != nil {
		_ = sqlDB.Close()
		return nil, err
	}
	if err := os.Chmod(path, 0o600); err != nil {
		_ = sqlDB.Close()
		return nil, fmt.Errorf("chmod database: %w", err)
	}
	return db, nil
}

func (db *DB) Close() error {
	if db == nil || db.SQL == nil {
		return nil
	}
	return db.SQL.Close()
}

func (db *DB) Ping(ctx context.Context) error {
	return db.SQL.PingContext(ctx)
}

func (db *DB) ensureServerState(ctx context.Context) error {
	_, err := db.Queries.GetServerState(ctx)
	if err == nil {
		return nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return fmt.Errorf("server state: %w", err)
	}
	return db.Queries.InsertServerState(ctx, sqlcdb.InsertServerStateParams{
		CreatedAt:     time.Now().UTC().Format(time.RFC3339Nano),
		SchemaKind:    "pacsmithd",
		SecretBackend: "",
		PkiReady:      0,
	})
}

func dsn(path string) string {
	return "file:" + filepath.ToSlash(path) + "?_pragma=foreign_keys(1)&_pragma=busy_timeout(5000)&_time_format=sqlite"
}

func applyPragmas(ctx context.Context, db *sql.DB) error {
	statements := []string{
		"PRAGMA foreign_keys = ON",
		"PRAGMA busy_timeout = 5000",
		"PRAGMA journal_mode = WAL",
		"PRAGMA synchronous = FULL",
	}
	for _, statement := range statements {
		if _, err := db.ExecContext(ctx, statement); err != nil {
			return fmt.Errorf("%s: %w", statement, err)
		}
	}
	return nil
}

var migrationName = regexp.MustCompile(`^(\d{4})_([a-z0-9_]+)\.sql$`)

type migration struct {
	version int64
	name    string
	sql     string
}

func migrate(ctx context.Context, db *sql.DB, files fs.FS) error {
	if _, err := db.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version INTEGER PRIMARY KEY,
		name TEXT NOT NULL,
		applied_at TEXT NOT NULL
	)`); err != nil {
		return fmt.Errorf("create schema_migrations: %w", err)
	}
	migrations, err := loadMigrations(files)
	if err != nil {
		return err
	}
	for _, item := range migrations {
		applied, err := migrationApplied(ctx, db, item.version)
		if err != nil {
			return err
		}
		if applied {
			continue
		}
		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, item.sql); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("migration %04d_%s: %w", item.version, item.name, err)
		}
		if _, err := tx.ExecContext(ctx,
			`INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, ?, ?)`,
			item.version, item.name, time.Now().UTC().Format(time.RFC3339Nano),
		); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("record migration %04d_%s: %w", item.version, item.name, err)
		}
		if err := tx.Commit(); err != nil {
			return err
		}
	}
	return nil
}

func loadMigrations(files fs.FS) ([]migration, error) {
	entries, err := fs.ReadDir(files, "migrations")
	if err != nil {
		return nil, fmt.Errorf("list migrations: %w", err)
	}
	var result []migration
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		match := migrationName.FindStringSubmatch(entry.Name())
		if match == nil {
			return nil, fmt.Errorf("migration filename %q must match NNNN_name.sql", entry.Name())
		}
		version, _ := strconv.ParseInt(match[1], 10, 64)
		body, err := fs.ReadFile(files, "migrations/"+entry.Name())
		if err != nil {
			return nil, err
		}
		result = append(result, migration{
			version: version,
			name:    match[2],
			sql:     strings.TrimSpace(string(body)),
		})
	}
	sort.Slice(result, func(i, j int) bool { return result[i].version < result[j].version })
	for i := range result {
		if result[i].version != int64(i+1) {
			return nil, fmt.Errorf("migrations must be contiguous starting at 0001; missing %04d", i+1)
		}
	}
	return result, nil
}

func migrationApplied(ctx context.Context, db *sql.DB, version int64) (bool, error) {
	var found int
	err := db.QueryRowContext(ctx, `SELECT 1 FROM schema_migrations WHERE version = ?`, version).Scan(&found)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return true, nil
}
