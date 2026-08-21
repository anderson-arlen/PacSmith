package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/anderson-arlen/pacsmith/server/internal/daemon"
	"github.com/anderson-arlen/pacsmith/server/internal/paths"
	"github.com/anderson-arlen/pacsmith/server/internal/version"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "pacsmithd: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		showVersion bool
		over        paths.Overrides
	)
	flag.BoolVar(&showVersion, "version", false, "print version and exit")
	flag.StringVar(&over.DataHome, "data-home", "", "XDG data home (default: $XDG_DATA_HOME)")
	flag.StringVar(&over.ConfigHome, "config-home", "", "XDG config home (default: $XDG_CONFIG_HOME)")
	flag.StringVar(&over.StateHome, "state-home", "", "XDG state home (default: $XDG_STATE_HOME)")
	flag.StringVar(&over.RuntimeDir, "runtime-dir", "", "XDG runtime dir (default: $XDG_RUNTIME_DIR)")
	flag.StringVar(&over.Socket, "socket", "", "Unix socket path")
	flag.Parse()
	if showVersion {
		fmt.Printf("pacsmithd %s api/%s\n", version.Version, version.API)
		return nil
	}

	dirs, err := paths.Resolve(over)
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	d, err := daemon.StartConfig(ctx, daemon.Config{Dirs: dirs})
	if err != nil {
		return err
	}
	defer d.Close()
	<-ctx.Done()
	return nil
}
