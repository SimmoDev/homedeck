#pragma once

#include <string>

namespace homedeck {

// Inserts a space at each camelCase word boundary: before an uppercase
// letter that follows a lowercase letter/digit ("RightBumper" ->
// "Right Bumper"), and before the last letter of an uppercase run that's
// followed by a lowercase one, so acronyms stay together ("TVShows" ->
// "TV Shows", "NavigationDSTB" -> "Navigation DSTB", not "T VShows" or
// "Navigation D S T B"). A no-op on text that's already spaced (there's
// no lowercase-then-uppercase transition to find), so it's safe to apply
// unconditionally - HarmonyControlGroup/HarmonyCommand names are
// consistently PascalCase, but the hub's own `label` field is
// inconsistent about spacing them out already (see DevicesScreen's own
// use of this).
std::string SplitCamelCase(const std::string& text);

}  // namespace homedeck
