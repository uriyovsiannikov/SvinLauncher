#include "launch.hpp"
#include "instance_store.hpp"
#include "instance_list.hpp"
#include "../app/app.hpp"
#include "../app/i18n.hpp"
#include "../minecraft/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

static void skip_json_space(std::string_view json, size_t& pos)
{
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
}

static bool read_json_string(std::string_view json, size_t& pos, std::string* output)
{
    if (pos >= json.size() || json[pos++] != '"') return false;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            if (output) output->push_back(c);
            continue;
        }
        if (pos >= json.size()) return false;
        c = json[pos++];
        switch (c) {
            case '"': case '\\': case '/': if (output) output->push_back(c); break;
            case 'b': if (output) output->push_back('\b'); break;
            case 'f': if (output) output->push_back('\f'); break;
            case 'n': if (output) output->push_back('\n'); break;
            case 'r': if (output) output->push_back('\r'); break;
            case 't': if (output) output->push_back('\t'); break;
            case 'u':
                if (pos + 4 > json.size()) return false;
                if (output) output->push_back('?');
                pos += 4;
                break;
            default: return false;
        }
    }
    return false;
}

struct RunningProcess {
    App* app;
    std::string instance_id;
    fs::path log_path;
};

static void launch_exited(GPid pid, gint status, gpointer data)
{
    auto* process = static_cast<RunningProcess*>(data);
    auto* app = process->app;
    app->running_instances.erase(process->instance_id);
    GError* error = nullptr;
    if (!g_spawn_check_wait_status(status, &error)) {
        std::ifstream log(process->log_path);
        std::string output((std::istreambuf_iterator<char>(log)), {});
        if (output.find("AppClassLoader") != std::string::npos &&
            output.find("URLClassLoader") != std::string::npos) {
            notify(app, std::string(tr("Эта старая версия Minecraft несовместима с Java 9+. Нужна Java 8; укажите её путь в ")) +
                        (app->root / "svinlauncher.cfg").string() + " в параметре JavaPath.");
        } else {
            notify(app, std::string(tr("Лог запуска: ")) + process->log_path.string() + " — " +
                        (error ? error->message : tr("Запуск игры завершился с ошибкой")));
        }
    }
    if (error) g_error_free(error);
    g_spawn_close_pid(pid);
    delete process;
}

static void redirect_child_output(gpointer data)
{
    const int log_fd = *static_cast<int*>(data);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    if (log_fd > STDERR_FILENO) close(log_fd);
}

static bool json_string_after(std::string_view json, std::string_view key, std::string& out, size_t start = 0)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle, start);
    if (pos == std::string_view::npos) return false;
    pos += needle.size();
    skip_json_space(json, pos);
    if (pos >= json.size() || json[pos++] != ':') return false;
    skip_json_space(json, pos);
    return read_json_string(json, pos, &out);
}

static std::string offline_uuid(const std::string& username)
{
    const std::string seed = "OfflinePlayer:" + username;
    auto* checksum = g_checksum_new(G_CHECKSUM_MD5);
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(seed.data()), seed.size());
    guchar bytes[16];
    gsize length = sizeof(bytes);
    g_checksum_get_digest(checksum, bytes, &length);
    g_checksum_free(checksum);
    bytes[6] = (bytes[6] & 0x0f) | 0x30;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    char id[37];
    g_snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
               bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return id;
}

