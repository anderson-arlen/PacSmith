package daemon

import (
	"context"
	"log"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
)

func (d *Daemon) startStorageMaintenance(ctx context.Context, lib *library.Service) {
	ctx, d.stopStorage = context.WithCancel(ctx)
	d.storageDone = make(chan struct{})
	go func() {
		defer close(d.storageDone)
		ticker := time.NewTicker(time.Hour)
		defer ticker.Stop()
		for {
			if err := lib.CollectStorage(ctx); err != nil && ctx.Err() == nil {
				log.Printf("Library storage cleanup: %v", err)
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
}
