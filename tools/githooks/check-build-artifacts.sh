#!/usr/bin/env bash
# Warns about generated files that belong in .gitignore: Python bytecode
# (`__pycache__/`, `*.pyc`), object files and log files. A script run from
# a tracked directory writes these next to its source, and `git add -A`
# then commits them. Path-based, so it applies to every file the caller
# passes regardless of content. Non-blocking - see pre-commit.
set -uo pipefail

status=0
for f in "$@"; do
    case "$f" in
        *__pycache__/*|*.pyc|*.pyo|*.o|*.obj|*.log)
            echo "[build-artifact] $f: generated file - add it to .gitignore instead of committing it"
            status=1
            ;;
    esac
done

exit $status
