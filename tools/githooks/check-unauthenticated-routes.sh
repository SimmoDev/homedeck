#!/usr/bin/env bash
# Warns about a server.RegisterHandler(...) call with no RequireAuth(...)
# wrapper nearby - every route in this codebase is expected to route
# through AdminAuthService::RequireAuth() (src/core/admin_auth_service.h)
# unless it's one of the deliberately-public exceptions below. The
# existing safety net for this is "the pattern is copy-pasted correctly
# and each route's own *_routes_test.cpp happens to assert it" - this
# check exists so a new route skipping RequireAuth by mistake is caught
# even before its own test (if any) is written. Non-blocking - see
# pre-commit.
set -uo pipefail

status=0

# The auth routes themselves (they ARE the auth system - status/setup/
# login can't require a session that doesn't exist yet) and static asset
# serving (must be reachable pre-login to serve the login page itself -
# see ServeStaticFiles's own comment) are the only known-legitimate
# unauthenticated RegisterHandler call sites in this codebase. tests/ is
# exempt entirely - test fixtures exercise the raw HttpServer API
# directly with throwaway handlers, not production routes.
is_exempt_file() {
    case "$1" in
        */admin_auth_service.cpp|*/static_assets.cpp|tests/*) return 0 ;;
        *) return 1 ;;
    esac
}

for f in "$@"; do
    [ -f "$f" ] || continue
    is_exempt_file "$f" && continue

    # For every *call* to RegisterHandler( (a preceding '.', not '::' -
    # excludes HttpServer's own RegisterHandler *definition* in
    # firmware/host http_server.cpp), look for RequireAuth within the
    # next 5 lines (covers this codebase's multi-line call style - see
    # e.g. settings_routes.cpp) - a plain sliding-window text search, not
    # real C++ parsing, so it can't perfectly attribute RequireAuth to
    # the *matching* RegisterHandler call in a file with several close
    # together, but that shape doesn't occur in this codebase today.
    matches=$(awk -v file="$f" '
        /[a-zA-Z_][a-zA-Z0-9_]*\.RegisterHandler\(/ {
            pending_line = NR
            window = 5
        }
        window > 0 {
            if ($0 ~ /RequireAuth/) { window = 0; pending_line = 0 }
            else window--
            if (window == 0 && pending_line != 0) {
                print file ":" pending_line ": RegisterHandler with no RequireAuth() found within 5 lines"
                pending_line = 0
            }
        }
    ' "$f" || true)
    if [ -n "$matches" ]; then
        echo "[unauthenticated-route] $matches"
        status=1
    fi
done

exit $status
