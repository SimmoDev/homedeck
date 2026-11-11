#!/usr/bin/env bash
# Warns about a code comment that asserts something has not happened yet
# ("no X exists yet", "not yet implemented", "not verified", "unimplemented").
# Such a claim goes stale when the thing lands in another file, and nothing
# else re-reads the comment. Each hit needs a human check against the current
# code and docs: a claim that is still true is reworded to state the current
# fact without "yet", and one that is false is corrected. Only comments are
# scanned. Also flags "Removed once ..." deferrals, whose condition is rarely
# revisited: state the thing as lasting, or tie it to a roadmap item.
# Non-blocking - see pre-commit.
set -uo pipefail

pattern='exists? yet|not yet (verified|implemented|built|tested|supported)|not verified|unverified|unimplemented|named-but|[Rr]emoved once'
status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    hits=$(grep -nE "(//|^[[:space:]]*\*|^[[:space:]]*#).*($pattern)" "$f" || true)
    [ -n "$hits" ] || continue
    echo "[stale-claim] $f: comment asserts something is absent, unverified or due for removal - check it against the current code:"
    echo "$hits" | cut -c1-160 | sed 's/^/    /'
    status=1
done

exit $status
