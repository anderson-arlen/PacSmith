package repo

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type Service struct {
	DB        *sqlite.DB
	Artifacts *artifact.Registry
	Secrets   *secret.LockedStore
	WorkDir   string
	GnuPGHome string
	Now       func() time.Time
	HostArch  string

	mu sync.Mutex
}

type BuildPrep struct {
	PackageName  string
	OriginalName string
	Provides     []string
	Conflicts    []string
	Publish      bool
}

type Settings struct {
	Revision              int64    `json:"revision"`
	Enabled               bool     `json:"enabled"`
	ListenHosts           []string `json:"listen_hosts"`
	ListenPort            int      `json:"listen_port"`
	AdvertisedURL         string   `json:"advertised_url"`
	StableEnabled         bool     `json:"stable_enabled"`
	SoakSeconds           int64    `json:"soak_seconds"`
	PackageNamePrefix     string   `json:"package_name_prefix"`
	TrustMode             string   `json:"trust_mode"`
	SigningInitialized    bool     `json:"signing_initialized"`
	Fingerprint           string   `json:"fingerprint"`
	FingerprintSpaced     string   `json:"fingerprint_spaced"`
	RootFingerprint       string   `json:"root_fingerprint"`
	RootFingerprintSpaced string   `json:"root_fingerprint_spaced,omitempty"`
	Certified             bool     `json:"certified"`
	KeyringVersion        int64    `json:"keyring_version"`
	KeyringPackage        string   `json:"keyring_package,omitempty"`
	KeyringURL            string   `json:"keyring_url,omitempty"`
	Bound                 []string `json:"bound,omitempty"`
}

type SettingsPatch struct {
	Revision          int64     `json:"revision"`
	Enabled           *bool     `json:"enabled"`
	ListenHosts       *[]string `json:"listen_hosts"`
	ListenPort        *int      `json:"listen_port"`
	AdvertisedURL     *string   `json:"advertised_url"`
	StableEnabled     *bool     `json:"stable_enabled"`
	SoakSeconds       *int64    `json:"soak_seconds"`
	PackageNamePrefix *string   `json:"package_name_prefix"`
	TrustMode         *string   `json:"trust_mode"`
}

type ProjectPatch struct {
	Revision            int64   `json:"revision"`
	Publish             *bool   `json:"publish"`
	AutomaticSoak       *bool   `json:"automatic_soak"`
	SoakSecondsOverride *int64  `json:"soak_seconds_override"`
	Override            *string `json:"package_name_override"`
}

type PackageRef struct {
	Pkgname   string `json:"pkgname"`
	Arch      string `json:"arch"`
	Epoch     int64  `json:"epoch"`
	Pkgver    string `json:"pkgver"`
	Pkgrel    string `json:"pkgrel"`
	Version   string `json:"version"`
	Filename  string `json:"filename"`
	Artifact  string `json:"artifact_id"`
	Signature string `json:"signature_artifact_id,omitempty"`
	ReleaseID string `json:"release_id,omitempty"`
}

type SoakStatus struct {
	Pkgname    string `json:"pkgname"`
	Arch       string `json:"arch"`
	Pkgver     string `json:"pkgver"`
	Pkgrel     string `json:"pkgrel"`
	Version    string `json:"version"`
	Status     string `json:"status"`
	StartedAt  string `json:"soak_started_at"`
	EligibleAt string `json:"eligible_at"`
	ArtifactID string `json:"artifact_id"`
	ReleaseID  string `json:"release_id,omitempty"`
}

type ProjectStatus struct {
	Revision             int64        `json:"revision"`
	Publish              bool         `json:"publish"`
	OriginalPackageName  string       `json:"original_package_name"`
	ArchPackageName      string       `json:"arch_package_name"`
	PrefixDefault        string       `json:"prefix_default"`
	Override             string       `json:"package_name_override"`
	EffectivePackageName string       `json:"effective_package_name"`
	PublishedPackageName string       `json:"published_package_name"`
	PkgnameChangeWarning bool         `json:"pkgname_change_warning"`
	Reserved             bool         `json:"reserved"`
	StableChannelEnabled bool         `json:"stable_channel_enabled"`
	AutomaticSoak        bool         `json:"automatic_soak"`
	SoakSecondsOverride  int64        `json:"soak_seconds_override"`
	LibrarySoakSeconds   int64        `json:"library_soak_seconds"`
	EffectiveSoakSeconds int64        `json:"effective_soak_seconds"`
	Unstable             *PackageRef  `json:"unstable"`
	Stable               *PackageRef  `json:"stable"`
	Soaks                []SoakStatus `json:"soaks"`
}

