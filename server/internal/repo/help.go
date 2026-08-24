package repo

import (
	"fmt"
	"strings"
)

func CertificationHelp(settings Settings) string {
	pacsmith := NormalizeFingerprint(settings.Fingerprint)
	root := NormalizeFingerprint(settings.RootFingerprint)
	var b strings.Builder
	b.WriteString(`Certifying the PacSmith repository key

OpenPGP certification is how one key vouches for another. It is not the same as
TLS certificates, and it is independent of PacSmith's Server CA / Client CA used
for management HTTPS.

Why a root identity can help
Direct trust is the simple default: machines trust the PacSmith-generated
signing key itself. That is enough for a single library host.

A long-lived organizational root is useful when you may replace the PacSmith
host later, or when several systems should share one human-comparable trust
anchor. Machines locally sign (trust) the root; they accept PacSmith signatures
because the root certified PacSmith's operational key.

Use an existing certification-capable OpenPGP key if that already represents
your organization. For company deployments, a dedicated organizational root is
usually better than a personal identity, so staff turnover does not become a
repository-trust problem.

The root private key can stay on a YubiKey/smart card or an offline machine.
PacSmith never asks for that private key and will not accept it.

Fingerprints
gpg --import prints a short key ID. That is not a fingerprint.
--quick-sign-key needs the full 40-hex-digit fingerprint, with no spaces.

`)
	if pacsmith == "" {
		b.WriteString("PacSmith repository key: initialize signing first.\n")
	} else {
		fmt.Fprintf(&b, "PacSmith repository key:\n  %s\n  %s\n", FormatFingerprint(pacsmith), pacsmith)
	}
	if root == "" {
		b.WriteString("Your root key: upload the root public key first.\n")
	} else {
		fmt.Fprintf(&b, "Your root key:\n  %s\n  %s\n", FormatFingerprint(root), root)
	}
	b.WriteString(`
Workflow
1. Initialize PacSmith signing if you have not already. The private key never
   leaves pacsmithd.
2. Upload your root public key.
3. Download the PacSmith public key from this same settings page.
4. Run the commands below with your root private key, offline.
5. Upload pacsmith-certified.asc. PacSmith checks that it is exactly its own
   key, fingerprint unchanged, certified by the root you uploaded.

Commands
`)
	b.WriteString(CertificationCommands(settings))
	b.WriteString(`
If the root private key is on a smart card / YubiKey, run gpg --card-status
first and touch the card when GnuPG asks. Do not copy the card's private key
onto the PacSmith host.

Never upload the root private key. Never download PacSmith's private signing
key; the management API cannot expose it.
`)
	return b.String()
}

func CertificationCommands(settings Settings) string {
	pacsmith := NormalizeFingerprint(settings.Fingerprint)
	root := NormalizeFingerprint(settings.RootFingerprint)
	if pacsmith == "" {
		return "# Initialize PacSmith signing, then reopen this help.\n"
	}
	if root == "" {
		return fmt.Sprintf(`gpg --import pacsmith.asc
gpg --quick-sign-key %s
gpg --export --armor %s > pacsmith-certified.asc
`, pacsmith, pacsmith)
	}
	return fmt.Sprintf(`gpg --import pacsmith.asc
gpg --default-key %s --quick-sign-key %s
gpg --export --armor %s > pacsmith-certified.asc
`, root, pacsmith, pacsmith)
}
