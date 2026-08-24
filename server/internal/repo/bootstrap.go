package repo

import (
	"context"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
)

func (s *Service) BootstrapScript(channel string) (string, error) {
	if !ValidChannel(channel) {
		return "", fmt.Errorf("%w: unknown channel", ErrInvalid)
	}
	settings, err := s.Settings(context.Background())
	if err != nil {
		return "", err
	}
	return RenderBootstrap(settings, channel), nil
}

func ClientBaseURL(settings Settings) string {
	if base := strings.TrimRight(strings.TrimSpace(settings.AdvertisedURL), "/"); base != "" {
		return base
	}
	port := settings.ListenPort
	if port <= 0 {
		port = DefaultListenPort
	}
	for _, host := range settings.ListenHosts {
		host = strings.TrimSpace(host)
		if host == "" {
			continue
		}
		switch strings.ToLower(host) {
		case "0.0.0.0", "*", "all", "any":
			continue
		}
		if ip := net.ParseIP(host); ip != nil && ip.IsUnspecified() {
			continue
		}
		return "http://" + net.JoinHostPort(host, strconv.Itoa(port))
	}
	if name, err := os.Hostname(); err == nil {
		name = strings.TrimSpace(name)
		if name != "" {
			return "http://" + net.JoinHostPort(name, strconv.Itoa(port))
		}
	}
	return "http://" + net.JoinHostPort(DefaultListenHost, strconv.Itoa(port))
}

func KeyringPackageFilename(version int64) string {
	if version <= 0 {
		return ""
	}
	return fmt.Sprintf("%s-%d-1-any.pkg.tar.zst", KeyringPackage, version)
}

func KeyringPackageURL(settings Settings) string {
	name := KeyringPackageFilename(settings.KeyringVersion)
	if name == "" {
		return ""
	}
	return ClientBaseURL(settings) + "/repo/" + ChannelStable + "/any/" + name
}

func RenderBootstrap(settings Settings, channel string) string {
	base := ClientBaseURL(settings)
	pacmanFPR := NormalizeFingerprint(settings.Fingerprint)
	rootFPR := NormalizeFingerprint(settings.RootFingerprint)
	var b strings.Builder
	b.WriteString("#!/bin/bash\n")
	b.WriteString("set -euo pipefail\n")
	b.WriteString("# PacSmith repository bootstrap. Keep repository access on a trusted private\n")
	b.WriteString("# network such as a LAN, Tailscale, or WireGuard network. Deliver this script\n")
	b.WriteString("# through a channel you already trust (configuration management, a provisioned\n")
	b.WriteString("# image, or the authenticated PacSmith management interface).\n")
	fmt.Fprintf(&b, "BASE_URL=%q\n", base)
	fmt.Fprintf(&b, "CHANNEL=%q\n", channel)
	fmt.Fprintf(&b, "EXPECTED_PACSMITH_FPR=%q\n", pacmanFPR)
	if settings.TrustMode == TrustRootCertified && rootFPR != "" {
		fmt.Fprintf(&b, "EXPECTED_ROOT_FPR=%q\n", rootFPR)
	} else {
		b.WriteString("EXPECTED_ROOT_FPR=\n")
	}
	b.WriteString(`
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "this script must run as root (for pacman-key and /etc/pacman.d)" >&2
  exit 1
fi
if [[ -z "$EXPECTED_PACSMITH_FPR" ]]; then
  echo "this script was generated before repository signing was initialized" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$BASE_URL/bootstrap/pacsmith.gpg" -o "$tmp/pacsmith.gpg"
curl -fsSL "$BASE_URL/bootstrap/pacsmith-trusted" -o "$tmp/pacsmith-trusted"
curl -fsSL "$BASE_URL/bootstrap/pacsmith-revoked" -o "$tmp/pacsmith-revoked" || : >"$tmp/pacsmith-revoked"

got=$(gpg --show-keys --with-colons "$tmp/pacsmith.gpg" | awk -F: '/^fpr:/ {print toupper($10)}')
if ! grep -qx "$EXPECTED_PACSMITH_FPR" <<<"$got"; then
  echo "downloaded PacSmith keyring fingerprint does not match EXPECTED_PACSMITH_FPR" >&2
  echo "expected $EXPECTED_PACSMITH_FPR" >&2
  echo "got:"$'\n'"$got" >&2
  exit 1
fi
if [[ -n "$EXPECTED_ROOT_FPR" ]] && ! grep -qx "$EXPECTED_ROOT_FPR" <<<"$got"; then
  echo "downloaded keyring is missing the expected root fingerprint" >&2
  exit 1
fi

install -d /usr/share/pacman/keyrings
install -m644 "$tmp/pacsmith.gpg" /usr/share/pacman/keyrings/pacsmith.gpg
install -m644 "$tmp/pacsmith-trusted" /usr/share/pacman/keyrings/pacsmith-trusted
install -m644 "$tmp/pacsmith-revoked" /usr/share/pacman/keyrings/pacsmith-revoked

pacman-key --init
pacman-key --populate pacsmith

umask 022
cat >/etc/pacman.d/pacsmith <<EOF
[pacsmith]
SigLevel = Required TrustedOnly
Server = ${BASE_URL}/repo/${CHANNEL}/\$arch
EOF

if ! grep -q '^Include *= */etc/pacman.d/pacsmith' /etc/pacman.conf; then
  printf '\n# PacSmith repository (review ordering relative to other repos)\nInclude = /etc/pacman.d/pacsmith\n' >>/etc/pacman.conf
fi

echo "PacSmith repository configured for channel ${CHANNEL}."
echo "Review /etc/pacman.conf repository order if you disabled the package-name prefix."
echo "Then run: pacman -Syu"
`)
	return b.String()
}
