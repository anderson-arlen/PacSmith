package repo

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) EvaluateSoaks(ctx context.Context) error {
	_, err := s.EvaluateSoaksChanged(ctx)
	return err
}

func (s *Service) EvaluateSoaksChanged(ctx context.Context) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.evaluateSoaksLocked(ctx)
}

func (s *Service) evaluateSoaksLocked(ctx context.Context) (bool, error) {
	changed := false
	now := s.now()
	soaks, err := s.DB.Queries.ListSoaks(ctx)
	if err != nil {
		return false, err
	}
	for _, soak := range soaks {
		if soak.Status != SoakSoaking {
			continue
		}
		eligibleAt, err := parseTime(soak.EligibleAt)
		if err != nil {
			continue
		}
		if !eligibleAt.After(now) {
			if err := s.DB.Queries.UpdateSoakStatus(ctx, sqlcdb.UpdateSoakStatusParams{
				Status:  SoakEligible,
				Pkgname: soak.Pkgname,
				Arch:    soak.Arch,
				Pkgver:  soak.Pkgver,
			}); err != nil {
				return false, err
			}
			changed = true
		}
	}
	soaks, err = s.DB.Queries.ListSoaks(ctx)
	if err != nil {
		return false, err
	}
	groups := map[string][]sqlcdb.RepoSoak{}
	for _, soak := range soaks {
		key := soak.Pkgname + "\x00" + soak.Arch
		groups[key] = append(groups[key], soak)
	}
	for _, group := range groups {
		var eligible []sqlcdb.RepoSoak
		for _, soak := range group {
			if soak.Status == SoakEligible {
				eligible = append(eligible, soak)
			}
		}
		if len(eligible) == 0 {
			continue
		}
		stable, err := s.DB.Queries.GetChannelEntry(ctx, sqlcdb.GetChannelEntryParams{
			Channel: ChannelStable,
			Arch:    group[0].Arch,
			Pkgname: group[0].Pkgname,
		})
		var stableEpoch int64
		stableVer := ""
		stableRel := ""
		if err == nil {
			stableEpoch, stableVer, stableRel = stable.Epoch, stable.Pkgver, stable.Pkgrel
		} else if !errors.Is(err, sql.ErrNoRows) {
			return false, err
		}
		var best *sqlcdb.RepoSoak
		for i := range eligible {
			soak := eligible[i]
			ok, err := Advances(soak.Epoch, soak.Pkgver, soak.Pkgrel, stableEpoch, stableVer, stableRel)
			if err != nil {
				return false, err
			}
			if !ok {
				if err := s.DB.Queries.UpdateSoakStatus(ctx, sqlcdb.UpdateSoakStatusParams{
					Status:  SoakSkipped,
					Pkgname: soak.Pkgname,
					Arch:    soak.Arch,
					Pkgver:  soak.Pkgver,
				}); err != nil {
					return false, err
				}
				changed = true
				continue
			}
			if best == nil {
				copy := eligible[i]
				best = &copy
				continue
			}
			cmp, err := CompareVersions(
				VersionString(soak.Epoch, soak.Pkgver, soak.Pkgrel),
				VersionString(best.Epoch, best.Pkgver, best.Pkgrel),
			)
			if err != nil {
				return false, err
			}
			if cmp > 0 {
				copy := eligible[i]
				best = &copy
			}
		}
		if best == nil {
			continue
		}
		if err := s.promoteLocked(ctx, *best, false); err != nil {
			return false, err
		}
		if err := s.publishStableDBs(ctx, *best); err != nil {
			return false, err
		}
		changed = true
		for _, soak := range eligible {
			if soak.Pkgver == best.Pkgver && soak.Pkgname == best.Pkgname && soak.Arch == best.Arch {
				continue
			}
			ok, err := Advances(soak.Epoch, soak.Pkgver, soak.Pkgrel, best.Epoch, best.Pkgver, best.Pkgrel)
			if err != nil {
				return false, err
			}
			if !ok {
				_ = s.DB.Queries.UpdateSoakStatus(ctx, sqlcdb.UpdateSoakStatusParams{
					Status:  SoakSkipped,
					Pkgname: soak.Pkgname,
					Arch:    soak.Arch,
					Pkgver:  soak.Pkgver,
				})
			}
		}
	}
	return changed, nil
}

