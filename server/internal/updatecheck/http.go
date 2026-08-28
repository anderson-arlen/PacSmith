package updatecheck

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const userAgent = "PacSmith/0.2"

func defaultHTTPClient() *http.Client {
	return &http.Client{
		Transport: &http.Transport{
			Proxy:                 http.ProxyFromEnvironment,
			DialContext:           (&net.Dialer{Timeout: 20 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
			TLSHandshakeTimeout:   20 * time.Second,
			ResponseHeaderTimeout: 30 * time.Second,
			ExpectContinueTimeout: time.Second,
			IdleConnTimeout:       90 * time.Second,
		},
		CheckRedirect: func(request *http.Request, via []*http.Request) error {
			if len(via) >= 10 {
				return fmt.Errorf("too many redirects")
			}
			if err := validateHTTPURL(request.URL); err != nil {
				return err
			}
			if len(via) > 0 && via[len(via)-1].URL.Scheme == "https" && request.URL.Scheme != "https" {
				return fmt.Errorf("refusing HTTPS downgrade redirect")
			}
			return nil
		},
	}
}

func validateHTTPURL(value *url.URL) error {
	if value == nil || value.Host == "" || value.User != nil ||
		(value.Scheme != "http" && value.Scheme != "https") {
		return fmt.Errorf("URL must be an absolute HTTP or HTTPS URL without credentials")
	}
	return nil
}

func repositoryBase(raw string) (*url.URL, error) {
	parsed, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return nil, fmt.Errorf("repository URL must not contain a query or fragment")
	}
	if err := validateHTTPURL(parsed); err != nil {
		return nil, err
	}
	if !strings.HasSuffix(parsed.Path, "/") {
		parsed.Path += "/"
	}
	return parsed, nil
}

func resolveRepositoryURL(base *url.URL, relative string) (*url.URL, error) {
	if !safeRepositoryPath(relative, false) {
		return nil, fmt.Errorf("repository metadata contains an unsafe relative path")
	}
	ref, err := url.Parse(relative)
	if err != nil || ref.IsAbs() || ref.Host != "" {
		return nil, fmt.Errorf("repository metadata contains an unsafe URL")
	}
	resolved := base.ResolveReference(ref)
	if err := validateHTTPURL(resolved); err != nil {
		return nil, err
	}
	return resolved, nil
}

func (s *Service) request(ctx context.Context, method string, target *url.URL, headers http.Header,
	maximum int64) ([]byte, http.Header, int, error) {
	if err := validateHTTPURL(target); err != nil {
		return nil, nil, 0, err
	}
	request, err := http.NewRequestWithContext(ctx, method, target.String(), nil)
	if err != nil {
		return nil, nil, 0, err
	}
	request.Header.Set("User-Agent", userAgent)
	request.Header.Set("Accept-Encoding", "identity")
	for name, values := range headers {
		for _, value := range values {
			request.Header.Add(name, value)
		}
	}
	response, err := s.httpClient().Do(request)
	if err != nil {
		return nil, nil, 0, err
	}
	defer response.Body.Close()
	effectiveURL := target
	if response.Request != nil && response.Request.URL != nil {
		effectiveURL = response.Request.URL
	}
	if err := validateHTTPURL(effectiveURL); err != nil {
		return nil, nil, 0, err
	}
	if target.Scheme == "https" && effectiveURL.Scheme != "https" {
		return nil, nil, 0, fmt.Errorf("refusing HTTPS downgrade redirect")
	}
	if maximum == 0 || method == http.MethodHead {
		return nil, response.Header.Clone(), response.StatusCode, nil
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maximum+1))
	if err != nil {
		return nil, nil, response.StatusCode, err
	}
	if int64(len(body)) > maximum {
		return nil, nil, response.StatusCode, fmt.Errorf("response exceeds the %d-byte safety limit", maximum)
	}
	return body, response.Header.Clone(), response.StatusCode, nil
}

func safeRepositoryPath(value string, allowTrailingSlash bool) bool {
	if value == "" || strings.HasPrefix(value, "/") || strings.Contains(value, "\\") ||
		strings.Contains(value, ":") || strings.HasPrefix(value, "//") {
		return false
	}
	if !allowTrailingSlash && strings.HasSuffix(value, "/") {
		return false
	}
	for _, part := range strings.Split(value, "/") {
		if part == "" && allowTrailingSlash {
			continue
		}
		if part == "" || part == "." || part == ".." {
			return false
		}
		for _, character := range part {
			if character >= 'a' && character <= 'z' || character >= 'A' && character <= 'Z' ||
				character >= '0' && character <= '9' || strings.ContainsRune("._+-", character) {
				continue
			}
			return false
		}
	}
	return true
}

func successfulStatus(status int) bool { return status >= 200 && status < 300 }

func requestFailure(label string, status int, err error) error {
	if err != nil {
		return fmt.Errorf("%s: %w", label, err)
	}
	return fmt.Errorf("%s (HTTP %d)", label, status)
}