func New(db *sqlite.DB, artifacts *artifact.Registry, secrets *secret.LockedStore, workDir, gnupgHome string) *Service {
	return &Service{
		DB:        db,
		Artifacts: artifacts,
		Secrets:   secrets,
		WorkDir:   workDir,
		GnuPGHome: gnupgHome,
		HostArch:  hostArch(),
	}
}

func hostArch() string {
	switch runtime.GOARCH {
	case "amd64":
		return "x86_64"
	case "arm64":
		return "aarch64"
	default:
		return runtime.GOARCH
	}
}

func (s *Service) now() time.Time {
	if s != nil && s.Now != nil {
		return s.Now().UTC()
	}
	return time.Now().UTC()
}

func (s *Service) nowString() string {
	return s.now().Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
}

func (s *Service) Settings(ctx context.Context) (Settings, error) {
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	return settingsFromRow(row), nil
}

func settingsFromRow(row sqlcdb.RepoSetting) Settings {
	settings := Settings{
		Revision:              row.Revision,
		Enabled:               row.Enabled != 0,
		ListenHosts:           decodeListenHosts(row.ListenHosts),
		ListenPort:            int(row.ListenPort),
		AdvertisedURL:         row.AdvertisedUrl,
		StableEnabled:         row.StableEnabled != 0,
		SoakSeconds:           row.SoakSeconds,
		PackageNamePrefix:     row.PackageNamePrefix,
		TrustMode:             row.TrustMode,
		SigningInitialized:    row.SigningInitialized != 0,
		Fingerprint:           row.SigningFingerprint,
		FingerprintSpaced:     FormatFingerprint(row.SigningFingerprint),
		RootFingerprint:       row.RootFingerprint,
		RootFingerprintSpaced: FormatFingerprint(row.RootFingerprint),
		Certified:             row.CertifiedPubkeyArtifactID.Valid,
		KeyringVersion:        row.KeyringVersion,
	}
	settings.KeyringPackage = KeyringPackageFilename(settings.KeyringVersion)
	settings.KeyringURL = KeyringPackageURL(settings)
	return settings
}

func (s Settings) ListenConfig() listen.Config {
	port := s.ListenPort
	if port <= 0 {
		port = DefaultListenPort
	}
	hosts := s.ListenHosts
	if len(hosts) == 0 {
		hosts = []string{DefaultListenHost}
	}
	return listen.Config{Enabled: s.Enabled, Port: port, Hosts: hosts}
}

func decodeListenHosts(raw string) []string {
	var hosts []string
	if json.Unmarshal([]byte(raw), &hosts) == nil && len(hosts) > 0 {
		return hosts
	}
	return []string{DefaultListenHost}
}

func encodeListenHosts(hosts []string) string {
	if len(hosts) == 0 {
		hosts = []string{DefaultListenHost}
	}
	raw, err := json.Marshal(hosts)
	if err != nil {
		return `["127.0.0.1"]`
	}
	return string(raw)
}

