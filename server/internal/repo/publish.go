package repo

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type pendingPackage struct {
	entry sqlcdb.RepoChannelEntry
	soak  sqlcdb.UpsertSoakParams
}

func (s *Service) PublishBuild(ctx context.Context, projectID, releaseID string, artifactIDs []string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.publishBuildLocked(ctx, projectID, releaseID, artifactIDs)
}

func (s *Service) publishBuildLocked(ctx context.Context, projectID, releaseID string,
	artifactIDs []string) error {
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		return err
	}
	if project.RepoPublish == 0 {
		return nil
	}
	policy, err := s.projectPolicy(ctx, projectID)
	if err != nil {
		return err
	}
	settings, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return err
	}
	if settings.SigningInitialized == 0 {
		return fmt.Errorf("%w: initialize repository signing before publishing", ErrInvalid)
	}
	if len(artifactIDs) == 0 {
		return nil
	}
	soakSeconds := effectiveSoakSeconds(settings.SoakSeconds, policy)

	var pending []pendingPackage
	arches := map[string]struct{}{}
	for _, id := range artifactIDs {
		item, err := s.preparePublishedPackage(ctx, soakSeconds, project, releaseID, id)
		if err != nil {
			return err
		}
		pending = append(pending, item)
		arches[item.entry.Arch] = struct{}{}
		if item.entry.Arch == "any" {
			for _, arch := range s.knownArches(ctx) {
				arches[arch] = struct{}{}
			}
		}
	}

	type materialized struct {
		arch     string
		dbID     string
		dbSig    sql.NullString
		filesID  sql.NullString
		filesSig sql.NullString
	}
	var built []materialized
	for arch := range arches {
		if arch == "any" {
			continue
		}
		entries, err := s.mergedChannelEntries(ctx, ChannelUnstable, arch, pending)
		if err != nil {
			return err
		}
		dbID, dbSig, filesID, filesSig, err := s.materializeDatabase(ctx, ChannelUnstable, arch, entries)
		if err != nil {
			return err
		}
		built = append(built, materialized{arch: arch, dbID: dbID, dbSig: dbSig, filesID: filesID, filesSig: filesSig})
	}

	now := s.nowString()
	for _, item := range pending {
		if settings.StableEnabled != 0 {
			if err := s.DB.Queries.UpsertSoak(ctx, item.soak); err != nil {
				return err
			}
		}
		item.entry.PublishedAt = now
		if err := s.upsertChannel(ctx, item.entry); err != nil {
			return err
		}
		if _, err := s.DB.Queries.GetRepoPackageByName(ctx, item.entry.Pkgname); errors.Is(err, sql.ErrNoRows) {
			if _, err := s.DB.Queries.InsertRepoPackage(ctx, sqlcdb.InsertRepoPackageParams{
				Pkgname:         item.entry.Pkgname,
				ProjectID:       ns(project.ID),
				OriginalPkgname: originalName(project),
				Internal:        0,
				CreatedAt:       now,
			}); err != nil {
				return err
			}
		} else if err != nil {
			return err
		}
	}
	for _, item := range built {
		if item.dbID == "" {
			if err := s.DB.Queries.DeleteRepoDatabase(ctx, sqlcdb.DeleteRepoDatabaseParams{
				Channel: ChannelUnstable,
				Arch:    item.arch,
			}); err != nil {
				return err
			}
			continue
		}
		if err := s.DB.Queries.UpsertRepoDatabase(ctx, sqlcdb.UpsertRepoDatabaseParams{
			Channel:            ChannelUnstable,
			Arch:               item.arch,
			DbArtifactID:       item.dbID,
			DbSigArtifactID:    item.dbSig,
			FilesArtifactID:    item.filesID,
			FilesSigArtifactID: item.filesSig,
			GeneratedAt:        now,
		}); err != nil {
			return err
		}
	}
	if project.RepoPublishedPkgname == "" && len(pending) > 0 {
		_, _ = s.DB.Queries.UpdateProjectRepo(ctx, sqlcdb.UpdateProjectRepoParams{
			RepoPublish:          project.RepoPublish,
			RepoPkgnameOverride:  project.RepoPkgnameOverride,
			RepoPublishedPkgname: pending[0].entry.Pkgname,
			ModifiedAt:           now,
			ID:                   project.ID,
			Revision:             project.Revision,
		})
	}
	if settings.StableEnabled != 0 && policy.AutomaticSoak {
		_, err = s.evaluateSoaksLocked(ctx)
		return err
	}
	return nil
}

