#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Minecraft {
struct ProgressInfo {
    std::string stage;
    std::string current_file;
    size_t completed_files = 0;
    size_t total_files = 0;
    uint64_t transferred_bytes = 0;
    uint64_t total_bytes = 0;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

bool installVanilla(const std::string& version, const std::filesystem::path& data_root,
                    const std::filesystem::path& instance, const std::string& name,
                    const ProgressCallback& progress, std::string& error);
}
