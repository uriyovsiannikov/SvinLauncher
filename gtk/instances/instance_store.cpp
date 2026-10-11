#include "instance_store.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

std::string read_setting(const fs::path& file, const std::string& key)
{
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        auto split = line.find('=');
        if (split != std::string::npos && line.substr(0, split) == key)
            return line.substr(split + 1);
    }
    return {};
}

bool write_setting(const fs::path& file, const std::string& key, const std::string& value)
{
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line)) {
        const auto split = line.find('=');
        if (split != std::string::npos && line.substr(0, split) == key) {
            if (!replaced) lines.push_back(key + "=" + value);
            replaced = true;
        } else {
            lines.push_back(std::move(line));
        }
    }
    if (!replaced) lines.push_back(key + "=" + value);
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return false;
    std::ofstream output(file, std::ios::trunc);
    if (!output) return false;
    for (const auto& item : lines) output << item << '\n';
    return static_cast<bool>(output);
}

std::string instance_version(const fs::path& file)
{
    std::ifstream input(file);
    std::string all((std::istreambuf_iterator<char>(input)), {});
    auto uid = all.find("net.minecraft");
    if (uid == std::string::npos) return {};
    auto key = all.find("\"version\"", uid);
    if (key == std::string::npos) return {};
    auto begin = all.find('"', key + 9);
    auto end = all.find('"', begin + 1);
    return begin == std::string::npos || end == std::string::npos ? std::string{} : all.substr(begin + 1, end - begin - 1);
}

std::vector<Instance> list_instances(const fs::path& directory)
{
    std::vector<Instance> result;
    if (!fs::exists(directory)) return result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_directory() || !fs::exists(entry.path() / "instance.cfg")) continue;
        auto name = read_setting(entry.path() / "instance.cfg", "name");
        if (name.empty()) name = entry.path().filename().string();
        result.push_back({entry.path().filename().string(), name, instance_version(entry.path() / "mmc-pack.json")});
    }
    std::sort(result.begin(), result.end(), [](const Instance& left, const Instance& right) {
        return left.name < right.name;
    });
    return result;
}