func (s *Service) preparePublishedPackage(ctx context.Context, soakSeconds int64,
	project sqlcdb.Project, releaseID, artifactID string) (pendingPackage, error) {
	record, file, err := s.Artifacts.Open(ctx, artifactID)
	if err != nil {
		return pendingPackage{}, err
	}
	info, err := ReadPKGINFO(file)
	_ = file.Close()
	if err != nil {
		return pendingPackage{}, fmt.Errorf("%w: built package is not a valid ALPM archive: %s", ErrInvalid, err.Error())
	}
	if IsReserved(info.Name) && info.Name != KeyringPackage {
		return pendingPackage{}, fmt.Errorf("%w: %s is reserved", ErrInvalid, info.Name)
	}
	if err := s.assertNameAvailable(ctx, project.ID, info.Name); err != nil {
		return pendingPackage{}, err
	}
	claimed, err := s.DB.Queries.GetRepoPackageByProject(ctx, ns(project.ID))
	if err == nil && claimed.Pkgname != info.Name {
		_ = s.DB.Queries.DeleteRepoPackageByProject(ctx, ns(project.ID))
	} else if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return pendingPackage{}, err
	}

	sigID, err := s.signArtifactLocked(ctx, record.ID, record.OriginalFilename)
	if err != nil {
		return pendingPackage{}, err
	}

	now := s.nowString()
	eligible := s.now().Add(time.Duration(soakSeconds) * time.Second).Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
	existing, err := s.DB.Queries.GetSoak(ctx, sqlcdb.GetSoakParams{
		Pkgname: info.Name,
		Arch:    info.Arch,
		Pkgver:  info.Pkgver,
	})
	resetSoak := true
	if err == nil && existing.ArtifactID == record.ID && existing.Pkgrel == info.Pkgrel {
		resetSoak = false
	}
	started := now
	status := SoakSoaking
	if !resetSoak {
		started = existing.SoakStartedAt
		eligible = existing.EligibleAt
		status = existing.Status
		if status == SoakPromoted || status == SoakSkipped {
			status = SoakSoaking
			started = now
			eligible = s.now().Add(time.Duration(soakSeconds) * time.Second).Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
		}
	} else if err == nil && (existing.ArtifactID != record.ID || existing.Pkgrel != info.Pkgrel) {
		status = SoakSoaking
		started = now
	}

	filename := PackageFilename(info)
	entry := sqlcdb.RepoChannelEntry{
		Channel:       ChannelUnstable,
		Arch:          info.Arch,
		Pkgname:       info.Name,
		ProjectID:     ns(project.ID),
		ReleaseID:     ns(releaseID),
		Epoch:         info.Epoch,
		Pkgver:        info.Pkgver,
		Pkgrel:        info.Pkgrel,
		ArtifactID:    record.ID,
		SigArtifactID: ns(sigID),
		Filename:      filename,
		PublishedAt:   now,
	}
	return pendingPackage{
		entry: entry,
		soak: sqlcdb.UpsertSoakParams{
			Pkgname:       info.Name,
			Arch:          info.Arch,
			Pkgver:        info.Pkgver,
			ProjectID:     ns(project.ID),
			ReleaseID:     ns(releaseID),
			Epoch:         info.Epoch,
			Pkgrel:        info.Pkgrel,
			ArtifactID:    record.ID,
			SigArtifactID: ns(sigID),
			SoakStartedAt: started,
			EligibleAt:    eligible,
			Status:        status,
		},
	}, nil
}

func (s *Service) upsertChannel(ctx context.Context, entry sqlcdb.RepoChannelEntry) error {
	return s.DB.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
		Channel:       entry.Channel,
		Arch:          entry.Arch,
		Pkgname:       entry.Pkgname,
		ProjectID:     entry.ProjectID,
		ReleaseID:     entry.ReleaseID,
		Epoch:         entry.Epoch,
		Pkgver:        entry.Pkgver,
		Pkgrel:        entry.Pkgrel,
		ArtifactID:    entry.ArtifactID,
		SigArtifactID: entry.SigArtifactID,
		Filename:      entry.Filename,
		PublishedAt:   entry.PublishedAt,
	})
}

func (s *Service) mergedChannelEntries(ctx context.Context, channel, arch string, pending []pendingPackage) ([]sqlcdb.RepoChannelEntry, error) {
	existing, err := s.channelEntriesFor(ctx, channel, arch)
	if err != nil {
		return nil, err
	}
	byName := map[string]sqlcdb.RepoChannelEntry{}
	for _, entry := range existing {
		byName[entry.Pkgname] = entry
	}
	for _, item := range pending {
		if item.entry.Channel != channel {
			continue
		}
		if item.entry.Arch == arch || item.entry.Arch == "any" {
			entry := item.entry
			entry.Arch = item.entry.Arch
			byName[entry.Pkgname] = entry
		}
	}
	out := make([]sqlcdb.RepoChannelEntry, 0, len(byName))
	for _, entry := range byName {
		out = append(out, entry)
	}
	return out, nil
}

