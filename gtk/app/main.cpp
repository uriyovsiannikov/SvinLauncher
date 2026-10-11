#include "app.hpp"
#include "i18n.hpp"
#include "../instances/instance_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <clocale>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    const char* home = std::getenv("HOME");
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    const char* data_override = std::getenv("SVINLAUNCHER_DATA_DIR");
    fs::path data_home = xdg_data ? fs::path(xdg_data) : (home ? fs::path(home) / ".local/share" : fs::path("."));
    fs::path root = data_override ? fs::path(data_override) : data_home / "SvinLauncher";
    if (!data_override) {
        const fs::path legacy_root = data_home / "PrismLauncher";
        std::error_code migration_error;
        if (!fs::exists(root, migration_error) && fs::exists(legacy_root, migration_error)) {
            fs::rename(legacy_root, root, migration_error);
            if (migration_error) root = legacy_root;
        }
    }
    std::error_code config_rename_error;
    const auto legacy_config = root / "prismlauncher.cfg";
    if (fs::exists(legacy_config, config_rename_error) && !fs::exists(root / "svinlauncher.cfg", config_rename_error))
        fs::rename(legacy_config, root / "svinlauncher.cfg", config_rename_error);
    auto configured = read_setting(root / "svinlauncher.cfg", "InstanceDir");
    const auto language = read_setting(root / "svinlauncher.cfg", "Language");
    setlocale(LC_ALL, "");
    if (!std::getenv("LANGUAGE")) {
        std::string preferred_language = language;
        if (preferred_language.empty()) {
            const char* const* system_languages = g_get_language_names();
            if (system_languages && system_languages[0]) preferred_language = system_languages[0];
            const auto suffix = preferred_language.find_first_of("_.@-");
            if (suffix != std::string::npos) preferred_language.resize(suffix);
        }
        if (!preferred_language.empty() && preferred_language != "C" && preferred_language != "POSIX") {
            const auto language_chain = preferred_language == "ru" ? preferred_language : preferred_language + ":en";
            setenv("LANGUAGE", language_chain.c_str(), 1);
        }
    }
    const fs::path user_locale = fs::path(g_get_user_data_dir()) / "locale";
    const fs::path user_catalog = user_locale / (language.empty() ? "en" : language) / "LC_MESSAGES/svinlauncher.mo";
    bindtextdomain("svinlauncher", fs::is_regular_file(user_catalog) ? user_locale.c_str() : SVIN_LOCALE_DIR);
    bind_textdomain_codeset("svinlauncher", "UTF-8");
    textdomain("svinlauncher");
    fs::path instance_dir = configured.empty() ? root / "instances" : fs::path(configured);
    if (instance_dir.is_relative()) instance_dir = root / instance_dir;
    App app{ADW_APPLICATION(adw_application_new(SVIN_APP_ID, G_APPLICATION_DEFAULT_FLAGS)), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            root, instance_dir, language, {}};
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    auto status = g_application_run(G_APPLICATION(app.application), argc, argv);
    g_object_unref(app.application);
    return status;
}
