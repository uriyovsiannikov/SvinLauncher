#include "settings.hpp"
#include "i18n.hpp"
#include "../instances/instance_store.hpp"

#include <adwaita.h>
#include <clocale>
#include <cstdlib>
#include <filesystem>

static const char* language_codes[] = {"", "ru", "en", "es", "pt_BR", "fr", "de", "zh_CN", "ja", "ko", "pl"};

static void apply_language(App* app, const std::string& language)
{
    app->language = language;
    const std::string language_chain = language.empty() ? app->system_language :
        (language == "ru" ? language : language + ":en");
    if (language_chain.empty()) unsetenv("LANGUAGE");
    else setenv("LANGUAGE", language_chain.c_str(), 1);

    const char* current_locale = setlocale(LC_MESSAGES, nullptr);
    const std::string saved_locale = current_locale ? current_locale : "C";
    setlocale(LC_MESSAGES, "C");
    setlocale(LC_MESSAGES, saved_locale.c_str());

    std::string catalog_language = language_chain.substr(0, language_chain.find(':'));
    const auto suffix = catalog_language.find_first_of(".@-");
    if (suffix != std::string::npos) catalog_language.resize(suffix);
    const auto user_locale = std::filesystem::path(g_get_user_data_dir()) / "locale";
    const auto user_catalog = user_locale / (catalog_language.empty() ? "en" : catalog_language) /
                              "LC_MESSAGES/svinlauncher.mo";
    bindtextdomain("svinlauncher", std::filesystem::is_regular_file(user_catalog) ? user_locale.c_str() : SVIN_LOCALE_DIR);
    bind_textdomain_codeset("svinlauncher", "UTF-8");
    textdomain("svinlauncher");
}

static gboolean rebuild_window(gpointer data)
{
    auto* app = static_cast<App*>(data);
    if (app->window) gtk_window_destroy(app->window);
    activate(G_APPLICATION(app->application), app);
    notify(app, tr("Язык интерфейса изменён"));
    return G_SOURCE_REMOVE;
}

static void settings_response(AdwAlertDialog* dialog, const char* response, gpointer data)
{
    if (!response || std::string(response) != "save") return;
    auto* app = static_cast<App*>(data);
    auto* dropdown = GTK_DROP_DOWN(g_object_get_data(G_OBJECT(dialog), "language-dropdown"));
    const guint selected = gtk_drop_down_get_selected(dropdown);
    const std::string language = selected < G_N_ELEMENTS(language_codes) ? language_codes[selected] : "";
    if (!write_setting(app->root / "svinlauncher.cfg", "Language", language)) {
        notify(app, tr("Не удалось сохранить язык интерфейса"));
        return;
    }
    apply_language(app, language);
    g_idle_add(rebuild_window, app);
}

void show_settings(GtkButton*, gpointer data)
{
    auto* app = static_cast<App*>(data);
    auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Настройки SvinLauncher"), nullptr));
    adw_alert_dialog_add_responses(dialog, "close", tr("Закрыть"), "save", tr("Сохранить"), nullptr);
    adw_alert_dialog_set_default_response(dialog, "save");
    adw_alert_dialog_set_close_response(dialog, "close");

    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    auto* about = gtk_label_new(tr("Минимальный лаунчер Minecraft Java Edition для GNOME. Создавайте экземпляры, выбирайте офлайн-имя и запускайте игру."));
    gtk_label_set_wrap(GTK_LABEL(about), TRUE);
    gtk_label_set_xalign(GTK_LABEL(about), 0.0f);
    gtk_box_append(GTK_BOX(content), about);

    auto* language_label = gtk_label_new(tr("Язык интерфейса"));
    gtk_label_set_xalign(GTK_LABEL(language_label), 0.0f);
    gtk_box_append(GTK_BOX(content), language_label);
    auto* languages = gtk_string_list_new(nullptr);
    const char* language_names[] = {tr("Язык системы"), "Русский", "English", "Español", "Português (Brasil)",
                                    "Français", "Deutsch", "简体中文", "日本語", "한국어", "Polski"};
    for (const auto* name : language_names) gtk_string_list_append(languages, name);
    auto* dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(languages), nullptr));
    guint active = 0;
    for (guint index = 0; index < G_N_ELEMENTS(language_codes); ++index)
        if (app->language == language_codes[index]) { active = index; break; }
    gtk_drop_down_set_selected(dropdown, active);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(dropdown));
    adw_alert_dialog_set_extra_child(dialog, content);
    g_object_set_data(G_OBJECT(dialog), "language-dropdown", dropdown);
    g_signal_connect(dialog, "response", G_CALLBACK(settings_response), app);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app->application))));
}
