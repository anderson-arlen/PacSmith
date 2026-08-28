package updatecheck

import (
	"bytes"
	"context"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"hash"
	"io"
	"net/http"
	"sort"
	"strings"
)

type rpmPrimary struct {
	Path, ChecksumType, Checksum string
}

type rpmPackage struct {
	Name, Architecture, Epoch, Version, Release string
	Filename, ChecksumType, Checksum            string
}

func (item rpmPackage) evr() string {
	value := item.Version
	if item.Release != "" {
		value += "-" + item.Release
	}
	if item.Epoch != "" && item.Epoch != "0" {
		value = item.Epoch + ":" + value
	}
	return value
}

func (s *Service) checkRPM(ctx context.Context, target checkTarget, log func(string)) (Result, error) {
	update := target.Update
	if stringValue(update, "rpmPackageName") == "" || stringValue(update, "rpmArchitecture") == "" {
		return Result{}, fmt.Errorf("RPM package name and architecture must be configured")
	}
	base, err := repositoryBase(stringValue(update, "url"))
	if err != nil {
		return Result{}, fmt.Errorf("RPM repository URL: %w", err)
	}
	repomdURL, _ := resolveRepositoryURL(base, "repodata/repomd.xml")
	log("Downloading signed RPM repository metadata…\n")
	repomd, _, status, err := s.request(ctx, http.MethodGet, repomdURL, nil, 16<<20)
	if err != nil || !successfulStatus(status) {
		return Result{}, requestFailure("RPM repomd.xml request failed", status, err)
	}
	signatureURL, _ := resolveRepositoryURL(base, "repodata/repomd.xml.asc")
	signature, _, signatureStatus, signatureErr := s.request(ctx, http.MethodGet, signatureURL, nil, 4<<20)
	if signatureErr != nil || !successfulStatus(signatureStatus) {
		return Result{}, requestFailure("RPM repomd.xml signature request failed", signatureStatus, signatureErr)
	}
	if err := s.verifyDetached(ctx, target, repomd, signature); err != nil {
		return Result{}, err
	}
	primary, err := parseRPMRepomd(repomd)
	if err != nil {
		return Result{}, err
	}
	primaryURL, err := resolveRepositoryURL(base, primary.Path)
	if err != nil {
		return Result{}, err
	}
	log("Downloading verified RPM primary metadata…\n")
	compressed, _, primaryStatus, primaryErr := s.request(ctx, http.MethodGet, primaryURL, nil, 128<<20)
	if primaryErr != nil || !successfulStatus(primaryStatus) {
		return Result{}, requestFailure("RPM primary metadata request failed", primaryStatus, primaryErr)
	}
	if metadataChecksum(compressed, primary.ChecksumType) != primary.Checksum {
		return Result{}, fmt.Errorf("RPM primary metadata checksum does not match signed repomd.xml")
	}
	primaryXML, err := decompressMetadata(compressed)
	if err != nil {
		return Result{}, fmt.Errorf("decompress RPM primary metadata: %w", err)
	}
	selected, err := latestRPMPackage(primaryXML, stringValue(update, "rpmPackageName"),
		stringValue(update, "rpmArchitecture"))
	if err != nil {
		return Result{}, err
	}
	downloadURL, err := resolveRepositoryURL(base, selected.Filename)
	if err != nil {
		return Result{}, err
	}
	version := selected.evr()
	available := rpmVersionCompare(version, target.Version) > 0
	statusName, prefix := "no-update", "No newer version"
	if available {
		statusName, prefix = "update", "Update available"
	}
	result := Result{Status: statusName, UpdateAvailable: available, DetectedVersion: version,
		Filename: selected.Filename, DownloadURL: downloadURL.String(), SignatureVerified: true}
	verification := "repomd signature and metadata checksums verified"
	if normalizeRPMPackageChecksumType(selected.ChecksumType) == "sha256" {
		result.SHA256 = selected.Checksum
	} else if available {
		label := rpmPackageChecksumLabel(selected.ChecksumType)
		log(fmt.Sprintf("Downloading RPM package to verify signed %s checksum and compute SHA256…\n", label))
		record, err := s.downloadArtifact(ctx, downloadURL, selected.Filename, "vendor")
		if err != nil {
			return Result{}, err
		}
		if err := s.verifyDownloadedRPMPackageChecksum(record.SHA256, selected.ChecksumType,
			selected.Checksum); err != nil {
			return Result{}, err
		}
		result.SHA256 = record.SHA256
		result.Artifact = &record
		verification += fmt.Sprintf("; package %s verified and SHA256 computed", label)
	}
	result.Message = fmt.Sprintf("%s: %s %s (%s; %s)", prefix, selected.Name, version,
		selected.Architecture, verification)
	return result, nil
}

