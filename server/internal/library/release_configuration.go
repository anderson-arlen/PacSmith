package library

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

var releaseConfigurationFields = map[string]struct{}{
	"displayName": {}, "iconPath": {}, "iconSourcePath": {}, "iconSha256": {},
	"installMapping": {}, "vendorName": {}, "archPkgrelOverride": {},
	"packageMetadata": {},
	"dependencies":    {}, "maintainerScripts": {}, "scriptFindings": {},
	"payloadRules": {}, "generatedPkgbuild": {}, "generatedPkgbuildSha256": {},
	"pkgbuildManuallyModified": {}, "lifecycleScript": {}, "fieldProvenance": {},
	"aiChanges": {}, "update": {}, "buildStatus": {}, "state": {},
	"lastBuildLog": {}, "producedPackages": {}, "history": {},
}

// PatchReleaseConfiguration keeps large inspection evidence on the daemon. The allowlist is
// the boundary between editable client state and PacSmith-owned source, payload, and artifacts.
func (s *Service) PatchReleaseConfiguration(ctx context.Context, releaseID string, revision int64,
	configuration map[string]any) (Release, error) {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, err
	}
	if revision == 0 {
		revision = row.Revision
	}
	if revision != row.Revision {
		return Release{}, ErrConflict
	}
	var document map[string]any
	if err := json.Unmarshal([]byte(row.BodyJson), &document); err != nil {
		return Release{}, err
	}
	changed := false
	for key, value := range configuration {
		if _, allowed := releaseConfigurationFields[key]; !allowed {
			return Release{}, fmt.Errorf("%w: release configuration field %q is not editable", ErrInvalid, key)
		}
		if !reflect.DeepEqual(document[key], value) {
			document[key] = value
			changed = true
		}
	}
	if !changed {
		return releaseDocument(row), nil
	}
	attachInspectedRelease(document)
	_ = s.persistSigningKeys(ctx, releaseID, document,
		inspect.InspectScripts(maintainerScriptsFromDocument(document)).SigningKeys, nil, "")
	attachIdentityVariables(row, document)
	raw, err := json.Marshal(document)
	if err != nil {
		return Release{}, err
	}
	state := row.State
	if value := stringValue(document, "state"); value != "" {
		state = value
	}
	updated, err := s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State: state, SourceType: row.SourceType, VendorVersion: row.VendorVersion,
		OriginalFilename: row.OriginalFilename, SourceSha256: row.SourceSha256,
		SourceArtifactID: row.SourceArtifactID, ArchPackageName: row.ArchPackageName,
		ArchPkgrel: row.ArchPkgrel, BodyJson: string(raw), ModifiedAt: nowUTC(),
		ID: row.ID, Revision: revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrConflict
	}
	if err != nil {
		return Release{}, err
	}
	return releaseDocument(updated), nil
}
