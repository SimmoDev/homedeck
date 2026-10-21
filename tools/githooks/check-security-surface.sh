#!/usr/bin/env bash
# Warns when a staged change adds a network-facing surface - an open Wi-Fi
# access point (`WIFI_AUTH_OPEN`) or another HTTP server (`httpd_start(`) -
# without docs/architecture/security.md also staged. security.md states which
# surfaces are unauthenticated; a new one that leaves it untouched makes that
# statement false. Diff-based, so it only runs from the pre-commit hook.
# Non-blocking - see pre-commit.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
added=$(git -C "$repo_root" diff --cached -U0 -- "$@" | grep -E '^\+[^+]' | grep -E 'WIFI_AUTH_OPEN|httpd_start\(' || true)
[ -n "$added" ] || exit 0

if git -C "$repo_root" diff --cached --name-only | grep -qx 'docs/architecture/security.md'; then
    exit 0
fi

echo "[security-surface] this change adds an open access point or HTTP server; update"
echo "    docs/architecture/security.md (which surfaces are unauthenticated) in the same commit:"
echo "$added" | sed 's/^/    /'
exit 1
