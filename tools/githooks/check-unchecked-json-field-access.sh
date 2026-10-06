#!/usr/bin/env bash
# Warns about nlohmann::json field extraction that skips a type check
# before converting: .value(key, default) where `default` isn't itself a
# json value (so the call converts to the default's own type, which
# throws json::type_error on a type mismatch, not just on a missing
# key), or .at(key).get<T>() with no is_number()/is_string()/
# is_boolean()/is_object()/is_array() check for that same key anywhere
# else in the file. Firmware builds with C++ exceptions disabled
# (CONFIG_COMPILER_CXX_EXCEPTIONS unset), so that throw is std::abort()
# there - a crash on any external response with an unexpected field
# type, not just a hypothetical, since this project's own modules
# connect to unauthenticated LAN services (ADR-0029/ADR-0030). See
# kodi_client.cpp's GetInt()/GetDouble()/GetString()/GetBool() and
# weather_provider.cpp's is_number() guards for the established fix
# pattern this flags deviation from. Heuristic, not data-flow analysis -
# a same-named key checked in an unrelated object elsewhere in the file
# reads as "guarded" too. Non-blocking - see pre-commit.
set -uo pipefail

status=0

for f in "$@"; do
    [ -f "$f" ] || continue
    stripped=$(sed -E 's#//.*$##' "$f")

    # A default that's itself a json value (nlohmann::json::object()/
    # array(), or anything mentioning nlohmann::json) is an identity
    # conversion that can't throw regardless of the field's actual type -
    # see kodi_client.cpp's MillisFromTimeObject() callers for the
    # established, deliberately-safe exception to this check.
    value_hits=$(echo "$stripped" | grep -nE '(->|\.)value\("[^"]+", *[^){]' |
        grep -vE 'json::(object|array)\(\)|nlohmann::json')
    if [ -n "$value_hits" ]; then
        echo "[json-field-type] $f: .value(key, default) with a non-json default - no type check before the conversion that can throw on a type mismatch:"
        echo "$value_hits" | sed 's/^/    /'
        status=1
    fi

    get_lines=$(echo "$stripped" | grep -noE '(->|\.)at\("[^"]+"\)\.get<')
    if [ -n "$get_lines" ]; then
        while IFS=: read -r lineno _; do
            key=$(sed -n "${lineno}p" <<<"$stripped" | grep -oE '(->|\.)at\("[^"]+"\)\.get<' | head -1 |
                sed -E 's/.*at\("([^"]+)"\).*/\1/')
            [ -n "$key" ] || continue
            if ! echo "$stripped" | grep -qE "is_(number|string|boolean|object|array)\(\).*\"$key\"|\"$key\"\).*is_(number|string|boolean|object|array)\("; then
                echo "[json-field-type] $f:$lineno: .at(\"$key\").get<T>() with no is_X() type check for \"$key\" found anywhere in this file:"
                sed -n "${lineno}p" <<<"$stripped" | sed 's/^/    /'
                status=1
            fi
        done <<<"$get_lines"
    fi
done

exit $status
