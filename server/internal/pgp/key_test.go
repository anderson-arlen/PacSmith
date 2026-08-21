package pgp

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestFingerprintsFromArmoredKey(t *testing.T) {
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("caller")
	}
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(file), "testdata", "testkey.asc"))
	if err != nil {
		t.Fatal(err)
	}
	got, err := Fingerprints(raw)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"6ADEB4C281928E350773C72917720DADE33935CF",
		"9D9287D9D63C54A22E5248029E0828FEFFB1330B",
	}
	if len(got) != len(want) {
		t.Fatalf("fingerprints %v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("fingerprint %d: %s want %s", i, got[i], want[i])
		}
	}
	normalized, err := Normalize(raw)
	if err != nil {
		t.Fatal(err)
	}
	again, err := Fingerprints(normalized)
	if err != nil {
		t.Fatal(err)
	}
	if again[0] != want[0] {
		t.Fatalf("binary round-trip %v", again)
	}
}
