#!/usr/bin/env bash
# Warns about doc narration patterns and \breal\b usage banned by CLAUDE.md's
# Documentation section, and about ADR cross-references whose cited quoted
# text no longer exists in the target ADR. Non-blocking - see pre-commit.
# Narration pattern list (and the false-positive-avoidance rationale behind
# it) lives in lib-narration-patterns.sh, shared with commit-msg.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
status=0

# shellcheck source=lib-narration-patterns.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib-narration-patterns.sh"

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

    # Prose wraps at ~72-80 cols, so some narration phrases are split
    # across a line break and invisible to the line-by-line pass above
    # (e.g. "...ruled out every software\nexplanation" or "...confirmed
    # against the project's own\nreference unit"). Flatten the file to one
    # line and re-check; only report what the line-by-line pass missed, to
    # avoid double-reporting the common case.
    flattened=$(tr '\n' ' ' < "$f")
    for pat in "${narration_patterns[@]}"; do
        if echo "$flattened" | grep -qiE "$pat"; then
            if ! grep -qiE "$pat" "$f"; then
                echo "[narration] $f: possible verification-log narration split across a line wrap (pattern: $pat)"
                status=1
            fi
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

    # One-sided ADR supersession: every other superseded ADR in this repo
    # carries its own "superseded by ADR-NNNN" note in its own Status
    # section (ADR-0002/0005/0006/0009/0010/0014 all do this) - the
    # convention this project actually follows is a reciprocal pointer,
    # not just the new ADR's own "Supersedes ADR-NNNN" claim. Caught once
    # already (M2 exit review, commit dca9456) and recurred once more
    # (ADR-0029 supersedes part of ADR-0003 with no backlink added) -
    # this check is scoped to files under docs/decisions/ only, since
    # "Supersedes" language elsewhere (e.g. a roadmap line describing an
    # ADR) isn't the ADR's own declaration.
    case "$f" in
        docs/decisions/ADR-*.md)
            this_adr=$(basename "$f" | grep -oE 'ADR-[0-9]{4}' | head -1)
            while IFS= read -r line; do
                target_adr=$(echo "$line" | grep -oE 'ADR-[0-9]{4}' | head -1)
                [ -n "$target_adr" ] || continue
                [ "$target_adr" != "$this_adr" ] || continue
                target=$(find "$repo_root/docs/decisions" -iname "${target_adr}-*.md" 2>/dev/null | head -1)
                [ -n "$target" ] || continue
                if ! grep -q "$this_adr" "$target"; then
                    echo "[crossref] $f: claims to supersede $target_adr, but $(basename "$target") has no reciprocal pointer back to $this_adr"
                    status=1
                fi
            done < <(grep -niE 'supersede[sd]?[^.]*ADR-[0-9]{4}' "$f" || true)
            ;;
    esac
done

exit $status
