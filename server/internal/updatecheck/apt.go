package updatecheck

import (
	"bufio"
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"

	"github.com/klauspost/compress/zstd"
	"github.com/ulikunitz/xz"
)

const maximumMetadataBytes = 256 << 20

type aptReleaseEntry struct {
	Path, SHA256 string
	Size         int64
}

type aptPackage struct {
	Name, Version, Architecture, Filename, SHA256 string
	Size                                          int64
}

var aptReleaseLine = regexp.MustCompile(`^([0-9a-fA-F]{64})\s+([0-9]+)\s+(.+)$`)
var sha256Pattern = regexp.MustCompile(`^[0-9a-fA-F]{64}$`)

func (s *Service) checkAPT(ctx context.Context, target checkTarget, log func(string)) (Result, error) {
	update := target.Update
	if stringValue(update, "aptPackageName") == "" || stringValue(update, "aptArchitecture") == "" ||
		stringValue(update, "aptSuite") == "" {
		return Result{}, fmt.Errorf("APT suite, package, and architecture must be configured")
	}
	base, err := repositoryBase(stringValue(update, "url"))
	if err != nil {
		return Result{}, fmt.Errorf("APT repository URL: %w", err)
	}
	suite := stringValue(update, "aptSuite")
	flat := strings.HasSuffix(suite, "/")
	if suite != "./" && !safeRepositoryPath(suite, flat) {
		return Result{}, fmt.Errorf("APT suite contains an unsafe path")
	}
	component := stringValue(update, "aptComponent")
	if !flat && !safeRepositoryPath(component, false) {
		return Result{}, fmt.Errorf("APT component contains an unsafe path")
	}
	relativeRoot := suite
	if !flat {
		relativeRoot = "dists/" + suite + "/"
	}
	releaseRoot := base
	if relativeRoot != "./" {
		releaseRoot, err = resolveDirectoryURL(base, relativeRoot)
		if err != nil {
			return Result{}, err
		}
	}
	log("Downloading signed APT release metadata…\n")
	inReleaseURL, _ := resolveRepositoryURL(releaseRoot, "InRelease")
	releaseData, _, status, requestErr := s.request(ctx, http.MethodGet, inReleaseURL, nil, 16<<20)
	clearSigned := requestErr == nil && successfulStatus(status)
	if clearSigned {
		if err := s.verifyClearSigned(ctx, target, releaseData); err != nil {
			return Result{}, err
		}
	} else {
		if requestErr != nil || status != http.StatusNotFound {
			return Result{}, requestFailure("APT InRelease request failed", status, requestErr)
		}
		log("InRelease is unavailable; downloading Release metadata and signature…\n")
		releaseURL, _ := resolveRepositoryURL(releaseRoot, "Release")
		releaseData, _, status, requestErr = s.request(ctx, http.MethodGet, releaseURL, nil, 16<<20)
		if requestErr != nil || !successfulStatus(status) {
			return Result{}, requestFailure("APT Release request failed", status, requestErr)
		}
		signatureURL, _ := resolveRepositoryURL(releaseRoot, "Release.gpg")
		signature, _, signatureStatus, signatureErr := s.request(ctx, http.MethodGet, signatureURL, nil, 4<<20)
		if signatureErr != nil || !successfulStatus(signatureStatus) {
			return Result{}, requestFailure("APT Release.gpg request failed", signatureStatus, signatureErr)
		}
		if err := s.verifyDetached(ctx, target, releaseData, signature); err != nil {
			return Result{}, err
		}
	}
	entries, err := parseAPTRelease(releaseData)
	if err != nil {
		return Result{}, err
	}
	entry, err := selectAPTIndex(entries, component, stringValue(update, "aptArchitecture"), flat)
	if err != nil {
		return Result{}, err
	}
	indexURL, err := resolveRepositoryURL(releaseRoot, entry.Path)
	if err != nil {
		return Result{}, err
	}
	log(fmt.Sprintf("Downloading %s…\n", entry.Path))
	compressed, _, status, err := s.request(ctx, http.MethodGet, indexURL, nil, 64<<20)
	if err != nil || !successfulStatus(status) {
		return Result{}, requestFailure("APT Packages request failed", status, err)
	}
	if int64(len(compressed)) != entry.Size {
		return Result{}, fmt.Errorf("Packages index size does not match signed Release metadata")
	}
	digest := sha256.Sum256(compressed)
	if hex.EncodeToString(digest[:]) != entry.SHA256 {
		return Result{}, fmt.Errorf("Packages index SHA256 does not match signed Release metadata")
	}
	uncompressed, err := decompressMetadata(compressed)
	if err != nil {
		return Result{}, fmt.Errorf("decompress Packages index: %w", err)
	}
	selected, err := latestAPTPackage(uncompressed, stringValue(update, "aptPackageName"),
		stringValue(update, "aptArchitecture"))
	if err != nil {
		return Result{}, err
	}
	downloadURL, err := resolveRepositoryURL(base, selected.Filename)
	if err != nil {
		return Result{}, err
	}
	available := debianVersionCompare(selected.Version, target.Version) > 0
	statusName, prefix := "no-update", "No newer version"
	if available {
		statusName, prefix = "update", "Update available"
	}
	return Result{Status: statusName, UpdateAvailable: available, DetectedVersion: selected.Version,
		Filename: selected.Filename, SHA256: selected.SHA256, DownloadURL: downloadURL.String(),
		SignatureVerified: true,
		Message: fmt.Sprintf("%s: %s %s (%s; repository signature and Packages index hash verified)",
			prefix, selected.Name, selected.Version, selected.Architecture)}, nil
}

