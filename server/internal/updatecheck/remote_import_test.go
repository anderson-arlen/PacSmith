package updatecheck

import (
	"context"
	"io"
	"net/http"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestDefaultHTTPClientHasNoWholeTransferDeadline(t *testing.T) {
	if timeout := defaultHTTPClient().Timeout; timeout != 0 {
		t.Fatalf("whole-transfer timeout = %s, want none", timeout)
	}
}

func TestDirectImportCreatesVisibleProjectAfterFirstBytes(t *testing.T) {
	payload := archiveFixture(t)
	reader, writer := io.Pipe()
	service, libraryService, cleanup := remoteImportFixture(t, &http.Client{
		Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: http.StatusOK, Body: reader, Request: request,
				ContentLength: int64(len(payload))}, nil
		}),
	})
	defer cleanup()
	service.DownloadIdleTimeout = time.Second

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	progress := make(chan Progress, 4)
	done := make(chan error, 1)
	go func() {
		_, err := service.ImportDirectURL(ctx, DirectImportRequest{
			URL: "https://vendor.invalid/demo-2.0.tar", Version: "2.0",
		}, "visible-import-job", nil, func(update Progress) { progress <- update })
		done <- err
	}()
	if _, err := writer.Write(payload[:64]); err != nil {
		t.Fatal(err)
	}
	var update Progress
	select {
	case update = <-progress:
	case <-time.After(3 * time.Second):
		t.Fatal("pending import did not report its project")
	}
	if update.ProjectID == "" || update.ReleaseID == "" || update.Current == 0 ||
		update.Total != int64(len(payload)) {
		t.Fatalf("progress = %+v", update)
	}
	projects, err := libraryService.ListProjectSummaries(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(projects) != 1 || len(projects[0].Releases) != 1 ||
		projects[0].Releases[0].State != "preparing" {
		t.Fatalf("visible projects = %+v", projects)
	}
	cancel()
	_ = writer.CloseWithError(context.Canceled)
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("canceled import unexpectedly succeeded")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("canceled import did not stop")
	}
	release, err := libraryService.GetRelease(context.Background(), update.ReleaseID)
	if err != nil {
		t.Fatal(err)
	}
	if release.State != "discovered" {
		t.Fatalf("canceled release state = %q, want discovered", release.State)
	}
}

func TestDirectImportTimesOutOnlyWhenTransferStalls(t *testing.T) {
	reader, writer := io.Pipe()
	service, libraryService, cleanup := remoteImportFixture(t, &http.Client{
		Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: http.StatusOK, Body: reader, Request: request,
				ContentLength: 4096}, nil
		}),
	})
	defer cleanup()
	service.DownloadIdleTimeout = 30 * time.Millisecond
	go func() {
		_, _ = writer.Write([]byte{'x'})
	}()
	_, err := service.ImportDirectURL(context.Background(), DirectImportRequest{
		URL: "https://vendor.invalid/stalled.tar",
	}, "stalled-import-job", nil)
	if err == nil || !strings.Contains(err.Error(), "received no data") {
		t.Fatalf("stalled import error = %v", err)
	}
	projects, listErr := libraryService.ListProjectSummaries(context.Background())
	if listErr != nil {
		t.Fatal(listErr)
	}
	if len(projects) != 1 || projects[0].Releases[0].State != "discovered" {
		t.Fatalf("stalled import project = %+v", projects)
	}
}

func TestDirectImportAllowsTransferLongerThanIdleTimeout(t *testing.T) {
	payload := archiveFixture(t)
	service, _, cleanup := remoteImportFixture(t, &http.Client{
		Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: http.StatusOK,
				Body: io.NopCloser(&slowChunkReader{contents: payload, chunkSize: 32,
					delay: 2 * time.Millisecond}), Request: request,
				ContentLength: int64(len(payload))}, nil
		}),
	})
	defer cleanup()
	service.DownloadIdleTimeout = 20 * time.Millisecond
	started := time.Now()
	result, err := service.ImportDirectURL(context.Background(), DirectImportRequest{
		URL: "https://vendor.invalid/demo-2.0.tar",
	}, "slow-active-import-job", nil)
	if err != nil {
		t.Fatal(err)
	}
	if result.ProjectID == "" || time.Since(started) <= service.DownloadIdleTimeout {
		t.Fatalf("result = %+v, elapsed = %s", result, time.Since(started))
	}
}

type slowChunkReader struct {
	contents  []byte
	chunkSize int
	delay     time.Duration
}

func (reader *slowChunkReader) Read(buffer []byte) (int, error) {
	if len(reader.contents) == 0 {
		return 0, io.EOF
	}
	time.Sleep(reader.delay)
	count := min(len(buffer), reader.chunkSize, len(reader.contents))
	copy(buffer[:count], reader.contents[:count])
	reader.contents = reader.contents[count:]
	return count, nil
}

func remoteImportFixture(t *testing.T, client *http.Client) (*Service, *library.Service, func()) {
	t.Helper()
	root := t.TempDir()
	db, err := sqlite.Open(context.Background(), filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	libraryService := &library.Service{DB: db, Artifacts: registry,
		WorkDir: filepath.Join(root, "work")}
	service := &Service{DB: db, Library: libraryService, Artifacts: registry, Client: client}
	return service, libraryService, func() { _ = db.Close() }
}
