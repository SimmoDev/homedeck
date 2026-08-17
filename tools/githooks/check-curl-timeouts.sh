#!/usr/bin/env bash
# Warns about a file that calls curl_easy_perform() without setting
# CURLOPT_TIMEOUT or CURLOPT_CONNECTTIMEOUT(_MS) anywhere in it - libcurl's
# undocumented-in-code default connect timeout is minutes long, and this
# codebase's own convention (see platform/host/http_client.cpp's
# kTimeoutSeconds comment) is that every curl handle doing blocking I/O off
# the connection-loop/UI thread must bound it explicitly, so Task::~Task()
# and reconnect/backoff logic stay responsive. Non-blocking - see pre-commit.
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    grep -q 'curl_easy_perform(' "$f" || continue
    if ! grep -qE 'CURLOPT_(CONNECT)?TIMEOUT' "$f"; then
        echo "[curl-timeout] $f: calls curl_easy_perform() but sets no CURLOPT_TIMEOUT/CURLOPT_CONNECTTIMEOUT anywhere in the file:"
        grep -n 'curl_easy_perform(' "$f" | grep -vE '^\s*[0-9]+:\s*//' | sed 's/^/    /'
        status=1
    fi
done

exit $status