func resolveDirectoryURL(base *url.URL, relative string) (*url.URL, error) {
	trimmed := strings.TrimSuffix(relative, "/")
	if !safeRepositoryPath(relative, true) || trimmed == "" {
		return nil, fmt.Errorf("repository directory contains an unsafe path")
	}
	ref, _ := url.Parse(relative)
	resolved := base.ResolveReference(ref)
	if !strings.HasSuffix(resolved.Path, "/") {
		resolved.Path += "/"
	}
	return resolved, validateHTTPURL(resolved)
}

func clearSignedPayload(data []byte) ([]byte, error) {
	if !bytes.HasPrefix(data, []byte("-----BEGIN PGP SIGNED MESSAGE-----")) {
		return data, nil
	}
	bodyStart := bytes.Index(data, []byte("\n\n"))
	if bodyStart < 0 {
		return nil, fmt.Errorf("invalid clear-signed InRelease data")
	}
	bodyStart += 2
	signature := bytes.Index(data[bodyStart:], []byte("\n-----BEGIN PGP SIGNATURE-----"))
	if signature < 0 {
		return nil, fmt.Errorf("invalid clear-signed InRelease data")
	}
	payload := append([]byte(nil), data[bodyStart:bodyStart+signature]...)
	payload = bytes.ReplaceAll(payload, []byte("\n- "), []byte("\n"))
	if bytes.HasPrefix(payload, []byte("- ")) {
		payload = payload[2:]
	}
	return payload, nil
}

func parseAPTRelease(data []byte) ([]aptReleaseEntry, error) {
	payload, err := clearSignedPayload(data)
	if err != nil {
		return nil, err
	}
	paragraphs := parseControlParagraphs(payload)
	if len(paragraphs) == 0 {
		return nil, fmt.Errorf("Release metadata has no fields")
	}
	var result []aptReleaseEntry
	for _, line := range strings.Split(paragraphs[0]["SHA256"], "\n") {
		match := aptReleaseLine.FindStringSubmatch(strings.TrimSpace(line))
		if len(match) != 4 || !safeRepositoryPath(match[3], false) {
			continue
		}
		size, err := strconv.ParseInt(match[2], 10, 64)
		if err != nil || size < 0 {
			continue
		}
		result = append(result, aptReleaseEntry{Path: match[3], SHA256: strings.ToLower(match[1]), Size: size})
	}
	if len(result) == 0 {
		return nil, fmt.Errorf("Release metadata has no usable SHA256 index entries")
	}
	return result, nil
}

