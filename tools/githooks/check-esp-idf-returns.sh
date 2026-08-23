#!/usr/bin/env bash
# Warns about esp_*/mdns_*/httpd_* calls whose return value is neither
# checked, assigned, nor wrapped in ESP_ERROR_CHECK - a bug class with no
# host-test coverage in firmware/main and src/platform/firmware, since
# both are 100% ESP-IDF-coupled and unreachable from tests/. Non-blocking
# - see pre-commit.
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    # httpd_resp_* (response-writing calls) are excluded: checking every one
    # of those would fire on nearly every HTTP handler in the codebase by
    # design - a categorically different, permanently-noisy class from the
    # state-changing/scheduling calls this check targets. A line with more
    # ')' than '(' is the closing line of some other multi-line call (e.g.
    # an ESP_LOGE(...) argument), not a bare statement of its own - filtered
    # via awk since it isn't the call this check is looking for. The call
    # may follow a genuine line start OR a `{`/`;`/`}` statement boundary -
    # not just line start - and may be followed by a single closing `}` -
    # not just end of line - so a call embedded in a one-line function/
    # lambda body (e.g. `void Foo() { esp_wifi_connect(); }`) isn't
    # invisible to this check just because something else shares its line;
    # a call preceded by anything else (`=`, `(`, `if (`, etc.) means its
    # result is actually being used, which is exactly what this check must
    # not flag.
    matches=$(grep -nE '(^|[{};])[[:space:]]*(esp_|mdns_|httpd_)[A-Za-z0-9_]+\(.*\);[[:space:]]*\}?[[:space:]]*$' "$f" \
        | grep -vE 'httpd_resp_' \
        | awk -F: '{
            line = $0
            sub(/^[0-9]+:/, "", line)
            n_open = gsub(/\(/, "(", line)
            n_close = gsub(/\)/, ")", line)
            if (n_close <= n_open) print $0
          }' || true)
    if [ -n "$matches" ]; then
        echo "[esp-idf-return] $f: bare call, return value not checked/logged:"
        echo "$matches" | sed 's/^/    /'
        status=1
    fi
done

exit $status