func (s *Service) PatchSettings(ctx context.Context, patch SettingsPatch) (Settings, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	current, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	revision := patch.Revision
	if revision == 0 {
		revision = current.Revision
	}
	next := current
	if patch.Enabled != nil {
		next.Enabled = boolInt(*patch.Enabled)
	}
	if patch.ListenPort != nil {
		if *patch.ListenPort < 1 || *patch.ListenPort > 65535 {
			return Settings{}, fmt.Errorf("%w: listen port must be between 1 and 65535", ErrInvalid)
		}
		next.ListenPort = int64(*patch.ListenPort)
	}
	if patch.ListenHosts != nil || patch.ListenPort != nil {
		hosts := decodeListenHosts(next.ListenHosts)
		if patch.ListenHosts != nil {
			hosts = *patch.ListenHosts
		}
		port := int(next.ListenPort)
		if port <= 0 {
			port = DefaultListenPort
		}
		cfg, err := listen.Normalize(listen.Config{
			Enabled: true,
			Port:    port,
			Hosts:   hosts,
		})
		if err != nil {
			return Settings{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
		}
		next.ListenHosts = encodeListenHosts(cfg.Hosts)
		next.ListenPort = int64(cfg.Port)
	}
	if patch.AdvertisedURL != nil {
		value := strings.TrimRight(strings.TrimSpace(*patch.AdvertisedURL), "/")
		if value != "" {
			parsed, err := url.Parse(value)
			if err != nil || parsed.Scheme == "" || parsed.Host == "" {
				return Settings{}, fmt.Errorf("%w: advertised repository URL is invalid", ErrInvalid)
			}
		}
		next.AdvertisedUrl = value
	}
	if patch.StableEnabled != nil {
		next.StableEnabled = boolInt(*patch.StableEnabled)
	}
	if patch.SoakSeconds != nil {
		if *patch.SoakSeconds < 0 {
			return Settings{}, fmt.Errorf("%w: soak duration cannot be negative", ErrInvalid)
		}
		next.SoakSeconds = *patch.SoakSeconds
	}
	if patch.PackageNamePrefix != nil {
		prefix, err := SanitizePrefix(*patch.PackageNamePrefix)
		if err != nil {
			return Settings{}, err
		}
		if err := s.ensurePrefixNames(ctx, prefix); err != nil {
			return Settings{}, err
		}
		next.PackageNamePrefix = prefix
	}
	if patch.TrustMode != nil {
		mode := strings.TrimSpace(*patch.TrustMode)
		if !ValidTrustMode(mode) {
			return Settings{}, fmt.Errorf("%w: trust mode must be direct or root-certified", ErrInvalid)
		}
		if mode == TrustRootCertified && next.CertifiedPubkeyArtifactID.String == "" {
			return Settings{}, fmt.Errorf("%w: upload a certified PacSmith public key before enabling root-certified trust", ErrInvalid)
		}
		next.TrustMode = mode
	}
	next.ModifiedAt = s.nowString()
	updated, err := s.DB.Queries.UpdateRepoSettings(ctx, updateParamsFrom(next, revision))
	if errors.Is(err, sql.ErrNoRows) {
		return Settings{}, fmt.Errorf("%w: revision conflict", ErrConflict)
	}
	if err != nil {
		return Settings{}, err
	}
	if next.TrustMode != current.TrustMode && next.SigningInitialized != 0 {
		if err := s.rebuildKeyringLocked(ctx, updated); err != nil {
			return Settings{}, err
		}
		updated, err = s.DB.Queries.GetRepoSettings(ctx)
		if err != nil {
			return Settings{}, err
		}
	}
	if next.StableEnabled == 0 && current.StableEnabled != 0 {
		if _, err := s.DB.SQL.ExecContext(ctx, `DELETE FROM repo_soaks`); err != nil {
			return Settings{}, err
		}
		if _, err := s.DB.SQL.ExecContext(ctx,
			`DELETE FROM repo_channel_entries WHERE channel = 'stable' AND project_id IS NOT NULL`); err != nil {
			return Settings{}, err
		}
		if _, err := s.DB.SQL.ExecContext(ctx, `DELETE FROM repo_databases WHERE channel = 'stable'`); err != nil {
			return Settings{}, err
		}
	}
	if next.StableEnabled != current.StableEnabled && next.StableEnabled != 0 {
		if err := s.republishAllLocked(ctx); err != nil {
			return Settings{}, err
		}
	}
	if next.StableEnabled != 0 && next.SoakSeconds != current.SoakSeconds {
		if err := s.rescheduleInheritedSoaks(ctx, next.SoakSeconds); err != nil {
			return Settings{}, err
		}
		if _, err := s.evaluateSoaksLocked(ctx); err != nil {
			return Settings{}, err
		}
	}
	return settingsFromRow(updated), nil
}

func (s *Service) ensurePrefixNames(ctx context.Context, prefix string) error {
	projects, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return err
	}
	seen := map[string]string{}
	for _, project := range projects {
		if project.RepoPublish == 0 {
			continue
		}
		original := originalName(project)
		effective, _ := EffectiveName(project.ArchPackageName, original, prefix, project.RepoPkgnameOverride)
		if IsReserved(effective) {
			return fmt.Errorf("%w: prefix would make %s use reserved name %s", ErrInvalid, project.DisplayName, effective)
		}
		if owner, ok := seen[effective]; ok {
			return fmt.Errorf("%w: prefix would cause %s and %s to share package name %s", ErrConflict, owner, project.DisplayName, effective)
		}
		seen[effective] = project.DisplayName
		existing, err := s.DB.Queries.GetRepoPackageByName(ctx, effective)
		if err == nil && existing.ProjectID.Valid && existing.ProjectID.String != project.ID && existing.Internal == 0 {
			return fmt.Errorf("%w: package name %s is already published by another project", ErrConflict, effective)
		}
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return err
		}
	}
	return nil
}

