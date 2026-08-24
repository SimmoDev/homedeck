#!/usr/bin/env bash
# Warns about a file that calls esp_http_client_perform() without setting
# config.timeout_ms anywhere in the same declare-configure-use block -
# this project's own established convention (platform/firmware/http_client.cpp's
# kTimeoutMs, mirroring check-curl-timeouts.sh's identical rule for the
# host/curl backend) is that every blocking network call off the
# connection-loop/UI thread must bound it explicitly, so Task::~Task()
# and reconnect/backoff logic stay responsive against an unreachable/
# black-hole host.
#
# Scoped per esp_http_client_config_t declaration, not file-wide - a
# file-wide grep would pass even if a *second*, separate config struct in
# the same file never sets timeout_ms, as long as some other unrelated
# config elsewhere in the file happens to. Every esp_http_client_config_t
# declaration line resets the "seen timeout_ms" state; an
# esp_http_client_perform() call checks whether timeout_ms was set since
# the most recent declaration before it - not truly variable-name-aware
# (real C++ parsing would be needed for that), but matches this
# codebase's own established declare-configure-use-per-function style
# (see http_client.cpp), which is what actually matters here.
#
# esp_websocket_client_start() is deliberately NOT checked here, unlike
# esp_http_client_perform() - its own config struct's network_timeout_ms
# already defaults to a bounded 10s per esp_websocket_client.h's own doc
# comment (unlike libcurl's multi-minute default, the reason
# check-curl-timeouts.sh exists at all), so relying on that default isn't
# the same unbounded-block risk this check exists to catch. Flagging it
# anyway would just create permanent noise against
# platform/firmware/websocket_client.cpp's own correct use of that
# default. Non-blocking - see pre-commit.
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    # Trailing `//` comments stripped first - a prose mention of
    # esp_http_client_perform() (e.g. explaining what it does) must not
    # count as an actual call, and a comment mentioning timeout_ms must
    # not count as actually setting it. Still not real parsing, so a
    # /* */ block comment isn't handled the same way - same tradeoff
    # check-curl-timeouts.sh's identical strip already accepts.
    stripped=$(sed -E 's#//.*$##' "$f")
    echo "$stripped" | grep -q 'esp_http_client_perform(' || continue

    matches=$(echo "$stripped" | awk -v file="$f" '
        /esp_http_client_config_t[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=/ { seen_timeout = 0 }
        /timeout_ms/ { seen_timeout = 1 }
        /esp_http_client_perform\(/ {
            if (!seen_timeout) {
                print file ":" NR ": esp_http_client_perform() with no config.timeout_ms set since the last esp_http_client_config_t declaration"
            }
        }
    ' || true)
    if [ -n "$matches" ]; then
        echo "[esp-http-timeout] $matches"
        status=1
    fi
done

exit $status
