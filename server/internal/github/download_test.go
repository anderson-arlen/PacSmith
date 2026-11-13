package github

import (
	"context"
	"io"
	"net/http"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestDownloadHandlesArchiveAndAssetEndpoints(t *testing.T) {
	for _, tc := range []struct {
		name, url, accept string
		redirect          bool
	}{
		{"source tar", "https://api.github.com/repos/vendor/tool/tarball/v2.0.0", "application/vnd.github+json", true},
		{"source zip", "https://api.github.com/repos/vendor/tool/zipball/v2.0.0", "application/vnd.github+json", true},
		{"default branch", "https://api.github.com/repos/vendor/tool/tarball", "application/vnd.github+json", true},
		{"asset API redirect", "https://api.github.com/repos/vendor/tool/releases/assets/201", "application/octet-stream", true},
		{"asset API stream", "https://api.github.com/repos/vendor/tool/releases/assets/201", "application/octet-stream", false},
		{"browser asset", "https://github.com/vendor/tool/releases/download/v2.0.0/tool.tar.gz", "application/octet-stream", false},
		{"repository named tarball", "https://api.github.com/repos/vendor/tarball/releases/assets/201", "application/octet-stream", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx := context.Background()
			dir := t.TempDir()
			db, err := sqlite.Open(ctx, filepath.Join(dir, "library.db"))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = db.Close() })
			store, err := artifact.New(filepath.Join(dir, "objects"), filepath.Join(dir, "tmp"))
			if err != nil {
				t.Fatal(err)
			}
			const payload = "downloaded package contents"
			const redirectedURL = "https://codeload.github.com/vendor/tool/legacy.tar.gz/v2.0.0"
			requests := 0
			service := Service{
				Artifacts: &artifact.Registry{DB: db, Store: store},
				Client: &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
					requests++
					response := &http.Response{
						StatusCode: http.StatusOK, Header: make(http.Header),
						Body: io.NopCloser(strings.NewReader(payload)), Request: request,
					}
					if request.URL.String() == tc.url {
						if request.Header.Get("Accept") != tc.accept {
							response.StatusCode = http.StatusUnsupportedMediaType
						} else if tc.redirect {
							response.StatusCode = http.StatusFound
							response.Header.Set("Location", redirectedURL)
						}
					} else if !tc.redirect || request.URL.String() != redirectedURL {
						t.Fatalf("unexpected download URL: %s", request.URL)
					}
					return response, nil
				})},
			}
			record, err := service.Download(ctx, Source{DownloadURL: tc.url, Filename: "tool.tar.gz"})
			if err != nil {
				t.Fatal(err)
			}
			wantRequests := 1
			if tc.redirect {
				wantRequests++
			}
			if requests != wantRequests {
				t.Fatalf("requests = %d, want %d", requests, wantRequests)
			}
			stored, file, err := service.Artifacts.Open(ctx, record.ID)
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			contents, err := io.ReadAll(file)
			if err != nil {
				t.Fatal(err)
			}
			if string(contents) != payload || stored.OriginalFilename != "tool.tar.gz" {
				t.Fatalf("downloaded artifact = %+v, contents = %q", stored, contents)
			}
		})
	}
}