func parseRPMRepomd(data []byte) (rpmPrimary, error) {
	decoder := xml.NewDecoder(bytes.NewReader(data))
	insidePrimary := false
	result := rpmPrimary{}
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return rpmPrimary{}, fmt.Errorf("invalid repomd.xml: %w", err)
		}
		switch element := token.(type) {
		case xml.StartElement:
			if element.Name.Local == "data" {
				insidePrimary = xmlAttribute(element, "type") == "primary"
			} else if insidePrimary && element.Name.Local == "checksum" {
				result.ChecksumType = strings.ToLower(xmlAttribute(element, "type"))
				if err := decoder.DecodeElement(&result.Checksum, &element); err != nil {
					return rpmPrimary{}, err
				}
				result.Checksum = strings.ToLower(strings.TrimSpace(result.Checksum))
			} else if insidePrimary && element.Name.Local == "location" {
				result.Path = xmlAttribute(element, "href")
			}
		case xml.EndElement:
			if element.Name.Local == "data" && insidePrimary {
				insidePrimary = false
			}
		}
	}
	if !safeRepositoryPath(result.Path, false) || !validMetadataChecksum(result.ChecksumType, result.Checksum) {
		return rpmPrimary{}, fmt.Errorf("repomd.xml has no safe primary metadata entry with a SHA256 or SHA512 checksum")
	}
	return result, nil
}

func latestRPMPackage(data []byte, name, architecture string) (rpmPackage, error) {
	decoder := xml.NewDecoder(bytes.NewReader(data))
	var best *rpmPackage
	matchedIdentity := false
	unsupportedChecksums := map[string]struct{}{}
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return rpmPackage{}, fmt.Errorf("invalid primary RPM metadata: %w", err)
		}
		start, ok := token.(xml.StartElement)
		if !ok || start.Name.Local != "package" {
			continue
		}
		candidate, err := decodeRPMPackage(decoder, start)
		if err != nil {
			return rpmPackage{}, err
		}
		if candidate.Name != name || candidate.Architecture != architecture && candidate.Architecture != "noarch" {
			continue
		}
		matchedIdentity = true
		if candidate.Version == "" || !safeRepositoryPath(candidate.Filename, false) {
			continue
		}
		if rpmPackageChecksumLength(candidate.ChecksumType) == 0 {
			kind := strings.TrimSpace(candidate.ChecksumType)
			if kind == "" {
				kind = "missing"
			}
			unsupportedChecksums[kind] = struct{}{}
			continue
		}
		if !validRPMPackageChecksum(candidate.ChecksumType, candidate.Checksum) {
			continue
		}
		if best == nil || rpmVersionCompare(candidate.evr(), best.evr()) > 0 {
			copy := candidate
			best = &copy
		}
	}
	if best == nil {
		if len(unsupportedChecksums) > 0 {
			kinds := make([]string, 0, len(unsupportedChecksums))
			for kind := range unsupportedChecksums {
				kinds = append(kinds, kind)
			}
			sort.Strings(kinds)
			return rpmPackage{}, fmt.Errorf("package %s for architecture %s was found but uses unsupported checksum type: %s",
				name, architecture, strings.Join(kinds, ", "))
		}
		if matchedIdentity {
			return rpmPackage{}, fmt.Errorf("package %s for architecture %s was found but has no usable version, location, or checksum metadata",
				name, architecture)
		}
		return rpmPackage{}, fmt.Errorf("package %s for architecture %s was not found in primary RPM metadata", name, architecture)
	}
	return *best, nil
}

