package listen

import (
	"encoding/json"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
)

const (
	DefaultPort = 8443
	maxHosts    = 16
)

type Config struct {
	Enabled bool     `json:"enabled"`
	Port    int      `json:"port"`
	Hosts   []string `json:"hosts"`
	Bound   []string `json:"bound,omitempty"`
}

type State struct {
	mu  sync.Mutex
	cfg Config
}

func Default() Config {
	return Config{
		Enabled: false,
		Port:    DefaultPort,
		Hosts:   []string{"0.0.0.0"},
	}
}

func FromStore(enabled bool, port int, hostsJSON string) Config {
	cfg := Default()
	cfg.Enabled = enabled
	if port > 0 {
		cfg.Port = port
	}
	if strings.TrimSpace(hostsJSON) != "" {
		var hosts []string
		if json.Unmarshal([]byte(hostsJSON), &hosts) == nil && len(hosts) > 0 {
			cfg.Hosts = hosts
		}
	}
	return cfg
}

func (c Config) HostsJSON() string {
	hosts := c.Hosts
	if len(hosts) == 0 {
		hosts = Default().Hosts
	}
	raw, err := json.Marshal(hosts)
	if err != nil {
		return `["0.0.0.0"]`
	}
	return string(raw)
}

func (s *State) Snapshot() Config {
	if s == nil {
		return Default()
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return clone(s.cfg)
}

func (s *State) Set(cfg Config) {
	if s == nil {
		return
	}
	s.mu.Lock()
	s.cfg = clone(cfg)
	s.mu.Unlock()
}

func (s *State) Bound() []string {
	return s.Snapshot().Bound
}

func (s *State) Serving() bool {
	cfg := s.Snapshot()
	return cfg.Enabled && len(cfg.Bound) > 0
}

func Normalize(cfg Config) (Config, error) {
	if cfg.Port == 0 {
		cfg.Port = DefaultPort
	}
	if cfg.Port < 1 || cfg.Port > 65535 {
		return Config{}, fmt.Errorf("listen port must be between 1 and 65535")
	}
	hosts := uniqueTrimmed(cfg.Hosts)
	if len(hosts) == 0 {
		hosts = append([]string(nil), Default().Hosts...)
	}
	if len(hosts) > maxHosts {
		return Config{}, fmt.Errorf("at most %d listen addresses are allowed", maxHosts)
	}
	normalized := make([]string, 0, len(hosts))
	for _, host := range hosts {
		host, err := normalizeHost(host)
		if err != nil {
			return Config{}, err
		}
		normalized = append(normalized, host)
	}
	cfg.Hosts = uniqueTrimmed(normalized)
	cfg.Bound = nil
	return cfg, nil
}

func BindAddrs(cfg Config) ([]string, error) {
	ephemeral := cfg.Enabled && cfg.Port == 0
	if !ephemeral {
		var err error
		cfg, err = Normalize(cfg)
		if err != nil {
			return nil, err
		}
	} else if len(cfg.Hosts) == 0 {
		return nil, fmt.Errorf("listen address is required")
	} else {
		host, err := normalizeHost(cfg.Hosts[0])
		if err != nil {
			return nil, err
		}
		cfg.Hosts = []string{host}
	}
	if !cfg.Enabled {
		return nil, nil
	}
	seen := map[string]struct{}{}
	var addrs []string
	for _, host := range cfg.Hosts {
		resolved, err := resolveHost(host)
		if err != nil {
			return nil, err
		}
		for _, ip := range resolved {
			addr := net.JoinHostPort(ip, strconv.Itoa(cfg.Port))
			if _, ok := seen[addr]; ok {
				continue
			}
			seen[addr] = struct{}{}
			addrs = append(addrs, addr)
		}
	}
	if len(addrs) == 0 {
		return nil, fmt.Errorf("no addresses to listen on")
	}
	return addrs, nil
}

func ParseOverride(value string) (Config, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return Default(), nil
	}
	host, portText, err := net.SplitHostPort(value)
	if err != nil {
		return Config{}, fmt.Errorf("listen address must be host:port")
	}
	if host == "" {
		host = "0.0.0.0"
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return Config{}, fmt.Errorf("listen port is invalid")
	}
	host, err = normalizeHost(host)
	if err != nil {
		return Config{}, err
	}
	if port == 0 {
		return Config{Enabled: true, Port: 0, Hosts: []string{host}}, nil
	}
	return Normalize(Config{Enabled: true, Port: port, Hosts: []string{host}})
}

func clone(cfg Config) Config {
	cfg.Hosts = append([]string(nil), cfg.Hosts...)
	cfg.Bound = append([]string(nil), cfg.Bound...)
	return cfg
}

func SameTarget(a, b Config) bool {
	if a.Enabled != b.Enabled || a.Port != b.Port {
		return false
	}
	left := uniqueTrimmed(a.Hosts)
	right := uniqueTrimmed(b.Hosts)
	if len(left) != len(right) {
		return false
	}
	seen := make(map[string]struct{}, len(left))
	for _, host := range left {
		seen[host] = struct{}{}
	}
	for _, host := range right {
		if _, ok := seen[host]; !ok {
			return false
		}
	}
	return true
}

func uniqueTrimmed(values []string) []string {
	seen := map[string]struct{}{}
	out := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	return out
}

func normalizeHost(host string) (string, error) {
	host = strings.TrimSpace(host)
	switch strings.ToLower(host) {
	case "*", "all", "any":
		return "0.0.0.0", nil
	}
	if ip := net.ParseIP(host); ip != nil {
		return ip.String(), nil
	}
	if _, err := net.InterfaceByName(host); err == nil {
		return host, nil
	}
	if strings.Contains(host, ":") {
		return "", fmt.Errorf("listen address %q is invalid", host)
	}
	if err := validHostname(host); err != nil {
		return "", err
	}
	return host, nil
}

func validHostname(host string) error {
	if host == "localhost" {
		return nil
	}
	if len(host) == 0 || len(host) > 253 {
		return fmt.Errorf("listen address %q is invalid", host)
	}
	for _, label := range strings.Split(host, ".") {
		if label == "" || len(label) > 63 {
			return fmt.Errorf("listen address %q is invalid", host)
		}
		for i, r := range label {
			ok := r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || (r == '-' && i > 0 && i < len(label)-1)
			if !ok {
				return fmt.Errorf("listen address %q is invalid", host)
			}
		}
	}
	return nil
}

func resolveHost(host string) ([]string, error) {
	if ip := net.ParseIP(host); ip != nil {
		return []string{ip.String()}, nil
	}
	if iface, err := net.InterfaceByName(host); err == nil {
		addrs, err := iface.Addrs()
		if err != nil {
			return nil, fmt.Errorf("interface %s: %w", host, err)
		}
		var ips []string
		for _, addr := range addrs {
			ipnet, ok := addr.(*net.IPNet)
			if !ok || ipnet.IP == nil {
				continue
			}
			if ipnet.IP.IsLinkLocalUnicast() {
				continue
			}
			ips = append(ips, ipnet.IP.String())
		}
		if len(ips) == 0 {
			return nil, fmt.Errorf("interface %s has no addresses to listen on", host)
		}
		return ips, nil
	}
	resolved, err := net.LookupIP(host)
	if err != nil {
		return nil, fmt.Errorf("could not resolve %s: %w", host, err)
	}
	var ips []string
	for _, ip := range resolved {
		if ip.IsLinkLocalUnicast() {
			continue
		}
		ips = append(ips, ip.String())
	}
	if len(ips) == 0 {
		return nil, fmt.Errorf("could not resolve %s to a listen address", host)
	}
	return ips, nil
}
