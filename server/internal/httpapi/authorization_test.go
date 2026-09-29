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

func TestACPRequestsUseLibraryAuthorization(t *testing.T) {
	for _, route := range []struct{ method, path string }{
		{"GET", "/api/v1/ai/settings"}, {"PUT", "/api/v1/ai/settings"},
		{"GET", "/api/v1/ai/conversations"}, {"GET", "/api/v1/ai/permissions"},
		{"POST", "/api/v1/ai/permissions/request/response"}, {"POST", "/api/v1/ai/conversations/chat/prompt"},
	} {
		request := httptest.NewRequest(route.method, route.path, nil)
		if authorized(auth.Principal{Kind: auth.KindEnrollment}, request) {
			t.Fatalf("unauthenticated ACP access: %s", route.path)
		}
		if !authorized(auth.Principal{Kind: auth.KindRemoteClient, ClientID: "client-a"}, request) {
			t.Fatalf("remote ACP denied: %s", route.path)
		}
	}
}
