package daemon

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net/http"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/httpapi"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
)

func (d *Daemon) SetListen(cfg listen.Config) error {
	if d == nil {
		return fmt.Errorf("daemon is not running")
	}
	current := d.listen.Snapshot()
	if listen.SameTarget(current, cfg) {
		if !cfg.Enabled {
			d.stopTLS()
			cfg.Bound = nil
			d.listen.Set(cfg)
			return nil
		}
		if len(current.Bound) > 0 {
			cfg.Bound = append([]string(nil), current.Bound...)
			d.listen.Set(cfg)
			return nil
		}
	}
	addrs, err := listen.BindAddrs(cfg)
	if err != nil {
		return err
	}
	if !cfg.Enabled {
		d.stopTLS()
		cfg.Bound = nil
		d.listen.Set(cfg)
		return nil
	}
	tlsConfig, err := d.pki.TLSConfig()
	if err != nil {
		return err
	}
	d.stopTLS()
	var started []tlsServe
	for _, addr := range addrs {
		listener, err := tls.Listen("tcp", addr, tlsConfig)
		if err != nil {
			closeServes(started)
			return fmt.Errorf("listen %s: %w", addr, err)
		}
		started = append(started, tlsServe{
			addr:     listener.Addr().String(),
			listener: listener,
			server:   httpapi.NewHTTPServer(d.handler),
		})
	}
	d.tlsMu.Lock()
	d.tlsServes = started
	if len(started) > 0 {
		d.tlsAddr = started[0].addr
	} else {
		d.tlsAddr = ""
	}
	d.tlsMu.Unlock()
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
	d.listen.Set(cfg)
	return nil
}

func (d *Daemon) stopTLS() {
	d.tlsMu.Lock()
	serves := d.tlsServes
	d.tlsServes = nil
	d.tlsAddr = ""
	d.tlsMu.Unlock()
	shutdownServes(serves)
}

func shutdownServes(serves []tlsServe) {
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

func closeServes(serves []tlsServe) {
	for _, item := range serves {
		if item.listener != nil {
			_ = item.listener.Close()
		}
	}
}
