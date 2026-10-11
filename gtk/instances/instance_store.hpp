#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Instance {
    std::string id;
    std::string name;
    std::string version;
};

std::string read_setting(const std::filesystem::path& file, const std::string& key);
bool write_setting(const std::filesystem::path& file, const std::string& key, const std::string& value);
std::string instance_version(const std::filesystem::path& file);
std::vector<Instance> list_instances(const std::filesystem::path& directory);
