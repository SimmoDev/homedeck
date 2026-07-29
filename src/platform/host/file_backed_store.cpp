#include "platform/host/file_backed_store.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace homedeck {

bool WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::cerr << "Failed to create directory '" << path.parent_path().string() << "': " << ec.message() << "\n";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Failed to open '" << path.string() << "' for writing\n";
        return false;
    }
    out << content;
    if (!out.good()) {
        std::cerr << "Failed to write '" << path.string() << "'\n";
        return false;
    }
    return true;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

bool EraseFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

}  // namespace homedeck