func selectAPTIndex(entries []aptReleaseEntry, component, architecture string, flat bool) (aptReleaseEntry, error) {
	stem := "Packages"
	if !flat {
		stem = component + "/binary-" + architecture + "/Packages"
	}
	for _, suffix := range []string{".xz", ".gz", ".zst", ".bz2", ""} {
		for _, entry := range entries {
			if entry.Path == stem+suffix {
				return entry, nil
			}
		}
	}
	return aptReleaseEntry{}, fmt.Errorf("no Packages index was published for %s/%s", component, architecture)
}

func parseControlParagraphs(data []byte) []map[string]string {
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(make([]byte, 64<<10), maximumMetadataBytes)
	var result []map[string]string
	current := map[string]string{}
	lastKey := ""
	flush := func() {
		if len(current) > 0 {
			result = append(result, current)
			current = map[string]string{}
			lastKey = ""
		}
	}
	for scanner.Scan() {
		line := strings.TrimSuffix(scanner.Text(), "\r")
		if line == "" {
			flush()
			continue
		}
		if (strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t")) && lastKey != "" {
			current[lastKey] += "\n" + strings.TrimSpace(line)
			continue
		}
		separator := strings.IndexByte(line, ':')
		if separator <= 0 {
			continue
		}
		lastKey = line[:separator]
		current[lastKey] = strings.TrimSpace(line[separator+1:])
	}
	flush()
	return result
}

func latestAPTPackage(data []byte, name, architecture string) (aptPackage, error) {
	var best *aptPackage
	for _, fields := range parseControlParagraphs(data) {
		filename := normalizeAPTFilePath(fields["Filename"])
		candidate := aptPackage{Name: fields["Package"], Version: fields["Version"],
			Architecture: fields["Architecture"], Filename: filename, SHA256: strings.ToLower(fields["SHA256"])}
		candidate.Size, _ = strconv.ParseInt(fields["Size"], 10, 64)
		if candidate.Name != name || candidate.Architecture != architecture && candidate.Architecture != "all" ||
			candidate.Version == "" || !safeRepositoryPath(candidate.Filename, false) ||
			!sha256Pattern.MatchString(candidate.SHA256) || candidate.Size < 0 {
			continue
		}
		if best == nil || debianVersionCompare(candidate.Version, best.Version) > 0 {
			copy := candidate
			best = &copy
		}
	}
	if best == nil {
		return aptPackage{}, fmt.Errorf("package %s for architecture %s was not found in the repository index", name, architecture)
	}
	return *best, nil
}

func normalizeAPTFilePath(value string) string {
	if strings.HasPrefix(value, "./") {
		return strings.TrimPrefix(value, "./")
	}
	return value
}

func decompressMetadata(data []byte) ([]byte, error) {
	var reader io.Reader = bytes.NewReader(data)
	cleanup := func() {}
	var err error
	switch {
	case len(data) >= 2 && data[0] == 0x1f && data[1] == 0x8b:
		var gzipReader *gzip.Reader
		gzipReader, err = gzip.NewReader(reader)
		if gzipReader != nil {
			reader = gzipReader
			cleanup = func() { _ = gzipReader.Close() }
		}
	case len(data) >= 6 && bytes.Equal(data[:6], []byte{0xfd, '7', 'z', 'X', 'Z', 0x00}):
		reader, err = xz.NewReader(reader)
	case len(data) >= 4 && bytes.Equal(data[:4], []byte{0x28, 0xb5, 0x2f, 0xfd}):
		var zstdReader *zstd.Decoder
		zstdReader, err = zstd.NewReader(reader)
		if zstdReader != nil {
			reader = zstdReader
			cleanup = zstdReader.Close
		}
	case len(data) >= 3 && string(data[:3]) == "BZh":
		reader = bzip2.NewReader(reader)
	}
	if err != nil {
		return nil, err
	}
	defer cleanup()
	result, err := io.ReadAll(io.LimitReader(reader, maximumMetadataBytes+1))
	if err != nil {
		return nil, err
	}
	if len(result) > maximumMetadataBytes {
		return nil, fmt.Errorf("decompressed metadata exceeds the 256 MiB safety limit")
	}
	return result, nil
}