func (s *Service) channelEntriesFor(ctx context.Context, channel, arch string) ([]sqlcdb.RepoChannelEntry, error) {
	entries, err := s.DB.Queries.ListChannelEntriesForChannelArch(ctx, sqlcdb.ListChannelEntriesForChannelArchParams{
		Channel: channel,
		Arch:    arch,
	})
	if err != nil {
		return nil, err
	}
	anyEntries, err := s.DB.Queries.ListChannelEntriesForChannelArch(ctx, sqlcdb.ListChannelEntriesForChannelArchParams{
		Channel: channel,
		Arch:    "any",
	})
	if err != nil {
		return nil, err
	}
	combined := append([]sqlcdb.RepoChannelEntry{}, entries...)
	seen := map[string]struct{}{}
	for _, entry := range entries {
		seen[entry.Pkgname] = struct{}{}
	}
	for _, entry := range anyEntries {
		if _, ok := seen[entry.Pkgname]; ok {
			continue
		}
		combined = append(combined, entry)
	}
	return combined, nil
}

func (s *Service) signArtifactLocked(ctx context.Context, artifactID, filename string) (string, error) {
	record, file, err := s.Artifacts.Open(ctx, artifactID)
	if err != nil {
		return "", err
	}
	defer file.Close()
	work := s.workPath("sign")
	if err := ensureDir(work); err != nil {
		return "", err
	}
	dest := filepath.Join(work, filepath.Base(filename))
	out, err := os.OpenFile(dest, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o600)
	if err != nil {
		return "", err
	}
	if _, err := io.Copy(out, file); err != nil {
		_ = out.Close()
		return "", err
	}
	if err := out.Close(); err != nil {
		return "", err
	}
	sigPath, err := s.signFile(ctx, dest)
	if err != nil {
		return "", err
	}
	sig, err := os.Open(sigPath)
	if err != nil {
		return "", err
	}
	defer sig.Close()
	stored, err := s.Artifacts.Put(ctx, filepath.Base(sigPath), "package_sig", sig)
	if err != nil {
		return "", err
	}
	_ = os.Remove(dest)
	_ = os.Remove(sigPath)
	_ = record
	return stored.ID, nil
}

func (s *Service) rebuildDatabaseLocked(ctx context.Context, channel, arch string) error {
	entries, err := s.channelEntriesFor(ctx, channel, arch)
	if err != nil {
		return err
	}
	dbID, dbSig, filesID, filesSig, err := s.materializeDatabase(ctx, channel, arch, entries)
	if err != nil {
		return err
	}
	if dbID == "" {
		return s.DB.Queries.DeleteRepoDatabase(ctx, sqlcdb.DeleteRepoDatabaseParams{
			Channel: channel,
			Arch:    arch,
		})
	}
	return s.DB.Queries.UpsertRepoDatabase(ctx, sqlcdb.UpsertRepoDatabaseParams{
		Channel:            channel,
		Arch:               arch,
		DbArtifactID:       dbID,
		DbSigArtifactID:    dbSig,
		FilesArtifactID:    filesID,
		FilesSigArtifactID: filesSig,
		GeneratedAt:        s.nowString(),
	})
}

