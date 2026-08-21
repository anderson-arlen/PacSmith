package inspect

import (
	"net/url"
	"strings"
)

func usableHTTPURI(value string) bool {
	parsed, err := url.Parse(value)
	return err == nil && parsed.Host != "" &&
		(parsed.Scheme == "https" || parsed.Scheme == "http")
}

func strippedPayloadPath(path, prefix string) string {
	if prefix == "" {
		return path
	}
	rooted := prefix + "/"
	if strings.HasPrefix(path, rooted) {
		return strings.TrimPrefix(path, rooted)
	}
	return path
}
