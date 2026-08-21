package pki

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"encoding/pem"
	"fmt"
	"net"
	"strings"
	"testing"
)

func TestGenerateCAs(t *testing.T) {
	material := mustGenerate(t)
	serverCA := mustParseCert(t, material.ServerCACert)
	clientCA := mustParseCert(t, material.ClientCACert)
	if !serverCA.IsCA || !clientCA.IsCA {
		t.Fatal("generated certificates must be CAs")
	}
	if bytes.Equal(material.ServerCACert, material.ClientCACert) {
		t.Fatal("server and client CA certificates are identical")
	}
	if bytes.Equal(material.ServerCAKey, material.ClientCAKey) {
		t.Fatal("server and client CA keys are identical")
	}
	serverPub, ok := serverCA.PublicKey.(*ecdsa.PublicKey)
	if !ok {
		t.Fatal("server CA is not ECDSA")
	}
	clientPub, ok := clientCA.PublicKey.(*ecdsa.PublicKey)
	if !ok {
		t.Fatal("client CA is not ECDSA")
	}
	if serverPub.Equal(clientPub) {
		t.Fatal("server and client CAs reused the same key")
	}
	if serverPub.Params().BitSize != 256 || clientPub.Params().BitSize != 256 {
		t.Fatalf("expected P-256 CAs, got %d and %d", serverPub.Params().BitSize, clientPub.Params().BitSize)
	}
}

func TestServerFingerprintStableAndFormatted(t *testing.T) {
	material := mustGenerate(t)
	abbrev, full, err := ServerFingerprint(material.ServerCACert)
	if err != nil {
		t.Fatal(err)
	}
	if len(full) != 64 || strings.Trim(full, "0123456789abcdef") != "" {
		t.Fatalf("full fingerprint %q is not 64 lowercase hex chars", full)
	}
	wantAbbrev := strings.ToUpper(full[:4]) + " " + strings.ToUpper(full[4:8]) + " " +
		strings.ToUpper(full[8:12]) + " " + strings.ToUpper(full[12:16]) + " " + strings.ToUpper(full[16:20])
	if abbrev != wantAbbrev {
		t.Fatalf("abbrev %q, want %q", abbrev, wantAbbrev)
	}
	if len(strings.ReplaceAll(abbrev, " ", "")) != 20 {
		t.Fatalf("abbrev is not 80 bits: %q", abbrev)
	}
	againAbbrev, againFull, err := ServerFingerprint(material.ServerCACert)
	if err != nil {
		t.Fatal(err)
	}
	if againAbbrev != abbrev || againFull != full {
		t.Fatal("fingerprint is not stable for the same Server CA")
	}
	otherAbbrev, otherFull, err := ServerFingerprint(material.ClientCACert)
	if err != nil {
		t.Fatal(err)
	}
	if otherAbbrev == abbrev || otherFull == full {
		t.Fatal("server and client CAs produced the same fingerprint")
	}
}

func TestSignClientCSRChainsToClientCA(t *testing.T) {
	material := mustGenerate(t)
	csrPEM, _ := mustCSR(t, false)
	clientID := "11111111-2222-3333-4444-555555555555"
	certPEM, err := SignClientCSR(material.ClientCACert, material.ClientCAKey, csrPEM, clientID)
	if err != nil {
		t.Fatal(err)
	}
	cert := mustParseCert(t, certPEM)
	if cert.Subject.CommonName != clientID {
		t.Fatalf("CN %q, want %q", cert.Subject.CommonName, clientID)
	}
	clientRoots := x509.NewCertPool()
	clientRoots.AddCert(mustParseCert(t, material.ClientCACert))
	if _, err := cert.Verify(x509.VerifyOptions{
		Roots:     clientRoots,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}); err != nil {
		t.Fatalf("client cert does not chain to Client CA: %v", err)
	}
	serverRoots := x509.NewCertPool()
	serverRoots.AddCert(mustParseCert(t, material.ServerCACert))
	if _, err := cert.Verify(x509.VerifyOptions{
		Roots:     serverRoots,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}); err == nil {
		t.Fatal("client cert chained to Server CA")
	}
}

