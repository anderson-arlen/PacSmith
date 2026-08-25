package httpapi

import (
	"database/sql"
	"errors"
	"net/http"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type librarySettingsJSON struct {
	Revision int64               `json:"revision"`
	Updates  updateSettingsJSON  `json:"updates"`
	Cleanup  cleanupSettingsJSON `json:"cleanup"`
}

type updateSettingsJSON struct {
	Enabled              bool `json:"enabled"`
	Daily                bool `json:"daily"`
	Weekday              int  `json:"weekday"`
	Hour                 int  `json:"hour"`
	Minute               int  `json:"minute"`
	AutomaticallyPrepare bool `json:"automatically_prepare"`
}

type cleanupSettingsJSON struct {
	RetainedPackageVersions  int `json:"retained_package_versions"`
	RetainedCompleteReleases int `json:"retained_complete_releases"`
}

func (s *Server) getSettings(w http.ResponseWriter, r *http.Request) {
	row, err := s.DB.Queries.GetLibrarySettings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.settingsFromRow(r, row))
}

func (s *Server) patchSettings(w http.ResponseWriter, r *http.Request) {
	current, err := s.DB.Queries.GetLibrarySettings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	var body librarySettingsJSON
	if !decodeJSON(w, r, &body) {
		return
	}
	next := settingsPatch(current, body)
	if err := validateSettings(next); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	updated, err := s.DB.Queries.UpdateLibrarySettings(r.Context(), next)
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusConflict, "conflict", "revision conflict")
		return
	}
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.settingsFromRow(r, updated))
}

func (s *Server) settingsFromRow(r *http.Request, row sqlcdb.LibrarySetting) librarySettingsJSON {
	return librarySettingsJSON{
		Revision: row.Revision,
		Updates: updateSettingsJSON{
			Enabled:              row.UpdatesEnabled != 0,
			Daily:                row.UpdatesDaily != 0,
			Weekday:              int(row.UpdatesWeekday),
			Hour:                 int(row.UpdatesHour),
			Minute:               int(row.UpdatesMinute),
			AutomaticallyPrepare: row.UpdatesAutoPrepare != 0,
		},
		Cleanup: cleanupSettingsJSON{
			RetainedPackageVersions:  int(row.RetainedPackageVersions),
			RetainedCompleteReleases: int(row.RetainedCompleteReleases),
		},
	}
}

func settingsPatch(current sqlcdb.LibrarySetting, body librarySettingsJSON) sqlcdb.UpdateLibrarySettingsParams {
	revision := body.Revision
	if revision == 0 {
		revision = current.Revision
	}
	weekday := int64(body.Updates.Weekday)
	if weekday == 0 {
		weekday = current.UpdatesWeekday
	}
	return sqlcdb.UpdateLibrarySettingsParams{
		AiProvider:               "none",
		AiModel:                  "",
		AiReasoningEffort:        "provider-default",
		AiExecutionMode:          "standard",
		AiAutoResolve:            0,
		UpdatesEnabled:           boolInt(body.Updates.Enabled),
		UpdatesDaily:             boolInt(body.Updates.Daily),
		UpdatesWeekday:           weekday,
		UpdatesHour:              int64(body.Updates.Hour),
		UpdatesMinute:            int64(body.Updates.Minute),
		UpdatesAutoPrepare:       boolInt(body.Updates.AutomaticallyPrepare),
		RetainedPackageVersions:  int64(body.Cleanup.RetainedPackageVersions),
		RetainedCompleteReleases: int64(body.Cleanup.RetainedCompleteReleases),
		Revision:                 revision,
	}
}

func validateSettings(next sqlcdb.UpdateLibrarySettingsParams) error {
	if next.UpdatesWeekday < 1 || next.UpdatesWeekday > 7 {
		return errors.New("weekday must be 1-7")
	}
	if next.UpdatesHour < 0 || next.UpdatesHour > 23 || next.UpdatesMinute < 0 || next.UpdatesMinute > 59 {
		return errors.New("update time is invalid")
	}
	if next.RetainedPackageVersions < -1 || next.RetainedCompleteReleases < -1 {
		return errors.New("retention counts must be -1 or greater")
	}
	if next.RetainedPackageVersions >= 0 && next.RetainedCompleteReleases >= 0 &&
		next.RetainedCompleteReleases < next.RetainedPackageVersions {
		return errors.New("complete-release retention cannot be lower than artifact retention")
	}
	return nil
}

func boolInt(value bool) int64 {
	if value {
		return 1
	}
	return 0
}
