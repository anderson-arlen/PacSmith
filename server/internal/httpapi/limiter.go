package httpapi

import (
	"net"
	"sync"
	"time"
)

const (
	registerLimit     = 5
	registerWindow    = time.Minute
	pendingCap        = 32
	registrationTTL   = 24 * time.Hour
	friendlyNameLimit = 80
)

type ipLimiter struct {
	mu       sync.Mutex
	attempts map[string][]time.Time
}

func newIPLimiter() *ipLimiter {
	return &ipLimiter{attempts: map[string][]time.Time{}}
}

func (l *ipLimiter) allow(ip string) bool {
	if l == nil {
		return true
	}
	now := time.Now()
	cutoff := now.Add(-registerWindow)
	l.mu.Lock()
	defer l.mu.Unlock()
	stamps := l.attempts[ip]
	kept := stamps[:0]
	for _, stamp := range stamps {
		if stamp.After(cutoff) {
			kept = append(kept, stamp)
		}
	}
	if len(kept) >= registerLimit {
		l.attempts[ip] = kept
		return false
	}
	l.attempts[ip] = append(kept, now)
	return true
}

func requestIP(remote string) string {
	host, _, err := net.SplitHostPort(remote)
	if err != nil {
		return remote
	}
	return host
}
