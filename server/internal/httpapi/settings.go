package httpapi

import (
	"database/sql"
	"errors"
	"net/http"
	"runtime"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type librarySettingsJSON struct {
	Revision int64              `json:"revision"`
	Updates  updateSettingsJSON `json:"updates"`
	Build    buildSettingsJSON  `json:"build"`
}

type buildSettingsJSON struct {
	Parallelism    *int `json:"parallelism,omitempty"`
	AvailableCores int  `json:"available_cores,omitempty"`
}

type updateSettingsJSON struct {
	Enabled              bool `json:"enabled"`
	Daily                bool `json:"daily"`
	Weekday              int  `json:"weekday"`
	Hour                 int  `json:"hour"`
	Minute               int  `json:"minute"`
	AutomaticallyPrepare bool `json:"automatically_prepare"`
	RetentionVersions    *int `json:"retention_versions,omitempty"`
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
	retentionVersions := int(row.RetentionVersions)
	buildParallelism := int(row.BuildParallelism)
	availableCores := min(runtime.NumCPU(), 1024)
	return librarySettingsJSON{
		Revision: row.Revision,
		Build: buildSettingsJSON{
			Parallelism:    &buildParallelism,
			AvailableCores: availableCores,
		},
		Updates: updateSettingsJSON{
			Enabled:              row.UpdatesEnabled != 0,
			Daily:                row.UpdatesDaily != 0,
			Weekday:              int(row.UpdatesWeekday),
			Hour:                 int(row.UpdatesHour),
			Minute:               int(row.UpdatesMinute),
			AutomaticallyPrepare: row.UpdatesAutoPrepare != 0,
			RetentionVersions:    &retentionVersions,
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
	retentionVersions := current.RetentionVersions
	if body.Updates.RetentionVersions != nil {
		retentionVersions = int64(*body.Updates.RetentionVersions)
	}
	buildParallelism := current.BuildParallelism
	if body.Build.Parallelism != nil {
		buildParallelism = int64(*body.Build.Parallelism)
	}
	return sqlcdb.UpdateLibrarySettingsParams{
		AiProvider:         "none",
		AiModel:            "",
		AiReasoningEffort:  "provider-default",
		AiExecutionMode:    "standard",
		AiAutoResolve:      0,
		UpdatesEnabled:     boolInt(body.Updates.Enabled),
		UpdatesDaily:       boolInt(body.Updates.Daily),
		UpdatesWeekday:     weekday,
		UpdatesHour:        int64(body.Updates.Hour),
		UpdatesMinute:      int64(body.Updates.Minute),
		UpdatesAutoPrepare: boolInt(body.Updates.AutomaticallyPrepare),
		RetentionVersions:  retentionVersions,
		BuildParallelism:   buildParallelism,
		Revision:           revision,
	}
}

func validateSettings(next sqlcdb.UpdateLibrarySettingsParams) error {
	if next.UpdatesWeekday < 1 || next.UpdatesWeekday > 7 {
		return errors.New("weekday must be 1-7")
	}
	if next.UpdatesHour < 0 || next.UpdatesHour > 23 || next.UpdatesMinute < 0 || next.UpdatesMinute > 59 {
		return errors.New("update time is invalid")
	}
	if next.RetentionVersions < -1 {
		return errors.New("retention versions must be -1 or greater")
	}
	if next.BuildParallelism < 1 || next.BuildParallelism > int64(min(runtime.NumCPU(), 1024)) {
		return errors.New("build parallelism must be between 1 and the server's available cores")
	}
	return nil
}

func boolInt(value bool) int64 {
	if value {
		return 1
	}
	return 0
}
