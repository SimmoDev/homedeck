#!/usr/bin/env bash
# Warns when docs/architecture/hardware.md drifts past its documented scope
# (electrical/physical facts only - chips, pins, register addresses,
# confirmed signal behavior) into software implementation detail: citing a
# specific function/API name, or narrating a debugging investigation rather
# than stating its resulting fact. Non-blocking - see pre-commit.
#
# This doc has drifted back into scope violations across four consecutive
# milestone exit reviews (M2 pass 9, M2 pass 12, M3 pass 2, M3 pass 10),
# each time fixed by a manual sweep that didn't hold to the next milestone -
# see the feedback-hardware-md-scope memory. A function name cited here has
# also gone stale/wrong at least once (a symbol that didn't exist in the
# vendored header), which a pure electrical-fact statement can't do.
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    case "$f" in
        */hardware.md) : ;;
        *) continue ;;
    esac

    # Function/API-call citations: a snake_case identifier with at least
    # three segments (ESP-IDF/BSP naming convention, e.g. bsp_feature_enable,
    # esp_sleep_enable_ext1_wakeup_io) immediately followed by a
    # parenthesized argument list, with or without surrounding backticks.
    # Two-segment identifiers (e.g. a bare struct-field-shaped name) are
    # deliberately excluded to keep the false-positive rate low - every
    # known violation so far has been three or more segments.
    fn_matches=$(grep -noE '\b[a-z][a-z0-9]*(_[a-z0-9]+){2,}\([^()]*\)' "$f" || true)
    if [ -n "$fn_matches" ]; then
        echo "[hardware-md-scope] $f: function/API name citation (hardware.md is facts-only, not software behavior):"
        echo "$fn_matches" | sed 's/^/    /'
        status=1
    fi

    # Debugging-investigation narrative rather than a stated fact.
    inv_matches=$(grep -niE 'root-cause investigation|exhausted every avenue|see git history for the diagnostic' "$f" || true)
    if [ -n "$inv_matches" ]; then
        echo "[hardware-md-scope] $f: debugging-investigation narrative (state the resulting fact instead):"
        echo "$inv_matches" | sed 's/^/    /'
        status=1
    fi
done

exit $status
