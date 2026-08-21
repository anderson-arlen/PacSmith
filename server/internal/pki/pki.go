package pki

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/pem"
	"fmt"
	"math/big"
	"strings"
	"time"
)

const (
	caValidity     = 10 * 365 * 24 * time.Hour
	clientValidity = 2 * 365 * 24 * time.Hour
	clockSkew      = 5 * time.Minute
	abbrevBits     = 80
	abbrevHexLen   = abbrevBits / 4
)

// Material is the two-CA PKI created on first genuine server init.
// Certificates and keys are PEM-encoded. The CAs MUST NOT share a key.
type Material struct {
	ServerCACert []byte
	ServerCAKey  []byte
	ClientCACert []byte
	ClientCAKey  []byte
}

// GenerateCAs creates a Server CA and a Client CA with separate ECDSA P-256 keys.
func GenerateCAs() (Material, error) {
	serverCert, serverKey, err := generateCA("PacSmith Server CA")
	if err != nil {
		return Material{}, fmt.Errorf("server CA: %w", err)
	}
	clientCert, clientKey, err := generateCA("PacSmith Client CA")
	if err != nil {
		return Material{}, fmt.Errorf("client CA: %w", err)
	}
	return Material{
		ServerCACert: serverCert,
		ServerCAKey:  serverKey,
		ClientCACert: clientCert,
		ClientCAKey:  clientKey,
	}, nil
}

func generateCA(commonName string) (certPEM, keyPEM []byte, err error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	serial, err := randomSerial()
	if err != nil {
		return nil, nil, err
	}
	now := time.Now()
	template := &x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			Organization: []string{"PacSmith"},
			CommonName:   commonName,
		},
		NotBefore:             now.Add(-clockSkew),
		NotAfter:              now.Add(caValidity),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
		BasicConstraintsValid: true,
		IsCA:                  true,
		MaxPathLenZero:        true,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return nil, nil, err
	}
	certPEM, err = encodePEM("CERTIFICATE", der)
	if err != nil {
		return nil, nil, err
	}
	keyPEM, err = encodePrivateKey(key)
	if err != nil {
		return nil, nil, err
	}
	return certPEM, keyPEM, nil
}

// ServerFingerprint is SHA-256 of the Server CA SubjectPublicKeyInfo.
// Abbrev is the first 80 bits, displayed as five groups of four uppercase hex characters.
func ServerFingerprint(serverCACert []byte) (abbrev string, fullSHA256Hex string, err error) {
	cert, err := parseCertificate(serverCACert)
	if err != nil {
		return "", "", err
	}
	sum := sha256.Sum256(cert.RawSubjectPublicKeyInfo)
	fullSHA256Hex = hex.EncodeToString(sum[:])
	head := strings.ToUpper(fullSHA256Hex[:abbrevHexLen])
	parts := make([]string, 0, abbrevHexLen/4)
	for i := 0; i < len(head); i += 4 {
		parts = append(parts, head[i:i+4])
	}
	return strings.Join(parts, " "), fullSHA256Hex, nil
}

// ValidateCSR parses a PEM CSR, rejects extra extensions, requires ECDSA
// (P-256 or stronger) or RSA >= 2048, and rejects any embedded private key.
func ValidateCSR(csrPEM []byte) error {
	_, err := parseCSR(csrPEM)
	return err
}

func parseCSR(csrPEM []byte) (*x509.CertificateRequest, error) {
	der, err := decodeCSRPEM(csrPEM)
	if err != nil {
		return nil, err
	}
	csr, err := x509.ParseCertificateRequest(der)
	if err != nil {
		return nil, fmt.Errorf("parse CSR: %w", err)
	}
	if err := csr.CheckSignature(); err != nil {
		return nil, fmt.Errorf("CSR signature: %w", err)
	}
	if len(csr.Extensions) > 0 || len(csr.ExtraExtensions) > 0 {
		return nil, fmt.Errorf("CSR contains extra extensions")
	}
	if len(csr.DNSNames) > 0 || len(csr.EmailAddresses) > 0 || len(csr.IPAddresses) > 0 || len(csr.URIs) > 0 {
		return nil, fmt.Errorf("CSR contains extra extensions")
	}
	switch key := csr.PublicKey.(type) {
	case *ecdsa.PublicKey:
		if key.Params() == nil || key.Params().BitSize < 256 {
			return nil, fmt.Errorf("ECDSA curve too small")
		}
	case *rsa.PublicKey:
		if key.N.BitLen() < 2048 {
			return nil, fmt.Errorf("RSA key is smaller than 2048 bits")
		}
	default:
		return nil, fmt.Errorf("CSR public key must be ECDSA or RSA")
	}
	return csr, nil
}

