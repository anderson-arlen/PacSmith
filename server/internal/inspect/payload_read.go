package inspect

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

var errStopWalk = errors.New("payload file found")

// ReadPayloadFile returns one regular file from a vendor artifact. The member
// path must be a normalized archive path. Contents are capped at maxBytes.
func ReadPayloadFile(archivePath, member string, maxBytes int) ([]byte, error) {
	want, ok := NormalizedArchivePath(member)
	if !ok || want == "" {
		return nil, fmt.Errorf("invalid payload path")
	}
	if maxBytes <= 0 {
		maxBytes = maxIconBytes
	}
	sourceType, err := Detect(archivePath)
	if err != nil {
		return nil, err
	}
	collector := payloadFileCollector{want: want, max: maxBytes}
	switch sourceType {
	case SourceDebian:
		err = walkDebData(archivePath, collector.walk)
	case SourceRPM:
		err = walkRPMFiles(archivePath, collector.walk)
	case SourceArchive, SourceArchPackage:
		err = walkArchiveFiles(archivePath, collector.walk)
	case SourceAppImage:
		err = walkAppImageFiles(archivePath, collector.walk)
	default:
		return nil, fmt.Errorf("cannot read payload files from this artifact type")
	}
	if err != nil && !errors.Is(err, errStopWalk) {
		return nil, err
	}
	if len(collector.data) == 0 {
		return nil, fmt.Errorf("payload file %s was not found", want)
	}
	return collector.data, nil
}

type payloadFileCollector struct {
	want string
	max  int
	data []byte
}

func (c *payloadFileCollector) walk(entry walkedEntry, body io.Reader) error {
	path, ok := NormalizedArchivePath(entry.RawName)
	if !ok || path != c.want || entry.Kind != kindFile {
		_, _ = io.Copy(io.Discard, body)
		return nil
	}
	data, err := readLimited(body, c.max)
	if err != nil {
		return err
	}
	c.data = data
	return errStopWalk
}

func walkDebData(path string, fn func(walkedEntry, io.Reader) error) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	reader, err := newArReader(file)
	if err != nil {
		return err
	}
	tmpDir, err := os.MkdirTemp("", "pacsmith-deb-icon-*")
	if err != nil {
		return fmt.Errorf("Could not create temporary archive files")
	}
	defer os.RemoveAll(tmpDir)
	dataPath := filepath.Join(tmpDir, "data.tar")
	foundData := false
	for {
		hdr, err := reader.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		name := strings.TrimSuffix(hdr.Name, "/")
		if strings.HasPrefix(name, "data.tar") && !foundData {
			if err := writeMember(dataPath, reader); err != nil {
				return err
			}
			foundData = true
		}
	}
	if !foundData {
		return fmt.Errorf("Not a supported Debian binary package (missing data.tar.*)")
	}
	return walkTarFile(dataPath, "data.tar", fn)
}

func walkRPMFiles(path string, fn func(walkedEntry, io.Reader) error) error {
	header, err := analyzeRPMHeader(path)
	if err != nil {
		return err
	}
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	if header.payloadOffset > 0 {
		if _, err := file.Seek(header.payloadOffset, io.SeekStart); err != nil {
			return err
		}
	}
	decoded, err := decodePayload(file, header.payloadCompressor)
	if err != nil {
		return err
	}
	defer decoded.Close()
	if strings.ToLower(header.payloadFormat) == "tar" {
		return walkTarReader(decoded, fn)
	}
	return walkCPIO(decoded, fn)
}

func walkArchiveFiles(path string, fn func(walkedEntry, io.Reader) error) error {
	if err := walkTarFile(path, path, fn); err == nil || errors.Is(err, errStopWalk) {
		return err
	}
	return walkZipFile(path, fn)
}

func walkAppImageFiles(path string, fn func(walkedEntry, io.Reader) error) error {
	unsquashfs, err := exec.LookPath("unsquashfs")
	if err != nil {
		return fmt.Errorf("Type 2 AppImage inspection requires /usr/bin/unsquashfs from Arch's squashfs-tools package")
	}
	offset, err := appImageSquashfsOffset(path)
	if err != nil {
		return err
	}
	directory, err := os.MkdirTemp("", "pacsmith-appimage-icon-*")
	if err != nil {
		return fmt.Errorf("Could not create a private AppImage inspection directory")
	}
	defer os.RemoveAll(directory)
	extract := exec.Command(unsquashfs, "-no-progress", "-no-xattrs",
		"-o", fmt.Sprintf("%d", offset), "-d", directory, path)
	if out, err := extract.CombinedOutput(); err != nil {
		return fmt.Errorf("Static AppImage extraction failed: %s", strings.TrimSpace(string(out)))
	}
	return filepath.WalkDir(directory, func(full string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			if os.IsPermission(walkErr) {
				return nil
			}
			return walkErr
		}
		if full == directory {
			return nil
		}
		rel, err := filepath.Rel(directory, full)
		if err != nil {
			return err
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		entry := walkedEntry{
			RawName: filepath.ToSlash(rel),
			Size:    info.Size(),
		}
		mode := info.Mode()
		switch {
		case mode.IsDir():
			entry.Kind = kindDir
			return fn(entry, bytesNewReader(nil))
		case mode&os.ModeSymlink != 0:
			entry.Kind = kindSymlink
			return fn(entry, bytesNewReader(nil))
		case mode.IsRegular():
			entry.Kind = kindFile
		default:
			entry.Kind = kindOther
			return fn(entry, bytesNewReader(nil))
		}
		file, err := os.Open(full)
		if err != nil {
			return err
		}
		err = fn(entry, file)
		_ = file.Close()
		return err
	})
}
