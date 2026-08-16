#include "ui/text_format.h"

#include <cctype>

namespace homedeck {

std::string SplitCamelCase(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (i > 0) {
            unsigned char prev = static_cast<unsigned char>(text[i - 1]);
            unsigned char curr = static_cast<unsigned char>(text[i]);
            bool lower_to_upper = (std::islower(prev) || std::isdigit(prev)) && std::isupper(curr);
            bool acronym_boundary =
                std::isupper(prev) && std::isupper(curr) && i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (lower_to_upper || acronym_boundary) {
                result += ' ';
            }
        }
        result += text[i];
    }
    return result;
}

}  // namespace homedeck
