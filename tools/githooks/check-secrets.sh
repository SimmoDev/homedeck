#!/usr/bin/env bash
# Warns about likely-committed secrets: private key blocks, cloud
# credential formats, common API-token shapes, and .env files. A
# lightweight pattern scan, not a full secret-scanning tool (gitleaks/
# trufflehog) - matching this project's existing warn-only hook
# philosophy (see pre-commit). Deliberately does not flag plausible-
# looking passwords/tokens in ordinary key=value form - tests/*_test.cpp
# deliberately uses fake credentials (e.g. "correct horse battery" in
# harmony_routes_test.cpp) that would otherwise be a permanent false-
# positive source; the specific formats below don't have that ambiguity.
# Blocking - see pre-commit (the only one of that hook's checks that
# is) and .github/workflows/secrets.yml (CI's backstop for clones that
# skip local hook setup or use --no-verify).
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue

    base="$(basename "$f")"
    case "$base" in
        .env|.env.*)
            if [ "$base" != ".env.example" ]; then
                echo "[secrets] $f: a .env file is staged - verify it holds no real credentials before committing"
                status=1
            fi
            continue
            ;;
    esac

    if grep -qE -- '-----BEGIN (RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----' "$f"; then
        echo "[secrets] $f: contains a private key block"
        status=1
    fi
    if grep -qE 'AKIA[0-9A-Z]{16}' "$f"; then
        echo "[secrets] $f: contains what looks like an AWS access key ID"
        status=1
    fi
    if grep -qE 'xox[baprs]-[0-9A-Za-z-]{10,}' "$f"; then
        echo "[secrets] $f: contains what looks like a Slack token"
        status=1
    fi
    if grep -qE 'ghp_[0-9A-Za-z]{36}' "$f"; then
        echo "[secrets] $f: contains what looks like a GitHub personal access token"
        status=1
    fi
done

exit $status
