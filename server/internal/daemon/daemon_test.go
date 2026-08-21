package daemon_test

import (
	"encoding/json"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/apitest"
	"github.com/anderson-arlen/pacsmith/server/internal/legacy"
)

func TestUnixSocketAPIAndLegacyIsolation(t *testing.T) {
	dirs, client := apitest.StartDaemon(t)
	legacyRoot := legacy.ProjectsDir(dirs.DataHome)
	before := apitest.TreeFingerprint(t, legacyRoot)
	apitest.RunContract(t, client, "http://localhost")
	t.Run("server_info_local_admin", func(t *testing.T) {
		resp, err := client.Get("http://localhost/api/v1/server")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != 200 {
			t.Fatalf("status %d", resp.StatusCode)
		}
		var info struct {
			Listen struct {
				Enabled bool     `json:"enabled"`
				Bound   []string `json:"bound"`
			} `json:"listen"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&info); err != nil {
			t.Fatal(err)
		}
		if info.Listen.Enabled || len(info.Listen.Bound) != 0 {
			t.Fatalf("HTTPS listen must be off by default %+v", info.Listen)
		}
	})
	apitest.AssertLegacyUntouched(t, dirs.DataHome, before)
	if legacy.ContainsProjects(dirs.Data) || legacy.ContainsProjects(dirs.Database) ||
		legacy.ContainsProjects(dirs.Objects) || legacy.ContainsProjects(dirs.Socket) {
		t.Fatal("server paths resolved inside the legacy library")
	}
}
