#!/usr/bin/env bash
# Warns about doc narration patterns and \breal\b usage banned by CLAUDE.md's
# Documentation section, and about ADR cross-references whose cited quoted
# text no longer exists in the target ADR. Non-blocking - see pre-commit.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
status=0

narration_patterns=(
    # Covers "confirmed on hardware", "confirmed on real hardware",
    # "confirmed on the K145 reference unit", "confirmed against ...",
    # "confirmed working against/on ..." in one pattern rather than
    # enumerating every phrasing separately.
    'confirmed (on|against|working on|working against) [a-zA-Z0-9]*[[:space:]]*(the )?[a-zA-Z0-9 ]*(hardware|reference unit)'
    'confirmed end.to.end'
    'confirmed manually'
    'confirmed via'
    'earlier attempt'
    'retried [0-9]+/[0-9]+'
)

for f in "$@"; do
    [ -f "$f" ] || continue

    for pat in "${narration_patterns[@]}"; do
        matches=$(grep -niE "$pat" "$f" || true)
        if [ -n "$matches" ]; then
            echo "[narration] $f: possible verification-log narration (CLAUDE.md Documentation section):"
            echo "$matches" | sed 's/^/    /'
            status=1
        fi
    done

    real_matches=$(grep -niE '\breal\b' "$f" || true)
    if [ -n "$real_matches" ]; then
        echo "[wording] $f: '\breal\b' hit - check it isn't the banned 'is real/are real' implementation-status sense (use 'implemented'):"
        echo "$real_matches" | sed 's/^/    /'
        status=1
    fi

    # Bare date parenthetical in prose, e.g. "...done. (2026-07-30)" - git
    # history already carries "when".
    date_matches=$(grep -nE '\([0-9]{4}-[0-9]{2}-[0-9]{2}\)' "$f" || true)
    if [ -n "$date_matches" ]; then
        echo "[narration] $f: parenthetical date stamp in prose:"
        echo "$date_matches" | sed 's/^/    /'
        status=1
    fi

    # Stale ADR cross-references: ADR-NNNN's "quoted text" citations - check
    # the quoted text still appears in the referenced ADR file.
    while IFS= read -r line; do
        adr_num=$(echo "$line" | grep -oE 'ADR-[0-9]{4}' | head -1)
        quoted=$(echo "$line" | grep -oE '"[^"]+"' | head -1)
        [ -n "$adr_num" ] || continue
        [ -n "$quoted" ] || continue
        target=$(find "$repo_root/docs/decisions" -iname "${adr_num}-*.md" 2>/dev/null | head -1)
        if [ -z "$target" ]; then
            echo "[crossref] $f: references $adr_num but no matching file in docs/decisions/"
            status=1
            continue
        fi
        quoted_text=$(echo "$quoted" | tr -d '"')
        if ! grep -qF "$quoted_text" "$target"; then
            echo "[crossref] $f: cites $adr_num $quoted - text not found in $(basename "$target")"
            status=1
        fi
    done < <(grep -nE 'ADR-[0-9]{4}.{0,40}"' "$f" || true)
done

exit $status
