#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: release-version.sh <vMAJOR.MINOR.PATCH[-alpha.N|-beta.N|-rc.N]>" >&2
    exit 2
fi

tag=$1
if [[ ! $tag =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-(alpha|beta|rc)\.(0|[1-9][0-9]*))?$ ]]; then
    echo "invalid release tag: $tag" >&2
    echo "expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-alpha.N, -beta.N, or -rc.N" >&2
    exit 2
fi

major=${BASH_REMATCH[1]}
minor=${BASH_REMATCH[2]}
patch=${BASH_REMATCH[3]}
prerelease_kind=${BASH_REMATCH[5]:-}
prerelease_number=${BASH_REMATCH[6]:-}
display_version=${tag#v}
arch_version="${major}.${minor}.${patch}"
if [[ -n $prerelease_kind ]]; then
    arch_version+="${prerelease_kind}${prerelease_number}"
fi

printf '%s\n%s\n' "$display_version" "$arch_version"
