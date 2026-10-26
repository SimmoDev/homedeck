#!/usr/bin/env bash
# Warns when a staged change adds a network-facing surface - an open Wi-Fi
# access point (`WIFI_AUTH_OPEN`) or another HTTP server (`httpd_start(`) -
# without docs/architecture/security.md also staged. security.md states which
# surfaces are unauthenticated; a new one that leaves it untouched makes that
# statement false. Diff-based: the pre-commit hook checks the staged change;
# with HOMEDECK_DIFF_RANGE set (e.g. origin/main...HEAD) CI checks that
# commit range instead. Non-blocking - see pre-commit.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
diff_args=(--cached)
[ -z "${HOMEDECK_DIFF_RANGE:-}" ] || diff_args=("$HOMEDECK_DIFF_RANGE")

added=$(git -C "$repo_root" diff "${diff_args[@]}" -U0 -- "$@" | grep -E '^\+[^+]' | grep -E 'WIFI_AUTH_OPEN|httpd_start\(' || true)
[ -n "$added" ] || exit 0

if git -C "$repo_root" diff "${diff_args[@]}" --name-only | grep -qx 'docs/architecture/security.md'; then
    exit 0
fi

echo "[security-surface] this change adds an open access point or HTTP server; update"
echo "    docs/architecture/security.md (which surfaces are unauthenticated) in the same commit:"
echo "$added" | sed 's/^/    /'
exit 1
