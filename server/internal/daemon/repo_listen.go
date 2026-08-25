package daemon

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/events"
	"github.com/anderson-arlen/pacsmith/server/internal/httpapi"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
)

type repoServe struct {
	addr     string
	listener net.Listener
	server   *http.Server
}

func (d *Daemon) SetRepoListen(cfg listen.Config) error {
	if d == nil {
		return fmt.Errorf("daemon is not running")
	}
	if cfg.Port <= 0 {
		cfg.Port = repo.DefaultListenPort
	}
	d.repoMu.Lock()
	defer d.repoMu.Unlock()
	current := d.repoListen
	if listen.SameTarget(current, cfg) {
		if !cfg.Enabled {
			d.stopRepoLocked()
			cfg.Bound = nil
			d.repoListen = cfg
			return nil
		}
		if len(current.Bound) > 0 {
			cfg.Bound = append([]string(nil), current.Bound...)
			d.repoListen = cfg
			return nil
		}
	}
	addrs, err := listen.BindAddrs(cfg)
	if err != nil {
		return err
	}
	d.stopRepoLocked()
	if !cfg.Enabled {
		cfg.Bound = nil
		d.repoListen = cfg
		return nil
	}
	var started []repoServe
	for _, addr := range addrs {
		listener, err := net.Listen("tcp", addr)
		if err != nil {
			closeRepoServes(started)
			return fmt.Errorf("repository listen %s: %w", addr, err)
		}
		started = append(started, repoServe{
			addr:     listener.Addr().String(),
			listener: listener,
			server:   httpapi.NewHTTPServer(d.repo.Handler()),
		})
	}
	d.repoServes = started
	for i := range started {
		serve := started[i]
		go func() {
			if serveErr := serve.server.Serve(serve.listener); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
				_ = d.Close()
			}
		}()
	}
	cfg.Bound = make([]string, 0, len(started))
	for _, item := range started {
		cfg.Bound = append(cfg.Bound, item.addr)
	}
	d.repoListen = cfg
	return nil
}

func (d *Daemon) RepoBound() []string {
	if d == nil {
		return nil
	}
	d.repoMu.Lock()
	defer d.repoMu.Unlock()
	return append([]string(nil), d.repoListen.Bound...)
}

func (d *Daemon) stopRepoLocked() {
	serves := d.repoServes
	d.repoServes = nil
	d.repoListen.Bound = nil
	shutdownRepoServes(serves)
}

func shutdownRepoServes(serves []repoServe) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for _, item := range serves {
		if item.server != nil {
			_ = item.server.Shutdown(ctx)
		}
		if item.listener != nil {
			_ = item.listener.Close()
		}
	}
}

func closeRepoServes(serves []repoServe) {
	for _, item := range serves {
		if item.listener != nil {
			_ = item.listener.Close()
		}
	}
}

func (d *Daemon) startRepoMaintenance() {
	if d == nil || d.repo == nil {
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	d.stopSoak = cancel
	go d.runRepoMaintenance(ctx)
}

func (d *Daemon) runRepoMaintenance(ctx context.Context) {
	d.evaluateRepoSoaks(ctx)
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			d.evaluateRepoSoaks(ctx)
		}
	}
}

func (d *Daemon) evaluateRepoSoaks(ctx context.Context) {
	changed, err := d.repo.EvaluateSoaksChanged(ctx)
	if err == nil && changed && d.events != nil {
		d.events.Publish(events.Event{Topics: []string{"repository", "projects"}})
	}
}
