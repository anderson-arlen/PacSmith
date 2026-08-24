package repo

import (
	"fmt"
	"net/http"
	"path"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /repo/{channel}/{arch}/{file}", s.serveRepoFile)
	mux.HandleFunc("HEAD /repo/{channel}/{arch}/{file}", s.serveRepoFile)
	mux.HandleFunc("GET /bootstrap/{file}", s.serveBootstrap)
	mux.HandleFunc("HEAD /bootstrap/{file}", s.serveBootstrap)
	return mux
}

func (s *Service) serveRepoFile(w http.ResponseWriter, r *http.Request) {
	channel := r.PathValue("channel")
	arch := r.PathValue("arch")
	file := path.Base(r.PathValue("file"))
	if !ValidChannel(channel) || arch == "" || file == "" || strings.Contains(file, "..") {
		http.NotFound(w, r)
		return
	}
	id, ok := s.lookupRepoFile(r, channel, arch, file)
	if !ok {
		http.NotFound(w, r)
		return
	}
	s.serveArtifact(w, r, id)
}

func (s *Service) lookupRepoFile(r *http.Request, channel, arch, file string) (string, bool) {
	ctx := r.Context()
	switch file {
	case RepoName + ".db", RepoName + ".db.tar.gz":
		db, err := s.DB.Queries.GetRepoDatabase(ctx, sqlcdb.GetRepoDatabaseParams{Channel: channel, Arch: arch})
		if err != nil {
			return "", false
		}
		return db.DbArtifactID, true
	case RepoName + ".db.sig", RepoName + ".db.tar.gz.sig":
		db, err := s.DB.Queries.GetRepoDatabase(ctx, sqlcdb.GetRepoDatabaseParams{Channel: channel, Arch: arch})
		if err != nil || !db.DbSigArtifactID.Valid {
			return "", false
		}
		return db.DbSigArtifactID.String, true
	case RepoName + ".files", RepoName + ".files.tar.gz":
		db, err := s.DB.Queries.GetRepoDatabase(ctx, sqlcdb.GetRepoDatabaseParams{Channel: channel, Arch: arch})
		if err != nil || !db.FilesArtifactID.Valid {
			return "", false
		}
		return db.FilesArtifactID.String, true
	case RepoName + ".files.sig", RepoName + ".files.tar.gz.sig":
		db, err := s.DB.Queries.GetRepoDatabase(ctx, sqlcdb.GetRepoDatabaseParams{Channel: channel, Arch: arch})
		if err != nil || !db.FilesSigArtifactID.Valid {
			return "", false
		}
		return db.FilesSigArtifactID.String, true
	}
	wantSig := strings.HasSuffix(file, ".sig")
	name := strings.TrimSuffix(file, ".sig")
	for _, candidate := range []string{arch, "any"} {
		entries, err := s.DB.Queries.ListChannelEntriesForChannelArch(ctx, sqlcdb.ListChannelEntriesForChannelArchParams{
			Channel: channel,
			Arch:    candidate,
		})
		if err != nil {
			return "", false
		}
		for _, item := range entries {
			if item.Filename != name {
				continue
			}
			if wantSig {
				if !item.SigArtifactID.Valid {
					return "", false
				}
				return item.SigArtifactID.String, true
			}
			return item.ArtifactID, true
		}
	}
	return "", false
}

func (s *Service) serveBootstrap(w http.ResponseWriter, r *http.Request) {
	file := path.Base(r.PathValue("file"))
	ctx := r.Context()
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	switch file {
	case "pacsmith.gpg":
		if !row.KeyringGpgArtifactID.Valid {
			http.NotFound(w, r)
			return
		}
		s.serveArtifact(w, r, row.KeyringGpgArtifactID.String)
	case "pacsmith-trusted":
		if !row.KeyringTrustedArtifactID.Valid {
			http.NotFound(w, r)
			return
		}
		s.serveArtifact(w, r, row.KeyringTrustedArtifactID.String)
	case "pacsmith-revoked":
		if !row.KeyringRevokedArtifactID.Valid {
			http.NotFound(w, r)
			return
		}
		s.serveArtifact(w, r, row.KeyringRevokedArtifactID.String)
	case "pacsmith.asc":
		body, name, err := s.PublicKey(ctx)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/pgp-keys")
		w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename=%q`, name))
		http.ServeContent(w, r, name, time.Time{}, strings.NewReader(string(body)))
	case "setup-stable.sh":
		s.writeBootstrapScript(w, ChannelStable)
	case "setup-unstable.sh":
		s.writeBootstrapScript(w, ChannelUnstable)
	default:
		http.NotFound(w, r)
	}
}

func (s *Service) writeBootstrapScript(w http.ResponseWriter, channel string) {
	body, err := s.BootstrapScript(channel)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "text/x-shellscript")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", "setup-"+channel+".sh"))
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(body))
}

func (s *Service) serveArtifact(w http.ResponseWriter, r *http.Request, id string) {
	record, file, err := s.Artifacts.Open(r.Context(), id)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeContent(w, r, record.OriginalFilename, time.Time{}, file)
}