// SignClientCSR issues a client certificate from an approved CSR, signed only
// by the Client CA.
func SignClientCSR(clientCACert, clientCAKey, csrPEM []byte, clientID string) ([]byte, error) {
	if strings.TrimSpace(clientID) == "" {
		return nil, fmt.Errorf("client ID is required")
	}
	csr, err := parseCSR(csrPEM)
	if err != nil {
		return nil, err
	}
	caCert, caKey, err := parseCA(clientCACert, clientCAKey)
	if err != nil {
		return nil, err
	}
	serial, err := randomSerial()
	if err != nil {
		return nil, err
	}
	now := time.Now()
	template := &x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			Organization: []string{"PacSmith"},
			CommonName:   clientID,
		},
		NotBefore:             now.Add(-clockSkew),
		NotAfter:              now.Add(clientValidity),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, caCert, csr.PublicKey, caKey)
	if err != nil {
		return nil, fmt.Errorf("sign client CSR: %w", err)
	}
	return encodePEM("CERTIFICATE", der)
}

func parseCA(certPEM, keyPEM []byte) (*x509.Certificate, crypto.Signer, error) {
	cert, err := parseCertificate(certPEM)
	if err != nil {
		return nil, nil, err
	}
	if !cert.IsCA {
		return nil, nil, fmt.Errorf("certificate is not a CA")
	}
	key, err := parsePrivateKey(keyPEM)
	if err != nil {
		return nil, nil, err
	}
	if !publicKeysEqual(cert.PublicKey, key.Public()) {
		return nil, nil, fmt.Errorf("CA certificate and key do not match")
	}
	return cert, key, nil
}

func publicKeysEqual(a, b crypto.PublicKey) bool {
	eq, ok := a.(interface{ Equal(crypto.PublicKey) bool })
	return ok && eq.Equal(b)
}

func randomSerial() (*big.Int, error) {
	limit := new(big.Int).Lsh(big.NewInt(1), 128)
	for {
		serial, err := rand.Int(rand.Reader, limit)
		if err != nil {
			return nil, err
		}
		if serial.Sign() > 0 {
			return serial, nil
		}
	}
}

func parseCertificate(pemBytes []byte) (*x509.Certificate, error) {
	block, err := decodeSinglePEM(pemBytes, "CERTIFICATE")
	if err != nil {
		return nil, err
	}
	cert, err := x509.ParseCertificate(block)
	if err != nil {
		return nil, fmt.Errorf("parse certificate: %w", err)
	}
	return cert, nil
}

func parsePrivateKey(pemBytes []byte) (crypto.Signer, error) {
	der, typ, err := decodePrivateKeyPEM(pemBytes)
	if err != nil {
		return nil, err
	}
	var parsed any
	switch typ {
	case "EC PRIVATE KEY":
		parsed, err = x509.ParseECPrivateKey(der)
	default:
		parsed, err = x509.ParsePKCS8PrivateKey(der)
		if err != nil {
			if key, ecErr := x509.ParseECPrivateKey(der); ecErr == nil {
				parsed, err = key, nil
			}
		}
	}
	if err != nil {
		return nil, fmt.Errorf("parse private key: %w", err)
	}
	signer, ok := parsed.(crypto.Signer)
	if !ok {
		return nil, fmt.Errorf("private key cannot sign")
	}
	return signer, nil
}

func encodePrivateKey(key *ecdsa.PrivateKey) ([]byte, error) {
	der, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return nil, err
	}
	return encodePEM("PRIVATE KEY", der)
}

func encodePEM(typ string, der []byte) ([]byte, error) {
	return pem.EncodeToMemory(&pem.Block{Type: typ, Bytes: der}), nil
}

func decodeCSRPEM(in []byte) ([]byte, error) {
	rest := in
	var der []byte
	for {
		block, next := pem.Decode(rest)
		if block == nil {
			break
		}
		rest = next
		if strings.Contains(block.Type, "PRIVATE KEY") {
			return nil, fmt.Errorf("CSR contains a private key")
		}
		switch block.Type {
		case "CERTIFICATE REQUEST", "NEW CERTIFICATE REQUEST":
			if der != nil {
				return nil, fmt.Errorf("multiple CSRs")
			}
			der = block.Bytes
		default:
			return nil, fmt.Errorf("unexpected PEM block %q", block.Type)
		}
	}
	if der == nil {
		return nil, fmt.Errorf("no CSR PEM block")
	}
	return der, nil
}

func decodeSinglePEM(in []byte, wantType string) ([]byte, error) {
	block, _ := pem.Decode(in)
	if block == nil {
		return nil, fmt.Errorf("no PEM block")
	}
	if block.Type != wantType {
		return nil, fmt.Errorf("expected %s PEM, got %s", wantType, block.Type)
	}
	return block.Bytes, nil
}

func decodePrivateKeyPEM(in []byte) ([]byte, string, error) {
	block, _ := pem.Decode(in)
	if block == nil {
		return nil, "", fmt.Errorf("no PEM block")
	}
	if !strings.Contains(block.Type, "PRIVATE KEY") {
		return nil, "", fmt.Errorf("expected private key PEM, got %s", block.Type)
	}
	return block.Bytes, block.Type, nil
}
