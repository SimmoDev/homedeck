#!/usr/bin/env bash
# Warns about a comment that cites a repo file path no tracked file has: a
# path with a directory component ending in a source/doc extension, or a
# bare `*.md` name. A file that moves or is renamed leaves such citations
# pointing nowhere, and nothing else checks comments the way the link check
# covers Markdown. System and library headers are ignored (#include lines,
# and tokens such as `esp_log.h` that are bare names, not repo paths).
# Non-blocking - see pre-commit.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
tracked=$(git -C "$repo_root" ls-files)
status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    while IFS=: read -r lineno token; do
        [ -n "$token" ] || continue
        case "$token" in
            http*|../*|*'*'*) continue ;;
            # "a.sh/b.sh" lists alternatives, not one path.
            *.[a-z]*/*) continue ;;
            # A directory component that is not a top-level repo directory is
            # a library or system header path (curl/curl.h), not a repo path.
            */*) case "${token%%/*}" in
                     .github|docs|firmware|hardware|simulator|src|tests|tools|webui) ;;
                     *) continue ;;
                 esac ;;
        esac
        # A path with a directory component must match the end of a tracked
        # path; a bare .md name must match some tracked file's basename.
        if ! grep -qF -- "$token" <<<"$tracked"; then
            echo "[comment-path] $f:$lineno: '$token' is not a tracked file"
            status=1
        fi
    done < <(
        grep -nE '(//|#|\*|--)' "$f" | grep -vE '^[0-9]+:\s*#\s*include' |
            grep -oE '^[0-9]+:|(^|[^A-Za-z0-9_./-])([A-Za-z0-9_.-]+/)+[A-Za-z0-9_.-]+\.(cpp|h|hpp|ts|svelte|sh|md|yml|cmake)|(^|[^A-Za-z0-9_./-])[A-Za-z0-9]([A-Za-z0-9_-]*[A-Za-z0-9])?\.md' |
            awk '/^[0-9]+:$/ {split($0,a,":"); ln=a[1]; next} {t=$0; sub(/^[^A-Za-z0-9_.-]/,"",t); print ln ":" t}'
    )
done

exit $status
