#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

check() {
    local tag=$1 expected_display=$2 expected_arch=$3
    local versions
    mapfile -t versions < <(bash "$root/scripts/release-version.sh" "$tag")
    [[ ${#versions[@]} -eq 2 ]]
    [[ ${versions[0]} == "$expected_display" ]]
    [[ ${versions[1]} == "$expected_arch" ]]
}

reject() {
    if bash "$root/scripts/release-version.sh" "$1" >/dev/null 2>&1; then
        echo "unexpectedly accepted release tag: $1" >&2
        return 1
    fi
}

check v0.1.0 0.1.0 0.1.0
check v0.1.0-alpha.1 0.1.0-alpha.1 0.1.0alpha1
check v0.1.0-beta.2 0.1.0-beta.2 0.1.0beta2
check v0.1.0-rc.3 0.1.0-rc.3 0.1.0rc3
check v12.34.56 12.34.56 12.34.56

reject 0.1.0
reject v0.1
reject v0.1.0-rc1
reject v0.1.0-preview.1
reject v01.1.0

[[ $(vercmp 0.1.0alpha1 0.1.0beta1) -lt 0 ]]
[[ $(vercmp 0.1.0beta1 0.1.0rc1) -lt 0 ]]
[[ $(vercmp 0.1.0rc1 0.1.0) -lt 0 ]]