func updateParamsFrom(row sqlcdb.RepoSetting, revision int64) sqlcdb.UpdateRepoSettingsParams {
	return sqlcdb.UpdateRepoSettingsParams{
		Enabled:                     row.Enabled,
		ListenHosts:                 row.ListenHosts,
		ListenPort:                  row.ListenPort,
		AdvertisedUrl:               row.AdvertisedUrl,
		StableEnabled:               row.StableEnabled,
		SoakSeconds:                 row.SoakSeconds,
		PackageNamePrefix:           row.PackageNamePrefix,
		TrustMode:                   row.TrustMode,
		SigningFingerprint:          row.SigningFingerprint,
		SigningInitialized:          row.SigningInitialized,
		SigningPubkeyArtifactID:     row.SigningPubkeyArtifactID,
		RootPubkeyArtifactID:        row.RootPubkeyArtifactID,
		RootFingerprint:             row.RootFingerprint,
		CertifiedPubkeyArtifactID:   row.CertifiedPubkeyArtifactID,
		KeyringGpgArtifactID:        row.KeyringGpgArtifactID,
		KeyringTrustedArtifactID:    row.KeyringTrustedArtifactID,
		KeyringRevokedArtifactID:    row.KeyringRevokedArtifactID,
		KeyringPackageArtifactID:    row.KeyringPackageArtifactID,
		KeyringPackageSigArtifactID: row.KeyringPackageSigArtifactID,
		KeyringVersion:              row.KeyringVersion,
		ModifiedAt:                  row.ModifiedAt,
		Revision:                    revision,
	}
}

func (s *Service) PrepareBuild(ctx context.Context, projectID, releaseID string) (BuildPrep, error) {
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		return BuildPrep{}, err
	}
	release, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if err != nil {
		return BuildPrep{}, err
	}
	original := originalFromRelease(release, project)
	if project.RepoPublish == 0 {
		return BuildPrep{
			PackageName:  project.ArchPackageName,
			OriginalName: original,
			Publish:      false,
		}, nil
	}
	settings, err := s.Settings(ctx)
	if err != nil {
		return BuildPrep{}, err
	}
	effective, original := EffectiveName(project.ArchPackageName, original, settings.PackageNamePrefix, project.RepoPkgnameOverride)
	if IsReserved(effective) {
		return BuildPrep{}, fmt.Errorf("%w: %s is reserved by PacSmith", ErrInvalid, effective)
	}
	if err := s.assertNameAvailable(ctx, project.ID, effective); err != nil {
		return BuildPrep{}, err
	}
	return BuildPrep{
		PackageName:  effective,
		OriginalName: original,
		Publish:      true,
	}, nil
}

func (s *Service) assertNameAvailable(ctx context.Context, projectID, pkgname string) error {
	row, err := s.DB.Queries.GetRepoPackageByName(ctx, pkgname)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return err
	}
	if row.Internal != 0 {
		return fmt.Errorf("%w: %s is reserved by PacSmith", ErrInvalid, pkgname)
	}
	if row.ProjectID.Valid && row.ProjectID.String != projectID {
		return fmt.Errorf("%w: package name %s is already used by another PacSmith project", ErrConflict, pkgname)
	}
	return nil
}