func (s *Service) Promote(ctx context.Context, projectID, pkgver, arch string) (ProjectStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ProjectStatus{}, ErrNotFound
		}
		return ProjectStatus{}, err
	}
	soaks, err := s.DB.Queries.ListSoaks(ctx)
	if err != nil {
		return ProjectStatus{}, err
	}
	var chosen *sqlcdb.RepoSoak
	for i := range soaks {
		soak := soaks[i]
		if !soak.ProjectID.Valid || soak.ProjectID.String != project.ID {
			continue
		}
		if pkgver != "" && soak.Pkgver != pkgver {
			continue
		}
		if arch != "" && soak.Arch != arch {
			continue
		}
		if soak.Status == SoakPromoted || soak.Status == SoakSkipped {
			continue
		}
		if chosen == nil {
			copy := soaks[i]
			chosen = &copy
			continue
		}
		cmp, err := CompareVersions(
			VersionString(soak.Epoch, soak.Pkgver, soak.Pkgrel),
			VersionString(chosen.Epoch, chosen.Pkgver, chosen.Pkgrel),
		)
		if err != nil {
			return ProjectStatus{}, err
		}
		if cmp > 0 {
			copy := soaks[i]
			chosen = &copy
		}
	}
	if chosen == nil {
		unstable, err := s.unstableForProject(ctx, project.ID, arch)
		if err != nil {
			return ProjectStatus{}, err
		}
		if unstable == nil {
			return ProjectStatus{}, fmt.Errorf("%w: nothing to promote to stable", ErrInvalid)
		}
		if err := s.promoteEntryLocked(ctx, *unstable, true); err != nil {
			return ProjectStatus{}, err
		}
		if err := s.publishStableDBs(ctx, sqlcdb.RepoSoak{
			Pkgname:       unstable.Pkgname,
			Arch:          unstable.Arch,
			Pkgver:        unstable.Pkgver,
			Epoch:         unstable.Epoch,
			Pkgrel:        unstable.Pkgrel,
			ArtifactID:    unstable.ArtifactID,
			SigArtifactID: unstable.SigArtifactID,
			ProjectID:     unstable.ProjectID,
			ReleaseID:     unstable.ReleaseID,
		}); err != nil {
			return ProjectStatus{}, err
		}
		return s.projectViewLocked(ctx, projectID)
	}
	if err := s.promoteLocked(ctx, *chosen, true); err != nil {
		return ProjectStatus{}, err
	}
	if err := s.publishStableDBs(ctx, *chosen); err != nil {
		return ProjectStatus{}, err
	}
	return s.projectViewLocked(ctx, projectID)
}

func (s *Service) promoteLocked(ctx context.Context, soak sqlcdb.RepoSoak, manual bool) error {
	stable, err := s.DB.Queries.GetChannelEntry(ctx, sqlcdb.GetChannelEntryParams{
		Channel: ChannelStable,
		Arch:    soak.Arch,
		Pkgname: soak.Pkgname,
	})
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return err
	}
	if err == nil {
		ok, cmpErr := Advances(soak.Epoch, soak.Pkgver, soak.Pkgrel, stable.Epoch, stable.Pkgver, stable.Pkgrel)
		if cmpErr != nil {
			return cmpErr
		}
		if !ok {
			if !manual {
				return s.DB.Queries.UpdateSoakStatus(ctx, sqlcdb.UpdateSoakStatusParams{
					Status:  SoakSkipped,
					Pkgname: soak.Pkgname,
					Arch:    soak.Arch,
					Pkgver:  soak.Pkgver,
				})
			}
			return fmt.Errorf("%w: refusing to downgrade stable %s", ErrInvalid, VersionString(stable.Epoch, stable.Pkgver, stable.Pkgrel))
		}
	}
	filename := PackageFilename(PackageInfo{
		Name:   soak.Pkgname,
		Pkgver: soak.Pkgver,
		Pkgrel: soak.Pkgrel,
		Arch:   soak.Arch,
	})
	if err := s.DB.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
		Channel:       ChannelStable,
		Arch:          soak.Arch,
		Pkgname:       soak.Pkgname,
		ProjectID:     soak.ProjectID,
		ReleaseID:     soak.ReleaseID,
		Epoch:         soak.Epoch,
		Pkgver:        soak.Pkgver,
		Pkgrel:        soak.Pkgrel,
		ArtifactID:    soak.ArtifactID,
		SigArtifactID: soak.SigArtifactID,
		Filename:      filename,
		PublishedAt:   s.nowString(),
	}); err != nil {
		return err
	}
	return s.DB.Queries.UpdateSoakStatus(ctx, sqlcdb.UpdateSoakStatusParams{
		Status:  SoakPromoted,
		Pkgname: soak.Pkgname,
		Arch:    soak.Arch,
		Pkgver:  soak.Pkgver,
	})
}

