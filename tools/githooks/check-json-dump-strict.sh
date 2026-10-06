#!/usr/bin/env bash
# Warns about a strict nlohmann::json .dump() (no arguments) in a file that
# also handles bytes that never passed through a JSON parser - mDNS
# instance names/hosts/TXT values (MdnsService, KodiDiscoveredInstance).
# JSON parsing rejects invalid UTF-8, but those bytes come straight off an
# unauthenticated LAN responder, and the default dump() error handler
# throws json::type_error on invalid UTF-8 - std::abort() on firmware,
# where C++ exceptions are compiled out. The fix is
# dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) (see
# kodi_routes.cpp). Heuristic: it keys on the raw-byte source types named
# above, so a new raw source (e.g. a Wi-Fi scan list) needs its type added
# to raw_sources. Only *_routes.cpp is checked: that is where a snapshot is
# serialised into an HTTP response, whereas a module's own outbound JSON-RPC
# is built from already-parsed (valid UTF-8) values. Skips tests/ and
# simulator/. Non-blocking - see pre-commit.
set -uo pipefail

raw_sources='MdnsService|KodiDiscoveredInstance'
status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    case "$f" in *_routes.cpp) ;; *) continue ;; esac
    case "$f" in tests/*|simulator/*) continue ;; esac
    stripped=$(sed -E 's#//.*$##' "$f")
    echo "$stripped" | grep -qE "$raw_sources" || continue
    hits=$(echo "$stripped" | grep -nE '\.dump\(\)')
    if [ -n "$hits" ]; then
        echo "[json-dump-strict] $f: .dump() with the strict default error handler in a file that handles raw mDNS bytes - invalid UTF-8 aborts on firmware; pass error_handler_t::replace:"
        echo "$hits" | sed 's/^/    /'
        status=1
    fi
done

exit $status
