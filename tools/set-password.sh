#!/usr/bin/env bash
# Sets the Web UI admin password on a freshly-erased device (see
# ADR-0007's first-login-sets-the-password flow). Only works once -
# there's no password-change flow yet, so a device that already has one
# set needs tools/factory-reset.sh first.
#
# Usage: tools/set-password.sh [host]  (default: homedeck.local)
set -euo pipefail

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required." >&2
  exit 1
fi

HOST="${1:-homedeck.local}"

STATUS=$(curl -sf "http://$HOST/api/auth/status")
if echo "$STATUS" | grep -q '"passwordSet":true'; then
  echo "A password is already set on $HOST - there's no change flow yet (ADR-0007)." >&2
  echo "Run tools/factory-reset.sh then tools/flash.sh first if you need to reset it." >&2
  exit 1
fi

read -r -s -p "New admin password for $HOST: " PASSWORD
echo

RESPONSE=$(curl -s -X POST "http://$HOST/api/auth/setup" \
  -H "Content-Type: application/json" \
  -d "$(jq -n --arg password "$PASSWORD" '{password: $password}')")

if echo "$RESPONSE" | grep -q '"ok":true'; then
  echo "Password set."
else
  echo "Failed: $RESPONSE" >&2
  exit 1
fi
