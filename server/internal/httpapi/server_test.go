package httpapi_test

import (
	"net/http/httptest"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/apitest"
)

func TestHTTPAPIContract(t *testing.T) {
	handler, _ := apitest.NewHandler(t)
	server := httptest.NewServer(handler)
	t.Cleanup(server.Close)
	apitest.RunContract(t, server.Client(), server.URL)
}
