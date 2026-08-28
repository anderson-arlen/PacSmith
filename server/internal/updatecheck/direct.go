package updatecheck

import (
	"context"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"path"
	"strconv"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
)

type directValidators struct {
	ETag, LastModified      string
	ContentLength           int64
	VendorName, VendorValue string
}

func (value directValidators) available() bool {
	return value.ETag != "" || value.LastModified != "" || value.VendorName != "" && value.VendorValue != ""
}

func directValidatorsFrom(update map[string]any) directValidators {
	length := int64Value(update, "directUrlContentLength")
	if _, exists := update["directUrlContentLength"]; !exists {
		length = -1
	}
	return directValidators{
		ETag:          stringValue(update, "directUrlEtag"),
		LastModified:  stringValue(update, "directUrlLastModified"),
		ContentLength: length,
		VendorName:    stringValue(update, "directUrlVendorValidatorName"),
		VendorValue:   stringValue(update, "directUrlVendorValidator"),
	}
}

func directValidatorsFromHeaders(headers http.Header) directValidators {
	length, err := strconv.ParseInt(headers.Get("Content-Length"), 10, 64)
	if err != nil || length < 0 {
		length = -1
	}
	result := directValidators{
		ETag:          strings.TrimSpace(headers.Get("ETag")),
		LastModified:  strings.TrimSpace(headers.Get("Last-Modified")),
		ContentLength: length,
	}
	for _, name := range []string{"X-Amz-Version-Id", "X-Goog-Generation"} {
		if value := strings.TrimSpace(headers.Get(name)); value != "" {
			result.VendorName = strings.ToLower(name)
			result.VendorValue = value
			break
		}
	}
	return result
}

func sameDirectValidators(stored, remote directValidators) (bool, bool) {
	if stored.ETag != "" && remote.ETag != "" {
		return stored.ETag == remote.ETag, true
	}
	if stored.VendorName != "" && stored.VendorName == remote.VendorName &&
		stored.VendorValue != "" && remote.VendorValue != "" {
		return stored.VendorValue == remote.VendorValue, true
	}
	if stored.LastModified != "" && remote.LastModified != "" {
		sameLength := stored.ContentLength < 0 || remote.ContentLength < 0 ||
			stored.ContentLength == remote.ContentLength
		return stored.LastModified == remote.LastModified && sameLength, true
	}
	return false, false
}

func (s *Service) checkDirect(ctx context.Context, target checkTarget, force bool, log func(string)) (Result, error) {
	rawURL := stringValue(target.Update, "url")
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return Result{}, fmt.Errorf("Direct URL is invalid: %w", err)
	}
	if err := validateHTTPURL(parsed); err != nil {
		return Result{}, err
	}
	stored := directValidatorsFrom(target.Update)
	headers := http.Header{}
	if stored.ETag != "" {
		headers.Set("If-None-Match", stored.ETag)
	} else if stored.LastModified != "" {
		headers.Set("If-Modified-Since", stored.LastModified)
	}
	log("Checking Direct URL headers…\n")
	_, responseHeaders, status, requestErr := s.request(ctx, http.MethodHead, parsed, headers, 0)
	remote := directValidators{}
	filename := path.Base(parsed.Path)
	if status == http.StatusNotModified {
		remote = stored
		observed := directValidatorsFromHeaders(responseHeaders)
		if observed.ETag != "" {
			remote.ETag = observed.ETag
		}
		if observed.LastModified != "" {
			remote.LastModified = observed.LastModified
		}
		if observed.ContentLength >= 0 {
			remote.ContentLength = observed.ContentLength
		}
		if observed.VendorValue != "" {
			remote.VendorName, remote.VendorValue = observed.VendorName, observed.VendorValue
		}
		return directObservation(remote, "Direct URL validators have not changed"), nil
	}
	if status != http.StatusMethodNotAllowed && status != http.StatusNotImplemented {
		if requestErr != nil {
			return Result{}, fmt.Errorf("Direct URL header check failed: %w", requestErr)
		}
		if status < 200 || status >= 400 {
			return Result{}, fmt.Errorf("Direct URL header check failed (HTTP %d)", status)
		}
		remote = directValidatorsFromHeaders(responseHeaders)
		filename = responseFilename(responseHeaders, parsed, filename)
	}
	if unchanged, comparable := sameDirectValidators(stored, remote); comparable && unchanged {
		return directObservation(remote, "Direct URL validators have not changed"), nil
	}
	if !remote.available() && !force && !directFullCheckDue(target.Update, s.now()) {
		result := directObservation(remote, "The server provides no cheap change validator; the next full-content check is not due yet")
		result.FullContentCheckDeferred = true
		if int64Value(target.Update, "directUrlFullCheckIntervalHours") <= 0 {
			result.Message = "The server provides no cheap change validator; full-content checks are manual only"
		}
		return result, nil
	}
	if filename == "" || filename == "." || filename == "/" {
		filename = stringValue(target.Release.Document, "originalSourceFilename")
	}
	if filename == "" {
		filename = "vendor-artifact"
	}
	log("Downloading Direct URL artifact for SHA256 comparison…\n")
	record, err := s.downloadArtifact(ctx, parsed, filename, "vendor")
	if err != nil {
		return Result{}, err
	}
	result := directObservation(remote, "")
	result.SHA256 = record.SHA256
	result.DirectLastSHA256 = record.SHA256
	result.DirectLastFullCheck = timestamp(s.now())
	baseline := stringValue(target.Update, "directUrlLastSha256")
	if baseline == "" {
		baseline = target.Release.SourceSHA256
	}
	if baseline != "" && strings.EqualFold(baseline, record.SHA256) {
		result.Message = "Direct URL artifact bytes are unchanged (SHA256 matched)"
		return result, nil
	}
	artifactPath, err := s.Artifacts.Store.Path(record.SHA256)
	if err != nil {
		return Result{}, err
	}
	analysis, err := inspect.AnalyzeArtifact(artifactPath, record.OriginalFilename)
	if err != nil {
		return Result{}, fmt.Errorf("inspect changed Direct URL artifact: %w", err)
	}
	if strings.TrimSpace(analysis.Metadata.Version) == "" {
		return Result{}, fmt.Errorf("changed Direct URL artifact did not expose a usable package version")
	}
	result.UpdateAvailable = true
	result.Status = "update"
	result.DetectedVersion = analysis.Metadata.Version
	result.Filename = record.OriginalFilename
	result.DownloadURL = parsed.String()
	result.Artifact = &record
	result.Message = fmt.Sprintf("Direct URL artifact changed; version %s is available", result.DetectedVersion)
	return result, nil
}

