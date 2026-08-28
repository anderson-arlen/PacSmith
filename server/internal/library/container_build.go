package library

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const defaultBuildImage = "docker.io/library/archlinux:base-devel"

var containerNameCleaner = regexp.MustCompile(`[^a-zA-Z0-9_.-]+`)

type buildExecution struct {
	ReleaseID          string
	ProjectID          string
	WorkDir            string
	Parallelism        int
	CompileCachePolicy string
	LogOutput          func(string)
}

func customBuild(document map[string]any) bool {
	return boolValue(document, "pkgbuildManuallyModified")
}

func runNativeBuild(ctx context.Context, execution buildExecution) (string, error) {
	arguments := []string{"--force", "--nodeps"}
	arguments = append(arguments, buildParallelismArguments(execution.Parallelism)...)
	cmd := exec.CommandContext(ctx, "/usr/bin/makepkg", arguments...)
	cmd.Dir = execution.WorkDir
	configureBuildProcess(cmd)
	return runBuildCommand(cmd, execution.LogOutput)
}

func runContainerBuild(ctx context.Context, execution buildExecution) (string, error) {
	if os.Geteuid() == 0 {
		return "", fmt.Errorf("custom PKGBUILDs require pacsmithd to run as a non-root user")
	}
	podman := strings.TrimSpace(os.Getenv("PACSMITH_PODMAN"))
	if podman == "" {
		var err error
		podman, err = exec.LookPath("podman")
		if err != nil {
			return "", fmt.Errorf("custom PKGBUILDs require rootless Podman: %w", err)
		}
	}
	image := strings.TrimSpace(os.Getenv("PACSMITH_BUILD_IMAGE"))
	if image == "" {
		image = defaultBuildImage
	}
	cacheRoot := filepath.Join(filepath.Dir(filepath.Dir(execution.WorkDir)), "cache")
	ccacheDir := projectCompileCacheDir(execution.WorkDir, execution.ProjectID)
	sourceCache := filepath.Join(cacheRoot, "sources", execution.ProjectID)
	if execution.CompileCachePolicy == "disabled" {
		ccacheDir = filepath.Join(execution.WorkDir, ".ccache")
	}
	pacmanCache := filepath.Join(cacheRoot, "pacman")
	for _, path := range []string{ccacheDir, sourceCache, pacmanCache} {
		if err := os.MkdirAll(path, 0o700); err != nil {
			return "", err
		}
		if err := os.Chmod(path, 0o700); err != nil {
			return "", err
		}
	}
	containerName := "pacsmith-build-" + containerNameCleaner.ReplaceAllString(execution.ReleaseID, "-")
	arguments := podmanBuildArguments(
		containerName, image, execution, ccacheDir, sourceCache, pacmanCache)
	cmd := exec.CommandContext(ctx, podman, arguments...)
	configureBuildProcess(cmd)
	logText, runErr := runBuildCommand(cmd, execution.LogOutput)

	cleanupCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	cleanup := exec.CommandContext(cleanupCtx, podman, "rm", "--force", containerName)
	if output, err := cleanup.CombinedOutput(); err != nil &&
		!bytes.Contains(output, []byte("no container with name or ID")) &&
		!bytes.Contains(output, []byte("no such container")) {
		logText += fmt.Sprintf("\n[PacSmith] container cleanup: %v: %s", err, output)
	}
	return logText, runErr
}

func projectCompileCacheDir(workDir, projectID string) string {
	cacheRoot := filepath.Join(filepath.Dir(filepath.Dir(workDir)), "cache")
	return filepath.Join(cacheRoot, "ccache", projectID)
}

func podmanBuildArguments(containerName, image string, execution buildExecution,
	ccacheDir, sourceCache, pacmanCache string) []string {
	parallelism := execution.Parallelism
	if parallelism < 1 {
		parallelism = 1
	}
	return []string{
		"run",
		"--name", containerName,
		"--pull=missing",
		"--cpus", strconv.Itoa(parallelism),
		"--pids-limit", "4096",
		"--security-opt", "no-new-privileges",
		"--mount", "type=bind,src=" + execution.WorkDir + ",dst=/build,rw",
		"--mount", "type=bind,src=" + ccacheDir + ",dst=/cache/ccache,rw",
		"--mount", "type=bind,src=" + sourceCache + ",dst=/cache/sources,rw",
		"--mount", "type=bind,src=" + pacmanCache + ",dst=/var/cache/pacman/pkg,rw",
		image,
		"/bin/bash", "-lc", containerBuildScript(parallelism),
	}
}

func containerBuildScript(parallelism int) string {
	return fmt.Sprintf(`set -euo pipefail
useradd --create-home --uid 1000 builder
trap 'chown -R 0:0 /build /cache/ccache /cache/sources /var/cache/pacman/pkg 2>/dev/null || true' EXIT
chown -R builder:builder /build /cache/ccache /cache/sources
runuser -u builder -- bash -lc 'cd /build && makepkg --printsrcinfo > .SRCINFO'
mapfile -t dependencies < <(
  awk -F ' = ' '/^[[:space:]]*(make|check)?depends(_[^ ]+)? = / { value=$2; sub(/[<>=].*$/, "", value); print value }' /build/.SRCINFO | sort -u
)
pacman -Syu --needed --noconfirm ccache "${dependencies[@]}"
runuser -u builder -- env \
  PATH=/usr/lib/ccache/bin:/usr/local/sbin:/usr/local/bin:/usr/bin \
  CCACHE_DIR=/cache/ccache \
  CCACHE_BASEDIR=/build \
  CCACHE_NOHASHDIR=true \
  CCACHE_COMPILERCHECK=content \
  SRCDEST=/cache/sources \
  MAKEFLAGS=-j%d \
  CMAKE_BUILD_PARALLEL_LEVEL=%d \
  bash -lc 'cd /build && makepkg --force --noconfirm'
`, parallelism, parallelism)
}

func configureBuildProcess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		if err := syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM); err != nil {
			if errors.Is(err, syscall.ESRCH) {
				return os.ErrProcessDone
			}
			return err
		}
		return nil
	}
	cmd.WaitDelay = 5 * time.Second
}

func runBuildCommand(cmd *exec.Cmd, logOutput func(string)) (string, error) {
	var output bytes.Buffer
	stream := io.Writer(&output)
	if logOutput != nil {
		stream = io.MultiWriter(&output, buildLogWriter(logOutput))
	}
	cmd.Stdout = stream
	cmd.Stderr = stream
	err := cmd.Run()
	logText := output.String()
	if err != nil {
		logText += "\n" + err.Error()
	}
	return logText, err
}
