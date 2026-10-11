#include "libraries.hpp"
#include "http.hpp"
#include "../app/i18n.hpp"

#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace Minecraft::Internal {
bool libraryAllowed(const json::Value& library)
{
    auto* rules = library.get("rules");
    if (!rules) return true;
    bool include = false;
    for (const auto& rule : rules->array) {
        const auto action = stringField(&rule, "action");
        auto* os = rule.get("os");
        const auto name = stringField(os, "name");
        const bool matches = name.empty() || name == "linux";
        if (matches) include = action == "allow";
    }
    return include;
}
bool extractNatives(const fs::path& jar, const fs::path& destination, std::string& error)
{
    std::ifstream input(jar, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto u16 = [&bytes](size_t offset) -> uint16_t {
        if (offset + 2 > bytes.size()) return 0;
        return static_cast<uint8_t>(bytes[offset]) | (static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset + 1])) << 8);
    };
    auto u32 = [&bytes](size_t offset) -> uint32_t {
        if (offset + 4 > bytes.size()) return 0;
        return static_cast<uint8_t>(bytes[offset]) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 3])) << 24);
    };
    if (!input || bytes.size() < 22) { error = std::string(tr("Не удалось открыть native-библиотеку ")) + jar.filename().string(); return false; }
    const size_t search_start = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    size_t eocd = std::string::npos;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (u32(pos) == 0x06054b50) { eocd = pos; break; }
        if (pos == search_start) break;
    }
    if (eocd == std::string::npos) { error = tr("Повреждён native ZIP-файл"); return false; }
    size_t cursor = u32(eocd + 16);
    const auto entries = u16(eocd + 10);
    std::error_code ec; fs::create_directories(destination, ec);
    if (ec) { error = ec.message(); return false; }
    for (uint16_t index = 0; index < entries && cursor + 46 <= bytes.size(); ++index) {
        if (u32(cursor) != 0x02014b50) { error = tr("Повреждён каталог native ZIP-файла"); break; }
        const auto flags = u16(cursor + 8);
        const auto method = u16(cursor + 10);
        const auto crc = u32(cursor + 16);
        const auto compressed_size = u32(cursor + 20);
        const auto uncompressed_size = u32(cursor + 24);
        const auto name_size = u16(cursor + 28);
        const auto extra_size = u16(cursor + 30);
        const auto comment_size = u16(cursor + 32);
        const auto local_offset = u32(cursor + 42);
        if (cursor + 46 + name_size + extra_size + comment_size > bytes.size()) { error = tr("Обрезан native ZIP-файл"); break; }
        fs::path relative(bytes.substr(cursor + 46, name_size));
        cursor += 46 + name_size + extra_size + comment_size;
        const auto extension = relative.filename().string();
        if (relative.is_absolute() || relative.string().find("..") != std::string::npos ||
            (!extension.ends_with(".so") && !extension.ends_with(".dylib") && !extension.ends_with(".jnilib")) ||
            (flags & 1) || (method != 0 && method != 8)) continue;
        if (local_offset + 30 > bytes.size() || u32(local_offset) != 0x04034b50) { error = tr("Повреждён native ZIP-файл"); break; }
        const auto local_name_size = u16(local_offset + 26);
        const auto local_extra_size = u16(local_offset + 28);
        const size_t data_offset = local_offset + 30 + local_name_size + local_extra_size;
        if (data_offset + compressed_size > bytes.size() || uncompressed_size > 128u * 1024 * 1024) { error = tr("Некорректный размер native-файла"); break; }
        std::string data(uncompressed_size, '\0');
        if (method == 0) {
            if (compressed_size != uncompressed_size) { error = tr("Повреждён native ZIP-файл"); break; }
            data.assign(bytes.data() + data_offset, compressed_size);
        } else {
            z_stream stream{};
            stream.next_in = reinterpret_cast<Bytef*>(bytes.data() + data_offset);
            stream.avail_in = compressed_size;
            stream.next_out = reinterpret_cast<Bytef*>(data.data());
            stream.avail_out = uncompressed_size;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) { error = tr("Не удалось распаковать native-библиотеки"); break; }
            const int status = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (status != Z_STREAM_END || stream.total_out != uncompressed_size) { error = tr("Native ZIP-файл повреждён"); break; }
        }
        if (crc32(0, reinterpret_cast<const Bytef*>(data.data()), data.size()) != crc) { error = tr("Проверка native-файла не прошла"); break; }
        const auto target = destination / relative;
        fs::create_directories(target.parent_path(), ec);
        if (ec) { error = ec.message(); break; }
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!output) { error = tr("Не удалось распаковать native-файлы"); break; }
    }
    return error.empty();
}
std::string nativeClassifier(const json::Value& library)
{
    auto* natives = library.get("natives");
    auto result = stringField(natives, "linux");
    if (result.empty()) return {};
    auto arch = result.find("${arch}");
    if (arch != std::string::npos) result.replace(arch, 7, sizeof(void*) == 8 ? "64" : "32");
    return result;
}
std::string mavenPath(std::string coordinate)
{
    std::string extension = "jar";
    if (auto at = coordinate.find('@'); at != std::string::npos) {
        extension = coordinate.substr(at + 1);
        coordinate.resize(at);
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= coordinate.size()) {
        auto end = coordinate.find(':', start);
        parts.push_back(coordinate.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (parts.size() < 3) return {};
    std::string group = parts[0];
    std::replace(group.begin(), group.end(), '.', '/');
    std::string file = parts[1] + "-" + parts[2];
    if (parts.size() > 3) file += "-" + parts[3];
    file += "." + extension;
    return group + "/" + parts[1] + "/" + parts[2] + "/" + file;
}

} // namespace Minecraft::Internal