func TestLeafForHelloDNSAndIP(t *testing.T) {
	material := mustGenerate(t)
	cache := NewLeafCache()
	certPEM, keyPEM, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, "example.com", nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(keyPEM) == 0 {
		t.Fatal("missing leaf key")
	}
	cert := mustParseCert(t, certPEM)
	if len(cert.DNSNames) != 1 || cert.DNSNames[0] != "example.com" {
		t.Fatalf("DNS SAN %v", cert.DNSNames)
	}
	if len(cert.IPAddresses) != 0 {
		t.Fatalf("unexpected IP SAN %v", cert.IPAddresses)
	}
	roots := x509.NewCertPool()
	roots.AddCert(mustParseCert(t, material.ServerCACert))
	if _, err := cert.Verify(x509.VerifyOptions{
		DNSName:   "example.com",
		Roots:     roots,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}); err != nil {
		t.Fatalf("example.com leaf does not chain to Server CA: %v", err)
	}

	ip := net.ParseIP("127.0.0.1")
	ipPEM, _, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, "", ip)
	if err != nil {
		t.Fatal(err)
	}
	ipCert := mustParseCert(t, ipPEM)
	if len(ipCert.DNSNames) != 0 {
		t.Fatalf("unexpected DNS SAN on IP leaf %v", ipCert.DNSNames)
	}
	if len(ipCert.IPAddresses) != 1 || !ipCert.IPAddresses[0].Equal(ip) {
		t.Fatalf("IP SAN %v", ipCert.IPAddresses)
	}
	if _, err := ipCert.Verify(x509.VerifyOptions{
		Roots:     roots,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}); err != nil {
		t.Fatalf("127.0.0.1 leaf does not chain to Server CA: %v", err)
	}

	again, _, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, "example.com", nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, certPEM) {
		t.Fatal("cached example.com leaf was regenerated")
	}
}

func TestLeafCacheBounded(t *testing.T) {
	material := mustGenerate(t)
	cache := NewLeafCache()
	for i := 0; i < 100; i++ {
		sni := fmt.Sprintf("h%03d.example.com", i)
		if _, _, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, sni, nil); err != nil {
			t.Fatalf("sni %s: %v", sni, err)
		}
	}
	if cache.Len() > 32 {
		t.Fatalf("cache grew to %d", cache.Len())
	}
	if cache.Len() != 32 {
		t.Fatalf("cache size %d, want 32", cache.Len())
	}
}

func TestLeafForHelloRejectsEmptyAndOverlongSNI(t *testing.T) {
	material := mustGenerate(t)
	cache := NewLeafCache()
	if _, _, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, "", nil); err == nil {
		t.Fatal("empty SNI accepted")
	}
	long := strings.Repeat("a", 256)
	if _, _, err := LeafForHello(material.ServerCACert, material.ServerCAKey, cache, long, nil); err == nil {
		t.Fatal("overlong SNI accepted")
	}
}

func TestValidateCSRRejectsGarbage(t *testing.T) {
	if err := ValidateCSR(nil); err == nil {
		t.Fatal("nil CSR accepted")
	}
	if err := ValidateCSR([]byte("not a csr")); err == nil {
		t.Fatal("garbage CSR accepted")
	}
	if err := ValidateCSR([]byte("-----BEGIN CERTIFICATE REQUEST-----\nbad\n-----END CERTIFICATE REQUEST-----\n")); err == nil {
		t.Fatal("malformed CSR accepted")
	}
	csrPEM, keyPEM := mustCSR(t, false)
	combined := append(append([]byte{}, csrPEM...), keyPEM...)
	if err := ValidateCSR(combined); err == nil {
		t.Fatal("CSR with private key accepted")
	}
	extra, _ := mustCSR(t, true)
	if err := ValidateCSR(extra); err == nil {
		t.Fatal("CSR with extra extensions accepted")
	}
	smallRSA, err := rsa.GenerateKey(rand.Reader, 1024)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.CreateCertificateRequest(rand.Reader, &x509.CertificateRequest{
		Subject: pkix.Name{CommonName: "too-small"},
	}, smallRSA)
	if err != nil {
		t.Fatal(err)
	}
	smallPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE REQUEST", Bytes: der})
	if err := ValidateCSR(smallPEM); err == nil {
		t.Fatal("RSA-1024 CSR accepted")
	}
	good, _ := mustCSR(t, false)
	if err := ValidateCSR(good); err != nil {
		t.Fatalf("valid CSR rejected: %v", err)
	}
}

func mustGenerate(t *testing.T) Material {
	t.Helper()
	material, err := GenerateCAs()
	if err != nil {
		t.Fatal(err)
	}
	return material
}

func mustParseCert(t *testing.T, pemBytes []byte) *x509.Certificate {
	t.Helper()
	block, _ := pem.Decode(pemBytes)
	if block == nil {
		t.Fatal("no PEM block")
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	return cert
}

func mustCSR(t *testing.T, extra bool) (csrPEM, keyPEM []byte) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.CertificateRequest{
		Subject: pkix.Name{CommonName: "pending-client"},
	}
	if extra {
		tmpl.DNSNames = []string{"unexpected.example"}
		tmpl.ExtraExtensions = []pkix.Extension{{
			Id:    asn1.ObjectIdentifier{1, 2, 3, 4, 5},
			Value: []byte("nope"),
		}}
	}
	der, err := x509.CreateCertificateRequest(rand.Reader, tmpl, key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE REQUEST", Bytes: der}),
		pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
}