func (s *Service) ProjectView(ctx context.Context, projectID string) (ProjectStatus, error) {
	return s.projectViewLocked(ctx, projectID)
}

func (s *Service) projectViewLocked(ctx context.Context, projectID string) (ProjectStatus, error) {
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ProjectStatus{}, ErrNotFound
		}
		return ProjectStatus{}, err
	}
	settings, err := s.Settings(ctx)
	if err != nil {
		return ProjectStatus{}, err
	}
	policy, err := s.projectPolicy(ctx, projectID)
	if err != nil {
		return ProjectStatus{}, err
	}
	original := originalName(project)
	releases, err := s.DB.Queries.ListReleasesForProject(ctx, project.ID)
	if err != nil {
		return ProjectStatus{}, err
	}
	if len(releases) > 0 {
		original = originalFromRelease(releases[len(releases)-1], project)
	}
	effective, original := EffectiveName(project.ArchPackageName, original, settings.PackageNamePrefix, project.RepoPkgnameOverride)
	prefixDefault, _ := EffectiveName(project.ArchPackageName, original, settings.PackageNamePrefix, "")
	status := ProjectStatus{
		Revision:             project.Revision,
		Publish:              project.RepoPublish != 0,
		OriginalPackageName:  original,
		ArchPackageName:      project.ArchPackageName,
		PrefixDefault:        prefixDefault,
		Override:             project.RepoPkgnameOverride,
		EffectivePackageName: effective,
		PublishedPackageName: project.RepoPublishedPkgname,
		PkgnameChangeWarning: project.RepoPublishedPkgname != "" && project.RepoPublishedPkgname != effective,
		Reserved:             IsReserved(effective),
		StableChannelEnabled: settings.StableEnabled,
		AutomaticSoak:        settings.StableEnabled && policy.AutomaticSoak,
		SoakSecondsOverride:  policy.SoakSecondsOverride,
		LibrarySoakSeconds:   settings.SoakSeconds,
		EffectiveSoakSeconds: effectiveSoakSeconds(settings.SoakSeconds, policy),
		Soaks:                []SoakStatus{},
	}
	entries, err := s.DB.Queries.ListChannelEntries(ctx)
	if err != nil {
		return ProjectStatus{}, err
	}
	for _, entry := range entries {
		if !entry.ProjectID.Valid || entry.ProjectID.String != project.ID {
			continue
		}
		ref := refFromEntry(entry)
		switch entry.Channel {
		case ChannelUnstable:
			status.Unstable = &ref
		case ChannelStable:
			if settings.StableEnabled {
				status.Stable = &ref
			}
		}
	}
	soaks, err := s.DB.Queries.ListSoaks(ctx)
	if err != nil {
		return ProjectStatus{}, err
	}
	for _, soak := range soaks {
		if !settings.StableEnabled {
			break
		}
		if !soak.ProjectID.Valid || soak.ProjectID.String != project.ID {
			continue
		}
		status.Soaks = append(status.Soaks, soakStatusFrom(soak))
	}
	return status, nil
}

func (s *Service) PatchProject(ctx context.Context, projectID string, patch ProjectPatch) (ProjectStatus, error) {
	return s.patchProject(ctx, projectID, patch, false)
}

func (s *Service) PatchProjectDeferred(ctx context.Context, projectID string, patch ProjectPatch) (ProjectStatus, error) {
	return s.patchProject(ctx, projectID, patch, true)
}