func normalizeRPMPackageChecksumType(kind string) string {
	kind = strings.ToLower(strings.TrimSpace(kind))
	if kind == "sha" {
		return "sha1"
	}
	return kind
}

func rpmPackageChecksumLength(kind string) int {
	switch normalizeRPMPackageChecksumType(kind) {
	case "sha1":
		return sha1.Size * 2
	case "sha256":
		return sha256.Size * 2
	case "sha512":
		return sha512.Size * 2
	default:
		return 0
	}
}

func validRPMPackageChecksum(kind, value string) bool {
	if len(value) != rpmPackageChecksumLength(kind) {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func rpmPackageChecksumLabel(kind string) string {
	switch normalizeRPMPackageChecksumType(kind) {
	case "sha1":
		return "SHA-1"
	case "sha256":
		return "SHA256"
	case "sha512":
		return "SHA512"
	default:
		return strings.ToUpper(strings.TrimSpace(kind))
	}
}

func (s *Service) verifyDownloadedRPMPackageChecksum(sha256Digest, kind, expected string) error {
	file, _, err := s.Artifacts.Store.Open(sha256Digest)
	if err != nil {
		return fmt.Errorf("open downloaded RPM package: %w", err)
	}
	defer file.Close()

	var digest hash.Hash
	switch normalizeRPMPackageChecksumType(kind) {
	case "sha1":
		digest = sha1.New()
	case "sha256":
		digest = sha256.New()
	case "sha512":
		digest = sha512.New()
	default:
		return fmt.Errorf("unsupported RPM package checksum type %q", kind)
	}
	if _, err := io.Copy(digest, file); err != nil {
		return fmt.Errorf("hash downloaded RPM package: %w", err)
	}
	actual := hex.EncodeToString(digest.Sum(nil))
	if !strings.EqualFold(actual, expected) {
		return fmt.Errorf("RPM package %s checksum does not match signed primary metadata",
			rpmPackageChecksumLabel(kind))
	}
	return nil
}

func decodeRPMPackage(decoder *xml.Decoder, start xml.StartElement) (rpmPackage, error) {
	result := rpmPackage{}
	for {
		token, err := decoder.Token()
		if err != nil {
			return rpmPackage{}, err
		}
		switch element := token.(type) {
		case xml.StartElement:
			switch element.Name.Local {
			case "name":
				if err := decoder.DecodeElement(&result.Name, &element); err != nil {
					return rpmPackage{}, err
				}
				result.Name = strings.TrimSpace(result.Name)
			case "arch":
				if err := decoder.DecodeElement(&result.Architecture, &element); err != nil {
					return rpmPackage{}, err
				}
				result.Architecture = strings.TrimSpace(result.Architecture)
			case "version":
				result.Epoch, result.Version, result.Release = xmlAttribute(element, "epoch"), xmlAttribute(element, "ver"), xmlAttribute(element, "rel")
			case "checksum":
				result.ChecksumType = strings.ToLower(xmlAttribute(element, "type"))
				if err := decoder.DecodeElement(&result.Checksum, &element); err != nil {
					return rpmPackage{}, err
				}
				result.Checksum = strings.ToLower(strings.TrimSpace(result.Checksum))
			case "location":
				result.Filename = xmlAttribute(element, "href")
			}
		case xml.EndElement:
			if element.Name.Local == start.Name.Local {
				return result, nil
			}
		}
	}
}

func xmlAttribute(element xml.StartElement, name string) string {
	for _, attribute := range element.Attr {
		if attribute.Name.Local == name {
			return strings.TrimSpace(attribute.Value)
		}
	}
	return ""
}

func validMetadataChecksum(kind, value string) bool {
	length := 0
	if kind == "sha256" {
		length = 64
	}
	if kind == "sha512" {
		length = 128
	}
	if len(value) != length || length == 0 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func metadataChecksum(data []byte, kind string) string {
	if kind == "sha512" {
		digest := sha512.Sum512(data)
		return hex.EncodeToString(digest[:])
	}
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}
