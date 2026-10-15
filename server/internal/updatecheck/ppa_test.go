package updatecheck

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"
)

const testPPAURL = "https://api.launchpad.net/1.0/~ubuntugis/+archive/ubuntu/ubuntugis-unstable"
const testPPAFingerprint = "2EC86B48E6A9F326623CD22FFF0E7BBEC491C6A1"

func ppaTestArchive() map[string]any {
	return map[string]any{"displayname": "UbuntuGIS", "private": false, "publish": true,
		"distribution_link": "https://api.launchpad.net/1.0/ubuntu", "signing_key_fingerprint": testPPAFingerprint}
}

func ppaTestService(t *testing.T, archive map[string]any, pages ...map[string]any) *Service {
	t.Helper()
	page := 0
	return &Service{Client: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		var response any
		if r.URL.String() == testPPAURL {
			response = archive
		} else {
			if r.URL.Host != "api.launchpad.net" || page >= len(pages) {
				t.Fatalf("unexpected request %s", r.URL)
			}
			if page == 0 && (r.URL.Query().Get("binary_name") != "grass-core" ||
				r.URL.Query().Get("exact_match") != "true") {
				t.Fatalf("lookup must filter by exact package name: %s", r.URL)
			}
			response = pages[page]
			page++
		}
		body, err := json.Marshal(response)
		if err != nil {
			t.Fatal(err)
		}
		return &http.Response{StatusCode: http.StatusOK, Request: r,
			Body: io.NopCloser(strings.NewReader(string(body)))}, nil
	})}}
}

func ppaTestBinary(name, suite, arch string) map[string]any {
	return map[string]any{"binary_package_name": name, "status": "Published",
		"distro_arch_series_link": "https://api.launchpad.net/1.0/ubuntu/" + suite + "/" + arch}
}

func TestResolvePPAPaginatesAndFiltersPackages(t *testing.T) {
	service := ppaTestService(t, ppaTestArchive(),
		map[string]any{"next_collection_link": testPPAURL + "?ws.start=300", "entries": []any{
			ppaTestBinary("grass-core", "noble", "amd64"), ppaTestBinary("grass", "noble", "arm64"),
			ppaTestBinary("grass", "noble", "amd64")}},
		map[string]any{"entries": []any{ppaTestBinary("grass-core", "noble", "amd64"),
			ppaTestBinary("grass-core", "jammy", "amd64"),
			map[string]any{"binary_package_name": "injected", "status": "Published", "distro_arch_series_link": "noble/amd64"}}})
	result, err := service.ResolvePPA(context.Background(), PPARequest{PPA: " ppa:ubuntugis/ubuntugis-unstable ", Package: "grass-core"})
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(result.Suites, []string{"jammy", "noble"}) || result.Package != "grass-core" {
		t.Fatalf("package releases = %#v", result)
	}
	if result.RepositoryURL != "https://ppa.launchpadcontent.net/ubuntugis/ubuntugis-unstable/ubuntu" ||
		result.SigningFingerprint != testPPAFingerprint || result.Architecture != "amd64" ||
		result.SigningKeyURL != "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x"+testPPAFingerprint {
		t.Fatalf("incorrect PPA identity: %+v", result)
	}
}

func TestResolvePPARejectsUnsafeInput(t *testing.T) {
	service := &Service{Client: &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		t.Fatal("invalid input must not make network requests")
		return nil, nil
	})}}
	for _, input := range []string{"", "ppa:owner", "ppa:owner/../archive", "https://evil.invalid/p/", "ppa:owner/archive?extra", "ppa:~owner/archive"} {
		if _, err := service.ResolvePPA(context.Background(), PPARequest{PPA: input, Package: "grass-core"}); err == nil {
			t.Errorf("accepted %q", input)
		}
	}
	for _, name := range []string{"", "../grass-core", "grass-core&status=Deleted", "Grass", "grass core"} {
		if _, err := service.ResolvePPA(context.Background(), PPARequest{PPA: "ppa:owner/archive", Package: name}); err == nil {
			t.Errorf("accepted package %q", name)
		}
	}
}

func TestResolvePPARejectsUnusableArchive(t *testing.T) {
	for _, test := range []struct {
		field string
		value any
	}{
		{"private", true}, {"publish", false}, {"distribution_link", "https://evil.invalid/ubuntu"},
		{"signing_key_fingerprint", "unknown"},
	} {
		t.Run(test.field, func(t *testing.T) {
			archive := ppaTestArchive()
			archive[test.field] = test.value
			if _, err := ppaTestService(t, archive).ResolvePPA(context.Background(), PPARequest{PPA: "ppa:ubuntugis/ubuntugis-unstable", Package: "grass-core"}); err == nil {
				t.Fatal("unusable archive accepted")
			}
		})
	}
}

func TestResolvePPARejectsUnsafeOrRepeatingPagination(t *testing.T) {
	for _, next := range []string{"https://evil.invalid/packages", "http://api.launchpad.net/1.0/packages", testPPAURL + "/other?ws.start=300",
		testPPAURL + "?ws.op=getPublishedBinaries&status=Published&ws.size=300&binary_name=grass-core&exact_match=true"} {
		service := ppaTestService(t, ppaTestArchive(), map[string]any{"next_collection_link": next})
		if _, err := service.ResolvePPA(context.Background(), PPARequest{PPA: "ppa:ubuntugis/ubuntugis-unstable", Package: "grass-core"}); err == nil {
			t.Errorf("accepted pagination %q", next)
		}
	}
}

func TestResolvePPARejectsEmptyArchitecture(t *testing.T) {
	service := ppaTestService(t, ppaTestArchive(), map[string]any{"entries": []any{ppaTestBinary("grass-core", "noble", "arm64")}})
	if _, err := service.ResolvePPA(context.Background(), PPARequest{PPA: "ppa:ubuntugis/ubuntugis-unstable", Package: "grass-core", Architecture: "amd64"}); err == nil {
		t.Fatal("archive without matching packages accepted")
	}
}
