package updatecheck

import (
	"archive/tar"
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestRepositoryKeyInspectionRunsOnServer(t *testing.T) {
	contents, err := os.ReadFile("../pgp/testdata/testkey.asc")
	if err != nil {
		t.Fatal(err)
	}
	client := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(bytes.NewReader(contents)),
			Request: request, ContentLength: int64(len(contents))}, nil
	})}
	service := &Service{Client: client}
	inspection, err := service.InspectRepositoryKey(context.Background(), "https://vendor.invalid/key.asc")
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := base64.StdEncoding.DecodeString(inspection.Contents)
	if err != nil || len(decoded) == 0 || inspection.SHA256 == "" || len(inspection.Fingerprints) == 0 {
		t.Fatalf("inspection = %+v, decode error = %v", inspection, err)
	}
	if inspection.RequestedURL != "https://vendor.invalid/key.asc" || inspection.ResolvedURL != inspection.RequestedURL {
		t.Fatalf("inspection URLs = %q -> %q", inspection.RequestedURL, inspection.ResolvedURL)
	}
}

func TestDirectImportAndUpdateCheckStayServerSide(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	libraryService := &library.Service{DB: db, Artifacts: registry, WorkDir: filepath.Join(root, "work")}
	payload := archiveFixture(t)
	requests := 0
	client := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		requests++
		if request.Method == http.MethodGet {
			return &http.Response{StatusCode: http.StatusOK, Header: http.Header{
				"Content-Length": []string{jsonNumber(len(payload))},
			}, Body: io.NopCloser(bytes.NewReader(payload)), Request: request,
				ContentLength: int64(len(payload))}, nil
		}
		if request.Method == http.MethodHead {
			if request.Header.Get("If-None-Match") != "\"stable\"" {
				t.Fatalf("conditional ETag = %q", request.Header.Get("If-None-Match"))
			}
			return &http.Response{StatusCode: http.StatusNotModified,
				Header: http.Header{"Etag": []string{"\"stable\""}},
				Body:   io.NopCloser(strings.NewReader("")), Request: request}, nil
		}
		t.Fatalf("unexpected method %s", request.Method)
		return nil, nil
	})}
	service := &Service{DB: db, Library: libraryService, Artifacts: registry, Client: client}
	imported, err := service.ImportDirectURL(ctx, DirectImportRequest{
		URL: "https://vendor.invalid/demo-2.0.tar?download=1",
	}, "direct-import-job", nil)
	if err != nil {
		t.Fatal(err)
	}
	if !imported.ProjectCreated || imported.ProjectID == "" || imported.ReleaseID == "" {
		t.Fatalf("imported = %+v", imported)
	}
	release, err := libraryService.GetRelease(ctx, imported.ReleaseID)
	if err != nil {
		t.Fatal(err)
	}
	if err := service.markPreparedReleaseState(ctx, release.ID); err != nil {
		t.Fatal(err)
	}
	release, err = libraryService.GetRelease(ctx, imported.ReleaseID)
	if err != nil {
		t.Fatal(err)
	}
	if release.State != "ready" {
		t.Fatalf("review-free prepared release state = %q, want ready", release.State)
	}
	update := object(release.Document["update"])
	update["strategy"] = StrategyDirect
	update["url"] = "https://vendor.invalid/demo-2.0.tar?download=1"
	update["directUrlEtag"] = "\"stable\""
	if _, err := libraryService.PatchReleaseConfiguration(ctx, release.ID, release.Revision,
		map[string]any{"update": update}); err != nil {
		t.Fatal(err)
	}
	result, err := service.Run(ctx, release.ID, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(result.Checks) != 1 || result.Checks[0].Status != "no-update" || requests != 2 {
		t.Fatalf("result = %+v, requests = %d", result, requests)
	}
	if result.Checks[0].ProjectName == "" || result.Checks[0].PackageName == "" {
		t.Fatalf("result omitted package identity: %+v", result.Checks[0])
	}
	source, err := db.Queries.GetUpdateSourceByRelease(ctx, release.ID)
	if err != nil {
		t.Fatal(err)
	}
	state, err := db.Queries.GetUpdateCheckState(ctx, source.ID)
	if err != nil {
		t.Fatal(err)
	}
	if source.Strategy != StrategyDirect || state.Etag != "\"stable\"" || state.LastCheckedAt == "" {
		t.Fatalf("source = %+v, state = %+v", source, state)
	}
	project, err := libraryService.GetProject(ctx, imported.ProjectID)
	if err != nil {
		t.Fatal(err)
	}
	if len(project.History) != 3 {
		t.Fatalf("project history = %#v", project.History)
	}
	if project.History[0].Event != "import-started" ||
		project.History[1].Event != "release-imported" ||
		project.History[2].Event != "update-check" {
		t.Fatalf("project history events = %#v", project.History)
	}
	if !strings.Contains(project.History[2].Detail, "no-update") ||
		!strings.Contains(project.History[2].Detail, result.Checks[0].Message) {
		t.Fatalf("update-check history did not record outcome: %#v", project.History[2])
	}
}

func archiveFixture(t *testing.T) []byte {
	t.Helper()
	var buffer bytes.Buffer
	writer := tar.NewWriter(&buffer)
	contents := []byte("#!/bin/sh\necho demo\n")
	if err := writer.WriteHeader(&tar.Header{Name: "demo-2.0/demo", Mode: 0o755,
		Size: int64(len(contents))}); err != nil {
		t.Fatal(err)
	}
	if _, err := writer.Write(contents); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return buffer.Bytes()
}

func jsonNumber(value int) string {
	raw, _ := json.Marshal(value)
	return string(raw)
}
