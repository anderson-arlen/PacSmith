package repo

import (
	"bytes"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
)

func CompareVersions(a, b string) (int, error) {
	a = strings.TrimSpace(a)
	b = strings.TrimSpace(b)
	if a == b {
		return 0, nil
	}
	cmd := exec.Command("/usr/bin/vercmp", a, b)
	out, err := cmd.Output()
	if err != nil {
		return 0, fmt.Errorf("vercmp: %w", err)
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(out)))
	if err != nil {
		return 0, fmt.Errorf("vercmp output %q: %w", bytes.TrimSpace(out), err)
	}
	if n < 0 {
		return -1, nil
	}
	if n > 0 {
		return 1, nil
	}
	return 0, nil
}

func VersionString(epoch int64, pkgver, pkgrel string) string {
	ver := pkgver
	if strings.TrimSpace(pkgrel) != "" {
		ver += "-" + pkgrel
	}
	if epoch > 0 {
		return strconv.FormatInt(epoch, 10) + ":" + ver
	}
	return ver
}

func Advances(candidateEpoch int64, candidateVer, candidateRel string, stableEpoch int64, stableVer, stableRel string) (bool, error) {
	if stableVer == "" {
		return true, nil
	}
	cmp, err := CompareVersions(
		VersionString(candidateEpoch, candidateVer, candidateRel),
		VersionString(stableEpoch, stableVer, stableRel),
	)
	if err != nil {
		return false, err
	}
	return cmp > 0, nil
}
