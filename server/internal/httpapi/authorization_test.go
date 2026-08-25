package httpapi

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/auth"
)

func TestEventStreamRequiresLibraryAuthorization(t *testing.T) {
	request := httptest.NewRequest(http.MethodGet, "/api/v1/events", nil)
	if authorized(auth.Principal{Kind: auth.KindEnrollment}, request) {
		t.Fatal("enrollment principal was authorized for event stream")
	}
	if !authorized(auth.Principal{Kind: auth.KindRemoteClient}, request) {
		t.Fatal("remote library client was denied event stream")
	}
	if !authorized(auth.LocalUnix(), request) {
		t.Fatal("local administrator was denied event stream")
	}
}