func directObservation(remote directValidators, message string) Result {
	return Result{Status: "no-update", Message: message, ETag: remote.ETag,
		DirectLastModified: remote.LastModified, DirectContentLength: remote.ContentLength,
		DirectValidatorName: remote.VendorName, DirectValidatorValue: remote.VendorValue}
}

func directFullCheckDue(update map[string]any, now time.Time) bool {
	interval := int64Value(update, "directUrlFullCheckIntervalHours")
	if _, exists := update["directUrlFullCheckIntervalHours"]; !exists {
		interval = 24
	}
	if interval <= 0 {
		return false
	}
	last, err := time.Parse(time.RFC3339Nano, stringValue(update, "directUrlLastFullCheck"))
	return err != nil || now.Sub(last) >= time.Duration(interval)*time.Hour
}

func responseFilename(headers http.Header, target *url.URL, fallback string) string {
	if _, parameters, err := mime.ParseMediaType(headers.Get("Content-Disposition")); err == nil {
		if name := path.Base(parameters["filename"]); name != "" && name != "." {
			fallback = name
		}
	}
	if fallback == "" {
		fallback = path.Base(target.Path)
	}
	name, err := artifact.SanitizeFilename(fallback)
	if err != nil {
		return "vendor-artifact"
	}
	return name
}

type boundedReader struct {
	reader io.Reader
	left   int64
}

func (reader *boundedReader) Read(buffer []byte) (int, error) {
	if reader.left <= 0 {
		var probe [1]byte
		count, err := reader.reader.Read(probe[:])
		if count > 0 {
			return 0, fmt.Errorf("artifact exceeds the %d-byte safety limit", artifact.MaxBytes)
		}
		return 0, err
	}
	if int64(len(buffer)) > reader.left {
		buffer = buffer[:reader.left]
	}
	count, err := reader.reader.Read(buffer)
	reader.left -= int64(count)
	return count, err
}

func (s *Service) downloadArtifact(ctx context.Context, target *url.URL, filename, kind string) (artifact.Record, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, target.String(), nil)
	if err != nil {
		return artifact.Record{}, err
	}
	request.Header.Set("User-Agent", userAgent)
	request.Header.Set("Accept-Encoding", "identity")
	response, err := s.httpClient().Do(request)
	if err != nil {
		return artifact.Record{}, err
	}
	defer response.Body.Close()
	effectiveURL := target
	if response.Request != nil && response.Request.URL != nil {
		effectiveURL = response.Request.URL
	}
	if err := validateHTTPURL(effectiveURL); err != nil {
		return artifact.Record{}, err
	}
	if target.Scheme == "https" && effectiveURL.Scheme != "https" {
		return artifact.Record{}, fmt.Errorf("refusing HTTPS downgrade redirect")
	}
	if !successfulStatus(response.StatusCode) {
		return artifact.Record{}, fmt.Errorf("artifact download failed (HTTP %d)", response.StatusCode)
	}
	if response.ContentLength > artifact.MaxBytes {
		return artifact.Record{}, fmt.Errorf("artifact exceeds the %d-byte safety limit", artifact.MaxBytes)
	}
	filename = responseFilename(response.Header, effectiveURL, filename)
	return s.Artifacts.Put(ctx, filename, kind, &boundedReader{reader: response.Body, left: artifact.MaxBytes})
}
