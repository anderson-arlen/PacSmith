package updatecheck

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (fn roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return fn(request)
}

func TestDirectCheckUsesStoredValidatorWithoutDownloading(t *testing.T) {
	requests := 0
	service := &Service{Client: &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		requests++
		if request.Method != http.MethodHead {
			t.Fatalf("unexpected method %s", request.Method)
		}
		if request.Header.Get("If-None-Match") != "\"stable\"" {
			t.Fatalf("missing conditional ETag: %q", request.Header.Get("If-None-Match"))
		}
		return &http.Response{
			StatusCode: http.StatusNotModified,
			Header:     http.Header{"Etag": []string{"\"stable\""}},
			Body:       io.NopCloser(strings.NewReader("")),
			Request:    request,
		}, nil
	})}}
	target := checkTarget{Update: map[string]any{
		"url":                             "https://vendor.invalid/demo.deb",
		"directUrlEtag":                   "\"stable\"",
		"directUrlContentLength":          "-1",
		"directUrlLastFullCheck":          "",
		"directUrlFullCheckIntervalHours": 24,
	}}
	result, err := service.checkDirect(context.Background(), target, false, func(string) {})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != "no-update" || result.Message == "" || requests != 1 {
		t.Fatalf("result %+v, requests %d", result, requests)
	}
}
