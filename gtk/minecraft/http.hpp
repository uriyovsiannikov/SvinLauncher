#pragma once

#include "installer.hpp"
#include "json.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Minecraft::Internal {
struct FileDownload {
    std::string url;
    std::string sha1;
    std::filesystem::path target;
    std::string display_name;
    uint64_t size = 0;
};

bool get(const std::string& url, std::string& body, std::string& error);
bool parseJson(std::string_view text, json::Value& root, std::string& error);
std::string stringField(const json::Value* object, std::string_view key);
uint64_t numberField(const json::Value* object, std::string_view key);
void reportProgress(const ProgressCallback& progress, const std::string& stage,
                    std::string current_file = {}, size_t completed_files = 0, size_t total_files = 0,
                    uint64_t transferred_bytes = 0, uint64_t total_bytes = 0);
bool saveFile(const std::filesystem::path& path, const std::string& bytes, std::string& error);
bool downloadFile(std::string url, const std::filesystem::path& target, std::string& error,
                  std::string_view sha1 = {},
                  const std::function<void(size_t, size_t)>& progress = {});
bool safeRelativePath(const std::filesystem::path& path);
bool isSha1(std::string_view hash);
bool downloadMany(const std::vector<FileDownload>& files, std::string& error,
                  const ProgressCallback& progress, const std::string& stage);
}
