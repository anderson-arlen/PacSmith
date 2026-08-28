package updatecheck

import "testing"

func TestDebianVersionComparison(t *testing.T) {
	tests := []struct {
		left, right string
		want        int
	}{
		{"1.0", "1.0", 0},
		{"1:1.0", "2.0", 1},
		{"1.0~rc1", "1.0", -1},
		{"1.0-2", "1.0-10", -1},
		{"2.0+vendor1", "2.0+vendor0", 1},
	}
	for _, test := range tests {
		if got := debianVersionCompare(test.left, test.right); got != test.want {
			t.Errorf("compare %q %q = %d, want %d", test.left, test.right, got, test.want)
		}
	}
}

func TestRPMVersionComparison(t *testing.T) {
	tests := []struct {
		left, right string
		want        int
	}{
		{"1.0", "1.0", 0},
		{"1:1.0-1", "2.0-1", 1},
		{"1.0~rc1", "1.0", -1},
		{"1.0^git1", "1.0", 1},
		{"1.0-10", "1.0-2", 1},
	}
	for _, test := range tests {
		if got := rpmVersionCompare(test.left, test.right); got != test.want {
			t.Errorf("compare %q %q = %d, want %d", test.left, test.right, got, test.want)
		}
	}
}
