package recipe

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"time"
)

const (
	lifecycleMaxBytes = 128 * 1024
	bashPath          = "/usr/bin/bash"
	bashTimeout       = 10 * time.Second
)

var (
	forbiddenLifecycle = []struct {
		re      *regexp.Regexp
		message string
	}{
		{regexp.MustCompile(`(?i)(?:^|[^A-Za-z0-9_])(curl|wget|aria2c|ftp)(?:[^A-Za-z0-9_]|$)`),
			"Network download commands are not allowed"},
		{regexp.MustCompile(`(?i)(?:^|[^A-Za-z0-9_])(apt|apt-get|dpkg|pacman|makepkg)(?:[^A-Za-z0-9_]|$)`),
			"Package-manager recursion is not allowed"},
		{regexp.MustCompile(`(?i)(?:^|[^A-Za-z0-9_])(sudo|pkexec|su)(?:[^A-Za-z0-9_]|$)`),
			"Privilege-elevation commands are not allowed"},
		{regexp.MustCompile(`(?:^|[;&|[:space:]])(eval|source|\.)[[:space:]]`),
			"Dynamic shell evaluation is not allowed"},
		{regexp.MustCompile(`(?i)https?://`),
			"Network URLs are not allowed in lifecycle scripts"},
		{regexp.MustCompile("`|\\$\\("),
			"Command substitution is not allowed in lifecycle scripts"},
	}
	lifecycleFunction = regexp.MustCompile(`(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\s*\{`)
	allowedLifecycle  = map[string]struct{}{
		"pre_install":  {},
		"post_install": {},
		"pre_upgrade":  {},
		"post_upgrade": {},
		"pre_remove":   {},
		"post_remove":  {},
	}
)

type LifecycleValidation struct {
	Passed   bool
	Problems []string
}

func (v LifecycleValidation) Message() string {
	if v.Passed {
		return "Syntax and PacSmith lifecycle policy validation passed."
	}
	return strings.Join(v.Problems, "\n")
}

// ValidateLifecycle checks an Arch .install script with bash -n and the
// PacSmith policy. The script is never sourced or executed as a package.
func ValidateLifecycle(contents string) LifecycleValidation {
	var result LifecycleValidation
	if strings.TrimSpace(contents) == "" {
		result.Problems = append(result.Problems, "Lifecycle script is empty")
		return result
	}
	if len(contents) > lifecycleMaxBytes {
		result.Problems = append(result.Problems, "Lifecycle script exceeds 128 KiB")
	}
	for _, rule := range forbiddenLifecycle {
		if rule.re.MatchString(contents) {
			result.Problems = append(result.Problems, rule.message)
		}
	}

	foundFunction := false
	for _, match := range lifecycleFunction.FindAllStringSubmatch(contents, -1) {
		foundFunction = true
		name := match[1]
		if _, ok := allowedLifecycle[name]; !ok {
			result.Problems = append(result.Problems, "Unsupported lifecycle function: "+name)
		}
	}
	if !foundFunction {
		result.Problems = append(result.Problems, "No Arch lifecycle function was found")
	}

	if info, err := os.Stat(bashPath); err != nil || info.IsDir() || info.Mode()&0o111 == 0 {
		result.Problems = append(result.Problems, "bash is required for syntax-only validation")
	} else if err := bashSyntaxCheck(contents); err != nil {
		result.Problems = append(result.Problems, err.Error())
	}

	result.Problems = uniqueStrings(result.Problems)
	result.Passed = len(result.Problems) == 0
	return result
}

func bashSyntaxCheck(contents string) error {
	tmp, err := os.CreateTemp("", "pacsmith-lifecycle-*")
	if err != nil {
		return syntaxPrepError{}
	}
	name := tmp.Name()
	defer os.Remove(name)

	n, writeErr := tmp.Write([]byte(contents))
	closeErr := tmp.Close()
	if writeErr != nil || closeErr != nil || n != len(contents) {
		return syntaxPrepError{}
	}

	ctx, cancel := context.WithTimeout(context.Background(), bashTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, bashPath, "-n", name)
	cmd.Stdin = nil
	cmd.Stdout = nil
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		message := strings.TrimSpace(stderr.String())
		if message == "" {
			message = "bash syntax validation failed"
		}
		return syntaxFailError{message: message}
	}
	return nil
}

type syntaxPrepError struct{}

func (syntaxPrepError) Error() string {
	return "Could not prepare lifecycle script for syntax validation"
}

type syntaxFailError struct {
	message string
}

func (e syntaxFailError) Error() string {
	return e.message
}

func uniqueStrings(values []string) []string {
	if len(values) < 2 {
		return values
	}
	seen := make(map[string]struct{}, len(values))
	out := make([]string, 0, len(values))
	for _, value := range values {
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	return out
}
