#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>

namespace Minecraft::Internal {
bool libraryAllowed(const json::Value& library);
bool extractNatives(const std::filesystem::path& jar, const std::filesystem::path& destination,
                    std::string& error);
std::string nativeClassifier(const json::Value& library);
std::string mavenPath(std::string coordinate);
}