func (s *Service) patchProject(ctx context.Context, projectID string, patch ProjectPatch,
	deferReconcile bool) (ProjectStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ProjectStatus{}, ErrNotFound
		}
		return ProjectStatus{}, err
	}
	revision := patch.Revision
	if revision == 0 {
		revision = project.Revision
	}
	publish := project.RepoPublish
	override := project.RepoPkgnameOverride
	policy, err := s.projectPolicy(ctx, projectID)
	if err != nil {
		return ProjectStatus{}, err
	}
	previousPolicy := policy
	settings, err := s.Settings(ctx)
	if err != nil {
		return ProjectStatus{}, err
	}
	previousSoakSeconds := effectiveSoakSeconds(settings.SoakSeconds, previousPolicy)
	if patch.Publish != nil {
		publish = boolInt(*patch.Publish)
	}
	if patch.Override != nil {
		override = strings.TrimSpace(*patch.Override)
		if override != "" {
			override = strings.TrimSpace(override)
		}
	}
	if patch.AutomaticSoak != nil {
		policy.AutomaticSoak = *patch.AutomaticSoak
	}
	if patch.SoakSecondsOverride != nil {
		if *patch.SoakSecondsOverride < -1 {
			return ProjectStatus{}, fmt.Errorf("%w: project soak duration must be -1 or greater", ErrInvalid)
		}
		policy.SoakSecondsOverride = *patch.SoakSecondsOverride
	}
	if !settings.StableEnabled {
		policy.AutomaticSoak = false
	}
	if publish == 0 {
		policy.AutomaticSoak = false
	}
	nextSoakSeconds := effectiveSoakSeconds(settings.SoakSeconds, policy)
	original := originalName(project)
	effective, _ := EffectiveName(project.ArchPackageName, original, settings.PackageNamePrefix, override)
	if publish != 0 {
		if IsReserved(effective) {
			return ProjectStatus{}, fmt.Errorf("%w: %s is reserved by PacSmith", ErrInvalid, effective)
		}
		if err := s.assertNameAvailable(ctx, project.ID, effective); err != nil {
			return ProjectStatus{}, err
		}
	}
	publishedName := project.RepoPublishedPkgname
	if publish == 0 {
		publishedName = ""
	}
	_, err = s.DB.Queries.UpdateProjectRepo(ctx, sqlcdb.UpdateProjectRepoParams{
		RepoPublish:          publish,
		RepoPkgnameOverride:  override,
		RepoPublishedPkgname: publishedName,
		ModifiedAt:           s.nowString(),
		ID:                   project.ID,
		Revision:             revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return ProjectStatus{}, fmt.Errorf("%w: revision conflict", ErrConflict)
	}
	if err != nil {
		return ProjectStatus{}, err
	}
	if err := s.saveProjectPolicy(ctx, projectID, policy); err != nil {
		return ProjectStatus{}, err
	}
	if deferReconcile {
		return s.projectViewLocked(ctx, projectID)
	}
	rebuild := false
	if publish == 0 {
		if err := s.DB.Queries.DeleteChannelEntriesForProject(ctx, ns(projectID)); err != nil {
			return ProjectStatus{}, err
		}
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return ProjectStatus{}, err
		}
		if err := s.DB.Queries.DeleteRepoPackageByProject(ctx, ns(projectID)); err != nil {
			return ProjectStatus{}, err
		}
		rebuild = true
	} else if !settings.StableEnabled {
		if _, err := s.DB.SQL.ExecContext(ctx,
			`DELETE FROM repo_channel_entries WHERE project_id = ? AND channel = 'stable'`,
			projectID); err != nil {
			return ProjectStatus{}, err
		}
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return ProjectStatus{}, err
		}
		rebuild = true
	} else if !policy.AutomaticSoak {
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return ProjectStatus{}, err
		}
	}
	if publish != 0 && settings.StableEnabled && policy.AutomaticSoak &&
		previousSoakSeconds != nextSoakSeconds {
		if err := s.rescheduleProjectSoaks(ctx, projectID, nextSoakSeconds); err != nil {
			return ProjectStatus{}, err
		}
		if _, err := s.evaluateSoaksLocked(ctx); err != nil {
			return ProjectStatus{}, err
		}
	}
	if rebuild {
		if err := s.republishAllLocked(ctx); err != nil {
			return ProjectStatus{}, err
		}
	}
	if (project.RepoPublish == 0 && publish != 0) ||
		(publish != 0 && policy.AutomaticSoak && !previousPolicy.AutomaticSoak) {
		latest, err := s.latestSuccessfulBuild(ctx, projectID)
		if err != nil {
			return ProjectStatus{}, err
		}
		if latest != nil {
			if err := s.publishBuildLocked(ctx, projectID, latest.ReleaseID,
				latest.ArtifactIDs); err != nil {
				return ProjectStatus{}, err
			}
		}
	}
	return s.projectViewLocked(ctx, projectID)
}