func channelEntryFromSoak(soak sqlcdb.RepoSoak, channel string) sqlcdb.RepoChannelEntry {
	return sqlcdb.RepoChannelEntry{
		Channel:       channel,
		Arch:          soak.Arch,
		Pkgname:       soak.Pkgname,
		ProjectID:     soak.ProjectID,
		ReleaseID:     soak.ReleaseID,
		Epoch:         soak.Epoch,
		Pkgver:        soak.Pkgver,
		Pkgrel:        soak.Pkgrel,
		ArtifactID:    soak.ArtifactID,
		SigArtifactID: soak.SigArtifactID,
		Filename: PackageFilename(PackageInfo{
			Name:   soak.Pkgname,
			Pkgver: soak.Pkgver,
			Pkgrel: soak.Pkgrel,
			Arch:   soak.Arch,
		}),
	}
}

func (s *Service) publishStableDBs(ctx context.Context, soak sqlcdb.RepoSoak) error {
	pending := pendingPackage{entry: channelEntryFromSoak(soak, ChannelStable)}
	targets := []string{soak.Arch}
	if soak.Arch == "any" {
		targets = s.knownArches(ctx)
	}
	now := s.nowString()
	for _, arch := range targets {
		if arch == "any" {
			continue
		}
		entries, err := s.mergedChannelEntries(ctx, ChannelStable, arch, []pendingPackage{pending})
		if err != nil {
			return err
		}
		dbID, dbSig, filesID, filesSig, err := s.materializeDatabase(ctx, ChannelStable, arch, entries)
		if err != nil {
			return err
		}
		if dbID == "" {
			if err := s.DB.Queries.DeleteRepoDatabase(ctx, sqlcdb.DeleteRepoDatabaseParams{
				Channel: ChannelStable,
				Arch:    arch,
			}); err != nil {
				return err
			}
			continue
		}
		if err := s.DB.Queries.UpsertRepoDatabase(ctx, sqlcdb.UpsertRepoDatabaseParams{
			Channel:            ChannelStable,
			Arch:               arch,
			DbArtifactID:       dbID,
			DbSigArtifactID:    dbSig,
			FilesArtifactID:    filesID,
			FilesSigArtifactID: filesSig,
			GeneratedAt:        now,
		}); err != nil {
			return err
		}
	}
	return nil
}

func (s *Service) promoteEntryLocked(ctx context.Context, entry sqlcdb.RepoChannelEntry, manual bool) error {
	return s.promoteLocked(ctx, sqlcdb.RepoSoak{
		Pkgname:       entry.Pkgname,
		Arch:          entry.Arch,
		Pkgver:        entry.Pkgver,
		ProjectID:     entry.ProjectID,
		ReleaseID:     entry.ReleaseID,
		Epoch:         entry.Epoch,
		Pkgrel:        entry.Pkgrel,
		ArtifactID:    entry.ArtifactID,
		SigArtifactID: entry.SigArtifactID,
		Status:        SoakEligible,
	}, manual)
}

func (s *Service) unstableForProject(ctx context.Context, projectID, arch string) (*sqlcdb.RepoChannelEntry, error) {
	entries, err := s.DB.Queries.ListChannelEntries(ctx)
	if err != nil {
		return nil, err
	}
	for i := range entries {
		entry := entries[i]
		if entry.Channel != ChannelUnstable {
			continue
		}
		if !entry.ProjectID.Valid || entry.ProjectID.String != projectID {
			continue
		}
		if arch != "" && entry.Arch != arch {
			continue
		}
		return &entries[i], nil
	}
	return nil, nil
}

func parseTime(value string) (time.Time, error) {
	if t, err := time.Parse(time.RFC3339Nano, value); err == nil {
		return t, nil
	}
	if t, err := time.Parse(time.RFC3339, value); err == nil {
		return t, nil
	}
	return time.Parse("2006-01-02T15:04:05.000Z07:00", value)
}
