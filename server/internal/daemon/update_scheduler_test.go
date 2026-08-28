package daemon

import (
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/updatecheck"
)

func TestScheduledOccurrence(t *testing.T) {
	location := time.FixedZone("test", -7*60*60)
	now := time.Date(2026, time.August, 28, 14, 30, 0, 0, location)
	daily := scheduledOccurrence(now, true, 1, 2, 0)
	if daily.Day() != 28 || daily.Hour() != 2 {
		t.Fatalf("daily occurrence %s", daily)
	}
	weekly := scheduledOccurrence(now, false, 3, 9, 15)
	if weekly.Day() != 26 || weekly.Hour() != 9 || weekly.Minute() != 15 {
		t.Fatalf("weekly occurrence %s", weekly)
	}
}

func TestUpdateBatchSummaryNamesFailuresAndPausedBuilds(t *testing.T) {
	result := updatecheck.BatchResult{Failed: 1, Checks: []updatecheck.Result{
		{ProjectName: "Slack", Status: "error", Message: "repository signing key is missing"},
		{ProjectName: "Signal", Status: "update", UpdateAvailable: true, Prepared: true,
			AutomaticStatus: "paused", AutomaticMessage: "vendor lifecycle scripts changed"},
		{ProjectName: "Brave", Status: "update", UpdateAvailable: true, Built: true,
			AutomaticStatus: "built"},
	}}
	summary := updateBatchSummary(result)
	for _, expected := range []string{"2 update(s) found", "1 built automatically",
		"Slack — repository signing key is missing",
		"Signal — vendor lifecycle scripts changed"} {
		if !strings.Contains(summary, expected) {
			t.Fatalf("summary %q does not contain %q", summary, expected)
		}
	}
	if updatePausedCount(result) != 1 {
		t.Fatalf("paused count = %d, want 1", updatePausedCount(result))
	}
}
