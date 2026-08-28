package updatecheck

import (
	"encoding/json"
	"strconv"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
)

const (
	StrategyManual = "Manual"
	StrategyDirect = "Direct URL"
	StrategyAPT    = "APT repository"
	StrategyRPM    = "RPM repository"
	StrategyGitHub = "GitHub releases"
)

type Result struct {
	ProjectID                string           `json:"project_id,omitempty"`
	ReleaseID                string           `json:"release_id,omitempty"`
	ProjectName              string           `json:"project_name,omitempty"`
	PackageName              string           `json:"package_name,omitempty"`
	Status                   string           `json:"status"`
	Message                  string           `json:"message"`
	DetectedVersion          string           `json:"detected_version,omitempty"`
	Filename                 string           `json:"filename,omitempty"`
	SHA256                   string           `json:"sha256,omitempty"`
	DownloadURL              string           `json:"download_url,omitempty"`
	UpdateAvailable          bool             `json:"update_available"`
	SignatureVerified        bool             `json:"signature_verified"`
	FullContentCheckDeferred bool             `json:"full_content_check_deferred,omitempty"`
	Prepared                 bool             `json:"prepared"`
	Built                    bool             `json:"built"`
	AutomaticStatus          string           `json:"automatic_status,omitempty"`
	AutomaticMessage         string           `json:"automatic_message,omitempty"`
	DiscoveredReleaseID      string           `json:"discovered_release_id,omitempty"`
	ProviderReleaseID        int64            `json:"provider_release_id,omitempty"`
	ProviderAssetID          int64            `json:"provider_asset_id,omitempty"`
	ProviderTag              string           `json:"provider_tag,omitempty"`
	PublisherDigest          string           `json:"publisher_digest,omitempty"`
	Prerelease               bool             `json:"prerelease,omitempty"`
	ETag                     string           `json:"etag,omitempty"`
	DirectLastModified       string           `json:"direct_last_modified,omitempty"`
	DirectContentLength      int64            `json:"direct_content_length,omitempty"`
	DirectValidatorName      string           `json:"direct_validator_name,omitempty"`
	DirectValidatorValue     string           `json:"direct_validator_value,omitempty"`
	DirectLastSHA256         string           `json:"direct_last_sha256,omitempty"`
	DirectLastFullCheck      string           `json:"direct_last_full_check,omitempty"`
	Artifact                 *artifact.Record `json:"-"`
}

type BatchResult struct {
	Checks []Result `json:"checks"`
	Failed int      `json:"failed"`
}

type Progress struct {
	Message     string
	ProjectID   string
	ReleaseID   string
	ProjectName string
	PackageName string
	Current     int64
	Total       int64
}

type checkTarget struct {
	Project library.Project
	Release library.Release
	Update  map[string]any
	Version string
}

func object(value any) map[string]any {
	result, _ := value.(map[string]any)
	return result
}

func objects(value any) []map[string]any {
	raw, _ := value.([]any)
	result := make([]map[string]any, 0, len(raw))
	for _, value := range raw {
		if item := object(value); item != nil {
			result = append(result, item)
		}
	}
	return result
}

func stringValue(document map[string]any, key string) string {
	value, _ := document[key].(string)
	return strings.TrimSpace(value)
}

func rawStringValue(document map[string]any, key string) string {
	value, _ := document[key].(string)
	return value
}

func boolValue(document map[string]any, key string) bool {
	value, _ := document[key].(bool)
	return value
}

func int64Value(document map[string]any, key string) int64 {
	switch value := document[key].(type) {
	case string:
		parsed, _ := strconv.ParseInt(value, 10, 64)
		return parsed
	case float64:
		return int64(value)
	case json.Number:
		parsed, _ := value.Int64()
		return parsed
	case int64:
		return value
	case int:
		return int64(value)
	default:
		return 0
	}
}

func cloneObject(source map[string]any) map[string]any {
	raw, _ := json.Marshal(source)
	result := map[string]any{}
	_ = json.Unmarshal(raw, &result)
	return result
}

func timestamp(now time.Time) string {
	return now.UTC().Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
}
