#!/usr/bin/env bash
# Warns about a file that calls esp_http_client_perform() without setting
# config.timeout_ms anywhere in it - this project's own established
# convention (platform/firmware/http_client.cpp's kTimeoutMs, mirroring
# check-curl-timeouts.sh's identical rule for the host/curl backend) is
# that every blocking network call off the connection-loop/UI thread must
# bound it explicitly, so Task::~Task() and reconnect/backoff logic stay
# responsive against an unreachable/black-hole host.
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
    grep -q 'esp_http_client_perform(' "$f" || continue
    if ! grep -q 'timeout_ms' "$f"; then
        echo "[esp-http-timeout] $f: calls esp_http_client_perform() but sets no config.timeout_ms anywhere in the file:"
        grep -n 'esp_http_client_perform(' "$f" | sed 's/^/    /'
        status=1
    fi
done

exit $status
