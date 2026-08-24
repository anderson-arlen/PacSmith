package version

// Version is the PacSmith package version. CMake overrides it via ldflags.
var Version = "dev"

// API is the HTTP protocol generation exposed under /api/<API>/.
const API = "v1"

// Capabilities advertised by this server build. Transport-specific listeners
// do not change the protocol; they only add a capability name.
var Capabilities = []string{
	"http",
	"unix",
	"artifacts",
	"library",
	"jobs",
	"pki",
	"ai",
	"repo",
}
