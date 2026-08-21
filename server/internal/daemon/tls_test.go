package daemon_test

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/apitest"
	"github.com/anderson-arlen/pacsmith/server/internal/daemon"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
)

func TestHTTPSMTLSContract(t *testing.T) {
	_, unixClient, addr := apitest.StartDaemonTLS(t)
	origin := "https://" + addr

	infoResp, err := unixClient.Get("http://localhost/api/v1/server")
	if err != nil {
		t.Fatal(err)
	}
	defer infoResp.Body.Close()
	var info struct {
		Fingerprint string `json:"fingerprint"`
		ServerCAPEM string `json:"server_ca_pem"`
	}
	if err := json.NewDecoder(infoResp.Body).Decode(&info); err != nil {
		t.Fatal(err)
	}
	if info.Fingerprint == "" || info.ServerCAPEM == "" {
		t.Fatalf("server info %+v", info)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM([]byte(info.ServerCAPEM)) {
		t.Fatal("server CA PEM")
	}

	enroll := &http.Client{
		Timeout: 15 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: &tls.Config{
				MinVersion:         tls.VersionTLS13,
				InsecureSkipVerify: true, // enrollment bootstrap only
			},
		},
	}
	version := mustHTTPS(t, enroll, origin+"/api/v1/version")
	defer version.Body.Close()
	if version.StatusCode != http.StatusOK {
		t.Fatalf("enrollment version %d", version.StatusCode)
	}
	forbidden := mustHTTPS(t, enroll, origin+"/api/v1/projects")
	defer forbidden.Body.Close()
	if forbidden.StatusCode != http.StatusForbidden {
		t.Fatalf("projects without cert %d", forbidden.StatusCode)
	}

	csrPEM, keyPEM := mustClientCSR(t)
	registerBody, _ := json.Marshal(map[string]string{"name": "test-client", "csr": string(csrPEM)})
	regReq, err := http.NewRequest(http.MethodPost, origin+"/api/v1/registrations",
		bytes.NewReader(registerBody))
	if err != nil {
		t.Fatal(err)
	}
	regReq.Header.Set("Content-Type", "application/json")
	regResp, err := enroll.Do(regReq)
	if err != nil {
		t.Fatal(err)
	}
	defer regResp.Body.Close()
	if regResp.StatusCode != http.StatusAccepted {
		t.Fatalf("register %d", regResp.StatusCode)
	}
	var pending struct {
		ID string `json:"id"`
	}
	if err := json.NewDecoder(regResp.Body).Decode(&pending); err != nil {
		t.Fatal(err)
	}

	approve, err := unixClient.Post("http://localhost/api/v1/registrations/"+pending.ID+"/approve",
		"application/json", http.NoBody)
	if err != nil {
		t.Fatal(err)
	}
	defer approve.Body.Close()
	if approve.StatusCode != http.StatusOK {
		t.Fatalf("approve %d", approve.StatusCode)
	}
	var approved struct {
		Status  string `json:"status"`
		CertPEM string `json:"cert_pem"`
	}
	if err := json.NewDecoder(approve.Body).Decode(&approved); err != nil {
		t.Fatal(err)
	}
	if approved.Status != "approved" || approved.CertPEM == "" {
		t.Fatalf("approved %+v", approved)
	}

	clientCert, err := tls.X509KeyPair([]byte(approved.CertPEM), keyPEM)
	if err != nil {
		t.Fatal(err)
	}
	mtls := &http.Client{
		Timeout: 15 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: &tls.Config{
				MinVersion:   tls.VersionTLS13,
				RootCAs:      pool,
				Certificates: []tls.Certificate{clientCert},
			},
		},
	}
	apitest.RunContract(t, mtls, origin)

	admin := mustHTTPS(t, mtls, origin+"/api/v1/server")
	defer admin.Body.Close()
	if admin.StatusCode != http.StatusForbidden {
		t.Fatalf("remote client must not administer PKI: %d", admin.StatusCode)
	}

	abbrev, _, err := pki.ServerFingerprint([]byte(info.ServerCAPEM))
	if err != nil {
		t.Fatal(err)
	}
	if abbrev != info.Fingerprint {
		t.Fatalf("fingerprint %q vs %q", info.Fingerprint, abbrev)
	}
}

func TestListenSettingStartsStopsAndPersists(t *testing.T) {
	dirs, unixClient, d := apitest.StartConfigured(t, "")
	if d.TLSAddr() != "" {
		t.Fatal("TLS must not listen by default")
	}

	holder, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := holder.Addr().(*net.TCPAddr).Port
	_ = holder.Close()

	payload, _ := json.Marshal(map[string]any{
		"listen": map[string]any{
			"enabled": true,
			"port":    port,
			"hosts":   []string{"127.0.0.1"},
		},
	})
	req, err := http.NewRequest(http.MethodPatch, "http://localhost/api/v1/server", bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := unixClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("enable listen %d", resp.StatusCode)
	}
	var enabled struct {
		Listen struct {
			Enabled bool     `json:"enabled"`
			Bound   []string `json:"bound"`
		} `json:"listen"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&enabled); err != nil {
		t.Fatal(err)
	}
	if !enabled.Listen.Enabled || len(enabled.Listen.Bound) == 0 {
		t.Fatalf("listen %+v", enabled.Listen)
	}
	againReq, err := http.NewRequest(http.MethodPatch, "http://localhost/api/v1/server", bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	againReq.Header.Set("Content-Type", "application/json")
	againEnable, err := unixClient.Do(againReq)
	if err != nil {
		t.Fatal(err)
	}
	defer againEnable.Body.Close()
	if againEnable.StatusCode != http.StatusOK {
		t.Fatalf("reapply listen %d", againEnable.StatusCode)
	}
	origin := "https://" + enabled.Listen.Bound[0]
	probe := mustHTTPS(t, &http.Client{
		Timeout: 5 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13},
		},
	}, origin+"/api/v1/version")
	defer probe.Body.Close()
	if probe.StatusCode != http.StatusOK {
		t.Fatalf("tls version %d", probe.StatusCode)
	}

	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	again, err := daemon.StartConfig(context.Background(), daemon.Config{Dirs: dirs})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = again.Close() })
	if again.TLSAddr() == "" {
		t.Fatal("listen setting did not survive restart")
	}

	off, _ := json.Marshal(map[string]any{"listen": map[string]any{"enabled": false}})
	unixClient = apitest.UnixClient(dirs.Socket)
	req, err = http.NewRequest(http.MethodPatch, "http://localhost/api/v1/server", bytes.NewReader(off))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	disabled, err := unixClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer disabled.Body.Close()
	if disabled.StatusCode != http.StatusOK {
		t.Fatalf("disable listen %d", disabled.StatusCode)
	}
	if again.TLSAddr() != "" {
		t.Fatalf("still bound %s", again.TLSAddr())
	}
}

func mustHTTPS(t *testing.T, client *http.Client, url string) *http.Response {
	t.Helper()
	resp, err := client.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	return resp
}

func mustClientCSR(t *testing.T) (csrPEM, keyPEM []byte) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.CreateCertificateRequest(rand.Reader, &x509.CertificateRequest{
		Subject: pkix.Name{CommonName: "pending-client"},
	}, key)
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
