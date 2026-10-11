#include "installer.hpp"
#include "http.hpp"
#include "libraries.hpp"
#include "../app/i18n.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

bool Minecraft::installVanilla(const std::string& version, const fs::path& dataRoot,
                               const fs::path& instance, const std::string& name,
                               const ProgressCallback& progress, std::string& error)
{
    using namespace Internal;
    reportProgress(progress, tr("Получение списка версий Minecraft"));
    std::string manifest_bytes;
    if (!get("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", manifest_bytes, error)) return false;
    json::Value manifest;
    if (!parseJson(manifest_bytes, manifest, error)) return false;
    const json::Value* selected = nullptr;
    if (auto* versions = manifest.get("versions")) {
        for (const auto& entry : versions->array) if (stringField(&entry, "id") == version && stringField(&entry, "type") == "release") { selected = &entry; break; }
    }
    const auto version_url = stringField(selected, "url");
    if (version_url.empty()) { error = std::string(tr("Не найдена release-версия Minecraft ")) + version; return false; }
    reportProgress(progress, std::string(tr("Получение данных Minecraft ")) + version);
    std::string metadata_bytes;
    if (!get(version_url, metadata_bytes, error)) return false;
    json::Value metadata;
    if (!parseJson(metadata_bytes, metadata, error)) return false;

    const auto game = instance / ".minecraft";
    const auto version_dir = game / "versions" / version;
    if (!saveFile(version_dir / (version + ".json"), metadata_bytes, error)) return false;
    auto* downloads = metadata.get("downloads");
    auto* client = downloads ? downloads->get("client") : nullptr;
    const auto client_url = stringField(client, "url");
    const auto client_size = numberField(client, "size");
    const auto client_name = (version_dir / (version + ".jar")).filename().string();
    reportProgress(progress, tr("Загрузка игрового клиента"), client_name, 0, 1, 0, client_size);
    if (client_url.empty() || !downloadFile(client_url, version_dir / (version + ".jar"), error, stringField(client, "sha1"),
        [&progress](size_t completed, size_t total) {
            reportProgress(progress, tr("Загрузка игрового клиента"), "client.jar", 0, 1, completed, total);
        })) return false;
    reportProgress(progress, tr("Игровой клиент загружен"), client_name, 1, 1, client_size, client_size);

    const auto libraries_root = dataRoot / "libraries";
    const auto native_dir = instance / "natives";
    std::vector<FileDownload> library_downloads_queue;
    std::vector<fs::path> native_jars;
    if (auto* libraries = metadata.get("libraries")) {
        for (const auto& library : libraries->array) {
            if (!libraryAllowed(library)) continue;
            auto* library_downloads = library.get("downloads");
            auto* artifact = library_downloads ? library_downloads->get("artifact") : nullptr;
            auto path = stringField(artifact, "path");
            auto url = stringField(artifact, "url");
            // Some old Mojang manifests provide only native classifiers (for
            // example lwjgl-platform 2.9.0). Do not invent a regular JAR for
            // those entries: it does not exist on the Maven server.
            if (path.empty() && !library_downloads) path = mavenPath(stringField(&library, "name"));
            if (!path.empty() && !safeRelativePath(path)) { error = tr("Манифест содержит небезопасный путь библиотеки"); return false; }
            if (url.empty() && !path.empty()) {
                url = stringField(&library, "url");
                if (url.empty()) url = "https://libraries.minecraft.net/";
                if (!url.ends_with('/')) url.push_back('/');
                url += path;
            }
            if (!path.empty() && !url.empty())
                library_downloads_queue.push_back({url, stringField(artifact, "sha1"), libraries_root / path,
                                                   path, numberField(artifact, "size")});
            const auto classifier = nativeClassifier(library);
            auto* classifiers = library_downloads ? library_downloads->get("classifiers") : nullptr;
            auto* native = classifiers ? classifiers->get(classifier) : nullptr;
            if (native) {
                auto native_path = stringField(native, "path");
                auto native_url = stringField(native, "url");
                if (!native_path.empty() && !native_url.empty()) {
                    const auto jar = libraries_root / native_path;
                    library_downloads_queue.push_back({native_url, stringField(native, "sha1"), jar,
                                                       native_path, numberField(native, "size")});
                    native_jars.push_back(jar);
                }
            } else if (!classifier.empty()) {
                const auto native_maven_path = mavenPath(stringField(&library, "name") + ":" + classifier);
                if (!native_maven_path.empty()) {
                    auto base = stringField(&library, "url");
                    if (base.empty()) base = "https://libraries.minecraft.net/";
                    if (!base.ends_with('/')) base.push_back('/');
                    const auto native_jar = libraries_root / native_maven_path;
                    library_downloads_queue.push_back({base + native_maven_path, {}, native_jar,
                                                       native_maven_path, 0});
                    native_jars.push_back(native_jar);
                }
            }
        }
    }
    if (!downloadMany(library_downloads_queue, error, progress, tr("Загрузка библиотек"))) return false;
    reportProgress(progress, tr("Распаковка библиотек"), {}, 0, native_jars.size());
    size_t extracted = 0;
    for (const auto& native_jar : native_jars) {
        if (!extractNatives(native_jar, native_dir, error)) return false;
        reportProgress(progress, tr("Распаковка библиотек"), native_jar.filename().string(), ++extracted, native_jars.size());
    }

    std::string asset_index_id;
    auto* asset_index = metadata.get("assetIndex");
    const auto asset_url = stringField(asset_index, "url");
    asset_index_id = stringField(asset_index, "id");
    if (!asset_url.empty() && !asset_index_id.empty()) {
        reportProgress(progress, tr("Загрузка списка игровых ресурсов"));
        std::string index_bytes;
        if (!get(asset_url, index_bytes, error)) return false;
        if (!saveFile(dataRoot / "assets" / "indexes" / (asset_index_id + ".json"), index_bytes, error)) return false;
        json::Value index;
        if (!parseJson(index_bytes, index, error)) return false;
        auto* objects = index.get("objects");
        std::vector<FileDownload> asset_downloads;
        std::unordered_set<std::string> scheduled_assets;
        if (objects) for (const auto& [logical, object] : objects->object) {
            const auto hash = stringField(&object, "hash");
            if (!isSha1(hash)) { error = tr("Манифест ассетов содержит некорректный SHA-1"); return false; }
            if (!scheduled_assets.insert(hash).second) continue;
            const auto object_url = "https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash;
            const auto object_path = dataRoot / "assets" / "objects" / hash.substr(0, 2) / hash;
            asset_downloads.push_back({object_url, hash, object_path, logical, numberField(&object, "size")});
        }
        if (!downloadMany(asset_downloads, error, progress, tr("Загрузка игровых ресурсов"))) return false;
        auto* virtual_assets = index.get("virtual");
        auto* mapped_assets = metadata.get("map_to_resources");
        if ((virtual_assets && virtual_assets->boolean) || (mapped_assets && mapped_assets->boolean)) {
            if (objects) for (const auto& [logical, object] : objects->object) {
                const auto hash = stringField(&object, "hash");
                if (!isSha1(hash)) continue;
                const auto source = dataRoot / "assets" / "objects" / hash.substr(0, 2) / hash;
                const fs::path relative(logical);
                if (relative.is_absolute() || relative.string().find("..") != std::string::npos) continue;
                const auto target = dataRoot / "assets" / "virtual" / asset_index_id / relative;
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                if (!ec && !fs::exists(target, ec)) fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
                if (ec) { error = std::string(tr("Не удалось подготовить виртуальные ресурсы Minecraft: ")) + ec.message(); return false; }
            }
        }
    }

    reportProgress(progress, tr("Завершение установки"));
    std::error_code ec;
    fs::create_directories(game, ec);
    if (ec) { error = ec.message(); return false; }
    std::ofstream cfg(instance / "instance.cfg", std::ios::trunc);
    cfg << "[General]\nInstanceType=OneSix\nname=" << name << "\n";
    std::ofstream pack(instance / "mmc-pack.json", std::ios::trunc);
    pack << "{\"formatVersion\":1,\"components\":[{\"cachedName\":\"Minecraft\",\"cachedVersion\":\""
         << version << "\",\"uid\":\"net.minecraft\",\"version\":\"" << version
         << "\",\"important\":true,\"volatile\":true}]}\n";
    if (!cfg || !pack) { error = tr("Не удалось сохранить файлы экземпляра"); return false; }
    return true;
}
