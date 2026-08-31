#pragma once

#include <cctype>
#include <string>

namespace homedeck {

// A hostname/IP that a user typed or that arrived over mDNS is
// concatenated straight into a URL authority elsewhere in the codebase
// (`http://<value>:8088/...` for Harmony, `ws://<value>:9090/jsonrpc`
// for Kodi). Reject every byte that would make that a *different* URL
// than intended rather than merely an unreachable one:
//
//   - a `://` scheme prefix,
//   - ASCII whitespace, any C0 control byte, DEL, or any non-ASCII byte
//     (a hostname/IP has no legitimate use for these; rejecting the
//     whole `>= 0x80` range also sidesteps enumerating every multi-byte
//     UTF-8 whitespace codepoint, e.g. U+00A0, that the frontend's
//     `/\s/` regex already covers but a byte-wise `std::isspace()`
//     cannot see),
//   - a path (`/`),
//   - `#` / `?` / `@` (a fragment truncates the authority, `@`
//     introduces userinfo, a second `?` starts another query string -
//     each changes what a URL parser treats as the actual host/port).
//
// `:` is rejected as well unless `allow_colon`: a bare IPv6 literal must
// be bracketed in a URL authority. The manual-entry paths do not bracket
// it, so they pass `allow_colon=false`; a discovered address is bracketed
// by the URL builder itself, so those paths pass `allow_colon=true` and
// rely on this function only for the byte classes above.
//
// This is the one server-side implementation behind Harmony's
// `IsValidHubHost()` and Kodi's `IsValidKodiHost()` /
// discovered-host screen. Mirrored on the frontend by
// webui/src/lib/hostValidation.ts.
inline bool HasUnsafeHostChars(const std::string& value, bool allow_colon) {
    if (value.find("://") != std::string::npos) {
        return true;
    }
    for (unsigned char c : value) {
        if (std::isspace(c) || c >= 0x80 || c < 0x20 || c == 0x7F) {
            return true;
        }
    }
    if (value.find_first_of("/#?@") != std::string::npos) {
        return true;
    }
    if (!allow_colon && value.find(':') != std::string::npos) {
        return true;
    }
    return false;
}

}  // namespace homedeck
