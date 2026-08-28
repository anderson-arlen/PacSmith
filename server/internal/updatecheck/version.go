package updatecheck

import (
	"strconv"
	"strings"
	"unicode"
)

type debVersion struct {
	epoch    uint64
	upstream string
	revision string
}

func splitDebVersion(value string) debVersion {
	result := debVersion{revision: "0"}
	if colon := strings.IndexByte(value, ':'); colon >= 0 {
		result.epoch, _ = strconv.ParseUint(value[:colon], 10, 64)
		value = value[colon+1:]
	}
	if dash := strings.LastIndexByte(value, '-'); dash >= 0 {
		result.upstream = value[:dash]
		result.revision = value[dash+1:]
	} else {
		result.upstream = value
	}
	return result
}

func debianVersionCompare(left, right string) int {
	lhs, rhs := splitDebVersion(left), splitDebVersion(right)
	if lhs.epoch < rhs.epoch {
		return -1
	}
	if lhs.epoch > rhs.epoch {
		return 1
	}
	if compared := compareDebPart(lhs.upstream, rhs.upstream); compared != 0 {
		return compared
	}
	return compareDebPart(lhs.revision, rhs.revision)
}

func debOrder(value byte) int {
	if value == '~' {
		return -1
	}
	if value == 0 || value >= '0' && value <= '9' {
		return 0
	}
	if value >= 'A' && value <= 'Z' || value >= 'a' && value <= 'z' {
		return int(value)
	}
	return int(value) + 256
}

func compareDebPart(left, right string) int {
	li, ri := 0, 0
	for li < len(left) || ri < len(right) {
		for li < len(left) && !isDigit(left[li]) || ri < len(right) && !isDigit(right[ri]) {
			var lc, rc byte
			if li < len(left) {
				lc = left[li]
			}
			if ri < len(right) {
				rc = right[ri]
			}
			if debOrder(lc) != debOrder(rc) {
				return sign(debOrder(lc) - debOrder(rc))
			}
			if li < len(left) {
				li++
			}
			if ri < len(right) {
				ri++
			}
		}
		for li < len(left) && left[li] == '0' {
			li++
		}
		for ri < len(right) && right[ri] == '0' {
			ri++
		}
		le, re := li, ri
		for le < len(left) && isDigit(left[le]) {
			le++
		}
		for re < len(right) && isDigit(right[re]) {
			re++
		}
		if le-li != re-ri {
			return sign((le - li) - (re - ri))
		}
		if left[li:le] < right[ri:re] {
			return -1
		}
		if left[li:le] > right[ri:re] {
			return 1
		}
		li, ri = le, re
	}
	return 0
}

type rpmEVR struct {
	epoch            uint64
	version, release string
}

func splitRPMVersion(value string) rpmEVR {
	result := rpmEVR{}
	if colon := strings.IndexByte(value, ':'); colon >= 0 {
		result.epoch, _ = strconv.ParseUint(value[:colon], 10, 64)
		value = value[colon+1:]
	}
	if dash := strings.LastIndexByte(value, '-'); dash >= 0 {
		result.version, result.release = value[:dash], value[dash+1:]
	} else {
		result.version = value
	}
	return result
}

func rpmVersionCompare(left, right string) int {
	lhs, rhs := splitRPMVersion(left), splitRPMVersion(right)
	if lhs.epoch < rhs.epoch {
		return -1
	}
	if lhs.epoch > rhs.epoch {
		return 1
	}
	if compared := compareRPMSegments(lhs.version, rhs.version); compared != 0 {
		return compared
	}
	return compareRPMSegments(lhs.release, rhs.release)
}

func compareRPMSegments(left, right string) int {
	li, ri := 0, 0
	for li < len(left) || ri < len(right) {
		for li < len(left) && !isAlphaNumeric(left[li]) && left[li] != '~' && left[li] != '^' {
			li++
		}
		for ri < len(right) && !isAlphaNumeric(right[ri]) && right[ri] != '~' && right[ri] != '^' {
			ri++
		}
		lt, rt := li < len(left) && left[li] == '~', ri < len(right) && right[ri] == '~'
		if lt || rt {
			if lt != rt {
				if lt {
					return -1
				}
				return 1
			}
			li++
			ri++
			continue
		}
		lc, rc := li < len(left) && left[li] == '^', ri < len(right) && right[ri] == '^'
		if lc || rc {
			if lc != rc {
				if lc && ri >= len(right) {
					return 1
				}
				if rc && li >= len(left) {
					return -1
				}
				if lc {
					return -1
				}
				return 1
			}
			li++
			ri++
			continue
		}
		if li >= len(left) || ri >= len(right) {
			if li >= len(left) && ri >= len(right) {
				return 0
			}
			if li >= len(left) {
				return -1
			}
			return 1
		}
		ln, rn := isDigit(left[li]), isDigit(right[ri])
		if ln != rn {
			if ln {
				return 1
			}
			return -1
		}
		if ln {
			for li < len(left) && left[li] == '0' {
				li++
			}
			for ri < len(right) && right[ri] == '0' {
				ri++
			}
		}
		le, re := li, ri
		if ln {
			for le < len(left) && isDigit(left[le]) {
				le++
			}
			for re < len(right) && isDigit(right[re]) {
				re++
			}
			if le-li != re-ri {
				return sign((le - li) - (re - ri))
			}
		} else {
			for le < len(left) && unicode.IsLetter(rune(left[le])) {
				le++
			}
			for re < len(right) && unicode.IsLetter(rune(right[re])) {
				re++
			}
		}
		if left[li:le] < right[ri:re] {
			return -1
		}
		if left[li:le] > right[ri:re] {
			return 1
		}
		li, ri = le, re
	}
	return 0
}

func isDigit(value byte) bool { return value >= '0' && value <= '9' }
func isAlphaNumeric(value byte) bool {
	return isDigit(value) || value >= 'A' && value <= 'Z' || value >= 'a' && value <= 'z'
}
func sign(value int) int {
	if value < 0 {
		return -1
	}
	if value > 0 {
		return 1
	}
	return 0
}