func (s *Service) materializeDatabase(ctx context.Context, channel, arch string, entries []sqlcdb.RepoChannelEntry) (string, sql.NullString, sql.NullString, sql.NullString, error) {
	if !ValidChannel(channel) {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, fmt.Errorf("%w: unknown channel", ErrInvalid)
	}
	for _, entry := range entries {
		if _, err := s.Artifacts.Get(ctx, entry.ArtifactID); err != nil {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, fmt.Errorf("%w: package %s is not available for download", ErrInvalid, entry.Filename)
		}
		if entry.SigArtifactID.Valid {
			if _, err := s.Artifacts.Get(ctx, entry.SigArtifactID.String); err != nil {
				return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, fmt.Errorf("%w: signature for %s is not available", ErrInvalid, entry.Filename)
			}
		}
	}
	if err := os.MkdirAll(s.WorkDir, 0o700); err != nil {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
	}
	work, err := os.MkdirTemp(s.WorkDir, "repo-"+channel+"-"+arch+"-*")
	if err != nil {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
	}
	defer os.RemoveAll(work)
	_ = os.Chmod(work, 0o700)

	var pkgFiles []string
	for _, entry := range entries {
		dest := filepath.Join(work, entry.Filename)
		if err := s.linkArtifact(ctx, entry.ArtifactID, dest); err != nil {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
		}
		if entry.SigArtifactID.Valid {
			if err := s.linkArtifact(ctx, entry.SigArtifactID.String, dest+".sig"); err != nil {
				return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
			}
		}
		pkgFiles = append(pkgFiles, dest)
	}

	dbPath := filepath.Join(work, RepoName+".db.tar.gz")
	if len(pkgFiles) == 0 {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, nil
	}
	args := []string{"--nocolor", "--include-sigs"}
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
	}
	if row.SigningInitialized != 0 && row.SigningFingerprint != "" {
		if err := s.ensureSigningHome(ctx); err != nil {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
		}
		args = append(args, "--sign", "--key", row.SigningFingerprint)
	}
	args = append(args, dbPath)
	args = append(args, pkgFiles...)
	cmd := exec.CommandContext(ctx, "/usr/bin/repo-add", args...)
	cmd.Dir = work
	cmd.Env = append(os.Environ(), "GNUPGHOME="+s.GnuPGHome)
	if out, err := cmd.CombinedOutput(); err != nil {
		msg := strings.TrimSpace(string(out))
		if msg == "" {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, fmt.Errorf("repo-add: %w", err)
		}
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, fmt.Errorf("repo-add: %s", msg)
	}

	dbRec, err := s.ingestFile(ctx, dbPath, "repo_db")
	if err != nil {
		return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
	}
	var dbSig, filesRec, filesSig sql.NullString
	if _, err := os.Stat(dbPath + ".sig"); err == nil {
		rec, err := s.ingestFile(ctx, dbPath+".sig", "repo_db_sig")
		if err != nil {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
		}
		dbSig = ns(rec)
	}
	filesPath := filepath.Join(work, RepoName+".files.tar.gz")
	if _, err := os.Stat(filesPath); err == nil {
		rec, err := s.ingestFile(ctx, filesPath, "repo_files")
		if err != nil {
			return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
		}
		filesRec = ns(rec)
		if _, err := os.Stat(filesPath + ".sig"); err == nil {
			sig, err := s.ingestFile(ctx, filesPath+".sig", "repo_files_sig")
			if err != nil {
				return "", sql.NullString{}, sql.NullString{}, sql.NullString{}, err
			}
			filesSig = ns(sig)
		}
	}
	return dbRec, dbSig, filesRec, filesSig, nil
}

func (s *Service) ingestFile(ctx context.Context, path, kind string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	record, err := s.Artifacts.Put(ctx, filepath.Base(path), kind, file)
	if err != nil {
		return "", err
	}
	return record.ID, nil
}

func (s *Service) linkArtifact(ctx context.Context, artifactID, dest string) error {
	record, err := s.Artifacts.Get(ctx, artifactID)
	if err != nil {
		return err
	}
	src, err := s.Artifacts.Store.Path(record.SHA256)
	if err != nil {
		return err
	}
	_ = os.Remove(dest)
	if err := os.Link(src, dest); err == nil {
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dest, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func (s *Service) knownArches(ctx context.Context) []string {
	seen := map[string]struct{}{s.hostArch(): {}}
	arches, err := s.DB.Queries.ListChannelArches(ctx)
	if err == nil {
		for _, arch := range arches {
			if arch != "" && arch != "any" {
				seen[arch] = struct{}{}
			}
		}
	}
	dbs, err := s.DB.Queries.ListRepoDatabases(ctx)
	if err == nil {
		for _, db := range dbs {
			if db.Arch != "" && db.Arch != "any" {
				seen[db.Arch] = struct{}{}
			}
		}
	}
	out := make([]string, 0, len(seen))
	for arch := range seen {
		out = append(out, arch)
	}
	return out
}

func (s *Service) hostArch() string {
	if s != nil && s.HostArch != "" {
		return s.HostArch
	}
	return hostArch()
}

func (s *Service) republishAllLocked(ctx context.Context) error {
	settings, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return err
	}
	if settings.SigningInitialized == 0 {
		return nil
	}
	arches := s.knownArches(ctx)
	channels := []string{ChannelUnstable}
	if settings.StableEnabled != 0 {
		channels = append(channels, ChannelStable)
	}
	for _, channel := range channels {
		for _, arch := range arches {
			if err := s.rebuildDatabaseLocked(ctx, channel, arch); err != nil {
				return err
			}
		}
	}
	return nil
}
