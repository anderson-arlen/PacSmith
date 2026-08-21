package listen

import (
	"net"
	"testing"
)

func TestNormalizeAliasesAndRejectsBadPort(t *testing.T) {
	cfg, err := Normalize(Config{Enabled: true, Port: 9443, Hosts: []string{"*", " 127.0.0.1 "}})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Port != 9443 || len(cfg.Hosts) != 2 || cfg.Hosts[0] != "0.0.0.0" || cfg.Hosts[1] != "127.0.0.1" {
		t.Fatalf("%+v", cfg)
	}
	if _, err := Normalize(Config{Port: 70000, Hosts: []string{"127.0.0.1"}}); err == nil {
		t.Fatal("expected invalid port")
	}
}

func TestBindLoopback(t *testing.T) {
	addrs, err := BindAddrs(Config{Enabled: true, Port: 8443, Hosts: []string{"127.0.0.1"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(addrs) != 1 || addrs[0] != "127.0.0.1:8443" {
		t.Fatalf("%v", addrs)
	}
	disabled, err := BindAddrs(Config{Enabled: false, Port: 8443, Hosts: []string{"127.0.0.1"}})
	if err != nil || disabled != nil {
		t.Fatalf("%v %v", disabled, err)
	}
}

func TestParseOverride(t *testing.T) {
	cfg, err := ParseOverride("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.Enabled || cfg.Port != 0 || cfg.Hosts[0] != "127.0.0.1" {
		t.Fatalf("%+v", cfg)
	}
	if _, err := ParseOverride("not-an-address"); err == nil {
		t.Fatal("expected parse error")
	}
}

func TestFromStoreDefaults(t *testing.T) {
	cfg := FromStore(false, 0, "")
	if cfg.Enabled || cfg.Port != DefaultPort || cfg.Hosts[0] != "0.0.0.0" {
		t.Fatalf("%+v", cfg)
	}
	cfg = FromStore(true, 9443, `["127.0.0.1"]`)
	if !cfg.Enabled || cfg.Port != 9443 || cfg.Hosts[0] != "127.0.0.1" {
		t.Fatalf("%+v", cfg)
	}
}

func TestSameTargetIgnoresBoundAndHostOrder(t *testing.T) {
	left := Config{Enabled: true, Port: 8443, Hosts: []string{"tailscale0", "127.0.0.1"}, Bound: []string{"100.1.2.3:8443"}}
	right := Config{Enabled: true, Port: 8443, Hosts: []string{"127.0.0.1", "tailscale0"}}
	if !SameTarget(left, right) {
		t.Fatal("expected same listen target")
	}
	right.Port = 9443
	if SameTarget(left, right) {
		t.Fatal("different port must not match")
	}
}

func TestLookupLocalhost(t *testing.T) {
	if _, err := net.LookupIP("localhost"); err != nil {
		t.Skip("localhost does not resolve")
	}
	addrs, err := BindAddrs(Config{Enabled: true, Port: 1, Hosts: []string{"localhost"}})
	if err != nil || len(addrs) == 0 {
		t.Fatalf("%v %v", addrs, err)
	}
}
