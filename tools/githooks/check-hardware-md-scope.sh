#!/usr/bin/env bash
# Warns when docs/architecture/hardware.md drifts past its documented scope
# (electrical/physical facts only - chips, pins, register addresses,
# confirmed signal behavior) into software implementation detail: citing a
# specific function/API name, or narrating a debugging investigation rather
# than stating its resulting fact. Non-blocking - see pre-commit.
#
# This doc has repeatedly drifted back into scope violations after manual
# sweeps that didn't hold across later edits, which is why this check is
# automated rather than relying on review alone. A function name cited
# here can also go stale/wrong (a symbol that doesn't exist in the
# vendored header), which a pure electrical-fact statement can't do. A
# src/core/ or platform/ header path with no parenthesized call is a
# separate violation shape the function-citation check below can't see
# on its own - see the src/core|platform path check below.
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

    # Core/platform source-file path citations (no parenthesized call
    # needed to be a violation, unlike the function-citation check above -
    # e.g. `src/core/url_codec.h`). Scoped to src/core/ and src/platform/
    # specifically, not firmware/components/ - a vendored BSP header cited
    # for a pin/register constant (e.g. confirming a GPIO number) is a
    # legitimate electrical-fact citation this file already makes
    # elsewhere; a src/core|platform path is Core/platform software by
    # definition (see CLAUDE.md's Core responsibilities), so citing one
    # here is always out of scope.
    path_matches=$(grep -noE '`?src/(core|platform)/[A-Za-z0-9_./-]*\.(h|hpp|cpp|c)`?' "$f" || true)
    if [ -n "$path_matches" ]; then
        echo "[hardware-md-scope] $f: src/core or src/platform file citation (hardware.md is facts-only, not software behavior):"
        echo "$path_matches" | sed 's/^/    /'
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