static std::string maven_path(std::string coordinate)
{
    std::string extension = "jar";
    if (auto at = coordinate.find('@'); at != std::string::npos) { extension = coordinate.substr(at + 1); coordinate.resize(at); }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= coordinate.size()) {
        auto end = coordinate.find(':', start);
        parts.push_back(coordinate.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (parts.size() < 3) return {};
    std::replace(parts[0].begin(), parts[0].end(), '.', '/');
    std::string filename = parts[1] + "-" + parts[2];
    if (parts.size() > 3) filename += "-" + parts[3];
    return parts[0] + "/" + parts[1] + "/" + parts[2] + "/" + filename + "." + extension;
}

static bool library_allowed(const json::Value& library)
{
    auto* rules = library.get("rules");
    if (!rules) return true;
    bool include = false;
    for (const auto& rule : rules->array) {
        auto* action = rule.get("action");
        auto* os = rule.get("os");
        auto* os_name = os ? os->get("name") : nullptr;
        const auto name = os_name ? os_name->string() : std::string{};
        if (name.empty() || name == "linux") include = action && action->string() == "allow";
    }
    return include;
}

static bool launch_offline_game(App* app, const std::string& id, const std::string& username)
{
    const auto instance = app->instance_dir / id;
    const auto ver = instance_version(instance / "mmc-pack.json");
    const auto game = fs::exists(instance / ".minecraft") ? instance / ".minecraft" : instance / "minecraft";
    const std::vector<fs::path> metadata_candidates = {
        game / "versions" / ver / (ver + ".json"), app->root / "versions" / ver / (ver + ".json"),
        instance / "versions" / ver / (ver + ".json")};
    fs::path metadata;
    for (const auto& candidate : metadata_candidates) if (fs::is_regular_file(candidate)) { metadata = candidate; break; }
    if (ver.empty() || metadata.empty()) {
        notify(app, tr("Для этой версии ещё нет установленных файлов Minecraft. Сначала потребуется установить vanilla-файлы."));
        return false;
    }
    std::ifstream input(metadata);
    std::string json((std::istreambuf_iterator<char>(input)), {});
    std::string main_class, asset_index;
    if (!json_string_after(json, "mainClass", main_class)) {
        notify(app, tr("В файле версии Minecraft отсутствует mainClass"));
        return false;
    }
    auto asset_key = json.find("\"assetIndex\"");
    if (asset_key != std::string::npos) json_string_after(json, "id", asset_index, asset_key);

    std::string classpath;
    auto add_jar = [&classpath](const fs::path& jar) {
        if (!classpath.empty()) classpath.push_back(':');
        classpath += jar.string();
    };
    json::Value version_json;
    json::Parser version_parser(json);
    if (!version_parser.parse(version_json)) {
        notify(app, tr("Повреждён JSON-файл версии Minecraft"));
        return false;
    }
    if (auto* libraries = version_json.get("libraries")) {
        for (const auto& library : libraries->array) {
            if (!library_allowed(library)) continue;
            auto* downloads = library.get("downloads");
            auto* artifact = downloads ? downloads->get("artifact") : nullptr;
            auto relative = artifact && artifact->get("path") ? artifact->get("path")->string() : std::string{};
            if (relative.empty()) relative = maven_path(library.get("name") ? library.get("name")->string() : std::string{});
            if (relative.empty()) continue;
            auto path = instance / "libraries" / relative;
            if (!fs::is_regular_file(path)) path = app->root / "libraries" / relative;
            if (!fs::is_regular_file(path)) path = game / "libraries" / relative;
            if (fs::is_regular_file(path) && path.extension() == ".jar") add_jar(path);
        }
    }
    const auto client_jar = metadata.parent_path() / (ver + ".jar");
    if (!fs::is_regular_file(client_jar)) {
        notify(app, std::string(tr("Не найден client.jar для Minecraft ")) + ver);
        return false;
    }
    add_jar(client_jar);
    const auto java_setting = read_setting(app->root / "svinlauncher.cfg", "JavaPath");
    const std::string java = java_setting.empty() ? "java" : java_setting;
    const auto native_path = instance / "natives";
    const auto instance_assets = instance / "assets";
    const auto assets_root = fs::is_directory(instance_assets) ? instance_assets : app->root / "assets";
    const auto uuid = offline_uuid(username);
    const bool modern_arguments = version_json.get("arguments") != nullptr;
    std::vector<std::string> args = {java};
    if (modern_arguments) args.push_back("--add-modules=ALL-MODULE-PATH");
    args.insert(args.end(), {"-Djava.library.path=" + native_path.string(), "-cp", classpath, main_class});
    if (modern_arguments) {
        args.insert(args.end(), {"--username", username, "--version", ver, "--gameDir", game.string(),
            "--assetsDir", assets_root.string(), "--uuid", uuid, "--accessToken", "0", "--userType", "legacy"});
        args.insert(args.end(), {"--clientId", "00000000-0000-0000-0000-000000000000", "--xuid", "", "--versionType", "release"});
    } else {
        const auto* minecraft_arguments = version_json.get("minecraftArguments");
        const auto raw_arguments = minecraft_arguments ? minecraft_arguments->string() : std::string{};
        std::istringstream input_arguments(raw_arguments);
        std::string argument;
        const std::vector<std::pair<std::string, std::string>> substitutions = {
            {"${auth_player_name}", username}, {"${auth_session}", "0"},
            {"${game_directory}", game.string()}, {"${game_assets}", assets_root.string()},
            {"${assets_root}", assets_root.string()}, {"${assets_index_name}", asset_index},
            {"${auth_uuid}", uuid}, {"${auth_access_token}", "0"}, {"${user_type}", "legacy"},
            {"${version_type}", "release"}, {"${version_name}", ver}, {"${user_properties}", "{}"},
            {"${clientid}", "00000000-0000-0000-0000-000000000000"}, {"${auth_xuid}", ""}};
        while (input_arguments >> argument) {
            for (const auto& [placeholder, replacement] : substitutions) {
                size_t position = 0;
                while ((position = argument.find(placeholder, position)) != std::string::npos) {
                    argument.replace(position, placeholder.size(), replacement);
                    position += replacement.size();
                }
            }
            args.push_back(std::move(argument));
        }
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    GPid pid = 0;
    GError* error = nullptr;
    const auto working_directory = game.string();
    const auto log_path = instance / "logs" / "latest.log";
    std::error_code log_error;
    fs::create_directories(log_path.parent_path(), log_error);
    int log_fd = log_error ? -1 : open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        notify(app, std::string(tr("Не удалось создать лог запуска: ")) + (log_error ? log_error.message() : log_path.string()));
        return false;
    }
    if (!g_spawn_async(working_directory.c_str(), argv.data(), nullptr,
                       static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
                       redirect_child_output, &log_fd, &pid, &error)) {
        close(log_fd);
        notify(app, error ? error->message : tr("Не удалось запустить Java"));
        if (error) g_error_free(error);
        return false;
    }
    close(log_fd);
    app->running_instances.insert(id);
    g_child_watch_add(pid, launch_exited, new RunningProcess{app, id, log_path});
    return true;
}

void launch_offline(GtkButton* button, gpointer data)
{
    if (button) dismiss_actions(button);
    auto* row = GTK_LIST_BOX_ROW(data);
    auto* app = static_cast<App*>(g_object_get_data(G_OBJECT(row), "launcher-app"));
    auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "instance-id"));
    const std::string username = app->username_entry ? gtk_editable_get_text(GTK_EDITABLE(app->username_entry)) : "";
    const bool valid = !username.empty() && username.size() <= 16 &&
        std::all_of(username.begin(), username.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    if (!valid) {
        notify(app, tr("Ник должен содержать от 1 до 16 латинских букв, цифр или знаков _"));
        return;
    }
    write_setting(app->root / "svinlauncher.cfg", "PlayerName", username);
    launch_offline_game(app, id ? id : "", username);
}
