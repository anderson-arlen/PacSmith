package pki

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"
)

const (
	leafValidity  = 90 * 24 * time.Hour
	maxSNILength  = 255
	maxCachedSNIs = 32
)

type leafMaterial struct {
	certPEM []byte
	keyPEM  []byte
}

// LeafCache is an in-memory LRU of hostname/IP leaf certificates. It is not
// persistent: hostile SNI values cannot create unbounded on-disk certs.
type LeafCache struct {
	mu      sync.Mutex
	items   map[string]leafMaterial
	order   []string
	maxSize int
}

func NewLeafCache() *LeafCache {
	return &LeafCache{
		items:   make(map[string]leafMaterial),
		maxSize: maxCachedSNIs,
	}
}

func (c *LeafCache) Len() int {
	if c == nil {
		return 0
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.items)
}

func (c *LeafCache) get(key string) (leafMaterial, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	item, ok := c.items[key]
	if !ok {
		return leafMaterial{}, false
	}
	c.touchLocked(key)
	return copyLeaf(item), true
}

func (c *LeafCache) put(key string, item leafMaterial) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.items == nil {
		c.items = make(map[string]leafMaterial)
	}
	if c.maxSize <= 0 {
		c.maxSize = maxCachedSNIs
	}
	if _, ok := c.items[key]; ok {
		c.touchLocked(key)
		c.items[key] = copyLeaf(item)
		return
	}
	for len(c.items) >= c.maxSize && len(c.order) > 0 {
		oldest := c.order[0]
		c.order = c.order[1:]
		delete(c.items, oldest)
	}
	c.items[key] = copyLeaf(item)
	c.order = append(c.order, key)
}

func (c *LeafCache) touchLocked(key string) {
	for i, existing := range c.order {
		if existing == key {
			c.order = append(c.order[:i], c.order[i+1:]...)
			break
		}
	}
	c.order = append(c.order, key)
}

func copyLeaf(item leafMaterial) leafMaterial {
	return leafMaterial{
		certPEM: append([]byte(nil), item.certPEM...),
		keyPEM:  append([]byte(nil), item.keyPEM...),
	}
}

// LeafForHello issues (or returns a cached) server TLS leaf for a ClientHello
// SNI and/or IP. Leaves are not stable identity; the Server CA is.
func LeafForHello(serverCACert, serverCAKey []byte, cache *LeafCache, sni string, ip net.IP) ([]byte, []byte, error) {
	dnsNames, ips, cacheKey, commonName, err := helloIdentity(sni, ip)
	if err != nil {
		return nil, nil, err
	}
	if cache != nil {
		if cached, ok := cache.get(cacheKey); ok {
			return cached.certPEM, cached.keyPEM, nil
		}
	}
	caCert, caKey, err := parseCA(serverCACert, serverCAKey)
	if err != nil {
		return nil, nil, err
	}
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
		SerialNumber:          serial,
		Subject:               pkix.Name{Organization: []string{"PacSmith"}, CommonName: commonName},
		DNSNames:              dnsNames,
		IPAddresses:           ips,
		NotBefore:             now.Add(-clockSkew),
		NotAfter:              now.Add(leafValidity),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, caCert, &key.PublicKey, caKey)
	if err != nil {
		return nil, nil, fmt.Errorf("sign leaf: %w", err)
	}
	certPEM, err := encodePEM("CERTIFICATE", der)
	if err != nil {
		return nil, nil, err
	}
	keyPEM, err := encodePrivateKey(key)
	if err != nil {
		return nil, nil, err
	}
	if cache != nil {
		cache.put(cacheKey, leafMaterial{certPEM: certPEM, keyPEM: keyPEM})
	}
	return certPEM, keyPEM, nil
}

func helloIdentity(sni string, ip net.IP) (dnsNames []string, ips []net.IP, cacheKey, commonName string, err error) {
	sni = strings.TrimSpace(sni)
	if sni != "" {
		if parsed := net.ParseIP(sni); parsed != nil {
			parsed = normalizeIP(parsed)
			ips = appendIP(ips, parsed)
			cacheKey = "ip:" + parsed.String()
			commonName = parsed.String()
		} else {
			if err := validateSNI(sni); err != nil {
				return nil, nil, "", "", err
			}
			host := strings.ToLower(sni)
			dnsNames = []string{host}
			cacheKey = "dns:" + host
			commonName = host
		}
	}
	if ip != nil {
		ip = normalizeIP(ip)
		ips = appendIP(ips, ip)
		if cacheKey == "" {
			cacheKey = "ip:" + ip.String()
			commonName = ip.String()
		}
	}
	if cacheKey == "" {
		return nil, nil, "", "", fmt.Errorf("empty SNI")
	}
	return dnsNames, ips, cacheKey, commonName, nil
}

func validateSNI(sni string) error {
	if sni == "" {
		return fmt.Errorf("empty SNI")
	}
	if len(sni) > maxSNILength {
		return fmt.Errorf("SNI too long")
	}
	if strings.ContainsRune(sni, 0) {
		return fmt.Errorf("invalid SNI")
	}
	return nil
}

func normalizeIP(ip net.IP) net.IP {
	if v4 := ip.To4(); v4 != nil {
		return v4
	}
	return ip.To16()
}

func appendIP(ips []net.IP, ip net.IP) []net.IP {
	for _, existing := range ips {
		if existing.Equal(ip) {
			return ips
		}
	}
	return append(ips, ip)
}