func (s *Service) ReconcileProjectDistribution(ctx context.Context, projectID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	project, err := s.DB.Queries.GetProject(ctx, projectID)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		return err
	}
	settings, err := s.Settings(ctx)
	if err != nil {
		return err
	}
	policy, err := s.projectPolicy(ctx, projectID)
	if err != nil {
		return err
	}
	if project.RepoPublish == 0 {
		if err := s.DB.Queries.DeleteChannelEntriesForProject(ctx, ns(projectID)); err != nil {
			return err
		}
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return err
		}
		if err := s.DB.Queries.DeleteRepoPackageByProject(ctx, ns(projectID)); err != nil {
			return err
		}
		return s.republishAllLocked(ctx)
	}
	if !settings.StableEnabled {
		if _, err := s.DB.SQL.ExecContext(ctx,
			`DELETE FROM repo_channel_entries WHERE project_id = ? AND channel = 'stable'`,
			projectID); err != nil {
			return err
		}
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return err
		}
	} else if !policy.AutomaticSoak {
		if err := s.DB.Queries.DeleteSoaksForProject(ctx, ns(projectID)); err != nil {
			return err
		}
	} else if err := s.rescheduleProjectSoaks(ctx, projectID,
		effectiveSoakSeconds(settings.SoakSeconds, policy)); err != nil {
		return err
	}
	latest, err := s.latestSuccessfulBuild(ctx, projectID)
	if err != nil || latest == nil {
		return err
	}
	return s.publishBuildLocked(ctx, projectID, latest.ReleaseID, latest.ArtifactIDs)
}

func (s *Service) OnProjectDeleted(ctx context.Context, projectID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.republishAllLocked(ctx)
}

func (s *Service) CleanupExclusive(ctx context.Context, fn func(protected map[string]struct{}) error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	protected, err := s.protectedLocked(ctx)
	if err != nil {
		return err
	}
	return fn(protected)
}

func originalName(project sqlcdb.Project) string {
	if project.ArchPackageName != "" {
		return project.ArchPackageName
	}
	return project.DisplayName
}

func originalFromRelease(release sqlcdb.Release, project sqlcdb.Project) string {
	var body struct {
		Debian struct {
			Package string `json:"package"`
		} `json:"debian"`
	}
	if json.Unmarshal([]byte(release.BodyJson), &body) == nil && strings.TrimSpace(body.Debian.Package) != "" {
		return body.Debian.Package
	}
	if project.ArchPackageName != "" {
		return project.ArchPackageName
	}
	return originalName(project)
}

func refFromEntry(entry sqlcdb.RepoChannelEntry) PackageRef {
	return PackageRef{
		Pkgname:   entry.Pkgname,
		Arch:      entry.Arch,
		Epoch:     entry.Epoch,
		Pkgver:    entry.Pkgver,
		Pkgrel:    entry.Pkgrel,
		Version:   VersionString(entry.Epoch, entry.Pkgver, entry.Pkgrel),
		Filename:  entry.Filename,
		Artifact:  entry.ArtifactID,
		Signature: entry.SigArtifactID.String,
		ReleaseID: entry.ReleaseID.String,
	}
}

func soakStatusFrom(row sqlcdb.RepoSoak) SoakStatus {
	return SoakStatus{
		Pkgname:    row.Pkgname,
		Arch:       row.Arch,
		Pkgver:     row.Pkgver,
		Pkgrel:     row.Pkgrel,
		Version:    VersionString(row.Epoch, row.Pkgver, row.Pkgrel),
		Status:     row.Status,
		StartedAt:  row.SoakStartedAt,
		EligibleAt: row.EligibleAt,
		ArtifactID: row.ArtifactID,
		ReleaseID:  row.ReleaseID.String,
	}
}

func boolInt(value bool) int64 {
	if value {
		return 1
	}
	return 0
}

func ns(value string) sql.NullString {
	value = strings.TrimSpace(value)
	if value == "" {
		return sql.NullString{}
	}
	return sql.NullString{String: value, Valid: true}
}

func (s *Service) workPath(parts ...string) string {
	return filepath.Join(append([]string{s.WorkDir}, parts...)...)
}

func ensureDir(path string) error {
	if err := os.MkdirAll(path, 0o700); err != nil {
		return err
	}
	return os.Chmod(path, 0o700)
}
