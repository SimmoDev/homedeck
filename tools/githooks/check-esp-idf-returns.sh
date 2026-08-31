#!/usr/bin/env bash
# Warns about esp_*/mdns_*/httpd_* calls whose return value is neither
# checked, assigned, nor wrapped in ESP_ERROR_CHECK - a bug class with no
# host-test coverage in firmware/main and src/platform/firmware, since
# both are 100% ESP-IDF-coupled and unreachable from tests/. Non-blocking
# - see pre-commit.
set -uo pipefail

status=0

# A handful of esp_*/mdns_* calls in this prefix set return void or are
# noreturn - there is no value to check, assign, or wrap, so a bare call
# is the only way to call them. Filtered out of all three passes below so
# they don't produce a finding no one can action. Extend as new ones turn
# up; keep it to functions genuinely declared void/noreturn in ESP-IDF.
void_returning='(esp_restart|esp_chip_info|esp_deep_sleep_start|esp_system_abort|mdns_query_results_free|mdns_free)\('

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
        | grep -vE "$void_returning" \
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

    # The single-line pass above is blind to a call whose own argument list
    # spans multiple lines: its start line has an unclosed `(`, so it can
    # never match the per-line regex's `.*\);...$` tail on its own. This
    # pass only starts a join from a line that is BOTH a genuine
    # statement-boundary call start (same (^|[{};]) requirement as above,
    # so an ESP_ERROR_CHECK(-wrapped or assigned multi-line call is still
    # correctly excluded) AND has more '(' than ')' on that single line -
    # i.e. an actually-unclosed call, not just a line whose trailing
    # punctuation happens not to match the single-line pass's own closing
    # shape (e.g. a lambda assigned to a variable, `cb = [](){ f(); };`,
    # is already fully balanced on one line and must never be treated as
    # multi-line, or the join would run away consuming the rest of the
    # file looking for a close that will never come). It then appends
    # subsequent lines until parens balance, and only reports if the
    # balanced result's own trailing shape confirms a bare, terminated
    # statement (mirroring the single-line pass's own closing check, with
    # an optional trailing `;` to also cover a `};`-closed lambda body) -
    # silently drops anything else (e.g. the call's result feeding into a
    # further expression) rather than risk a false positive.
    ml_matches=$(awk '
        { lines[NR] = $0 }
        END {
            n = NR
            i = 1
            while (i <= n) {
                line = lines[i]
                n_open_start = gsub(/\(/, "(", line)
                n_close_start = gsub(/\)/, ")", line)
                if (line ~ /(^|[{};])[[:space:]]*(esp_|mdns_|httpd_)[A-Za-z0-9_]+\(/ && n_open_start > n_close_start) {
                    combined = line
                    start = i
                    j = i
                    balanced = 0
                    while (1) {
                        n_open = gsub(/\(/, "(", combined)
                        n_close = gsub(/\)/, ")", combined)
                        if (n_close >= n_open) { balanced = 1; break }
                        j++
                        if (j > n) break
                        combined = combined " " lines[j]
                    }
                    if (balanced && combined ~ /\);[[:space:]]*\}?[[:space:]]*;?[[:space:]]*$/ && combined !~ /httpd_resp_/) {
                        print start ": " combined
                    }
                    i = j
                }
                i++
            }
        }
    ' "$f" | grep -vE "$void_returning" || true)
    if [ -n "$ml_matches" ]; then
        echo "[esp-idf-return] $f: bare multi-line call, return value not checked/logged:"
        echo "$ml_matches" | sed 's/^/    /'
        status=1
    fi

    # Neither pass above sees a call as the unbraced body of an
    # if/while/for statement (`if (ready) esp_wifi_connect();`) - it
    # follows the condition's closing `)`, not a `{`/`;`/`}` boundary or
    # line start. Walks the line by character to find the condition's own
    # balanced close (so a for-loop's internal `;`s, e.g.
    # `for (int i = 0; i < 3; i++) esp_foo();`, don't confuse where the
    # condition actually ends), then checks whether what follows is a
    # bare call with the same closing shape and paren-balance guard as
    # the single-line pass above.
    cf_matches=$(awk '
        {
            line = $0
            if (line ~ /^[[:space:]]*(if|while|for)[[:space:]]*\(/) {
                pos = index(line, "(")
                depth = 0
                n = length(line)
                close_pos = 0
                for (k = pos; k <= n; k++) {
                    c = substr(line, k, 1)
                    if (c == "(") depth++
                    else if (c == ")") {
                        depth--
                        if (depth == 0) { close_pos = k; break }
                    }
                }
                if (close_pos > 0) {
                    rest = substr(line, close_pos + 1)
                    sub(/^[[:space:]]+/, "", rest)
                    if (rest ~ /^(esp_|mdns_|httpd_)[A-Za-z0-9_]+\(.*\);[[:space:]]*$/ && rest !~ /httpd_resp_/) {
                        n_open = gsub(/\(/, "(", rest)
                        n_close = gsub(/\)/, ")", rest)
                        if (n_close <= n_open) print NR ": " line
                    }
                }
            }
        }
    ' "$f" | grep -vE "$void_returning" || true)
    if [ -n "$cf_matches" ]; then
        echo "[esp-idf-return] $f: bare call as an unbraced if/while/for body, return value not checked/logged:"
        echo "$cf_matches" | sed 's/^/    /'
        status=1
    fi
done

exit $status
