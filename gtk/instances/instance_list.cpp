#include "instance_list.hpp"
#include "instance_store.hpp"
#include "launch.hpp"
#include "../app/app.hpp"
#include "../app/i18n.hpp"

#include <adwaita.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static void request_delete(GtkButton* button, gpointer data);

void dismiss_actions(GtkButton* button)
{
    if (!button) return;
    auto* popover = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    if (popover) gtk_popover_popdown(popover);
}

static void open_folder(GtkButton* button, gpointer data)
{
    dismiss_actions(button);
    auto* row = GTK_LIST_BOX_ROW(data);
    auto* app = static_cast<App*>(g_object_get_data(G_OBJECT(row), "launcher-app"));
    auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "instance-id"));
    auto* uri = g_filename_to_uri((app->instance_dir / id).c_str(), nullptr, nullptr);
    GError* error = nullptr;
    if (!uri || !g_app_info_launch_default_for_uri(uri, nullptr, &error))
        notify(app, error ? error->message : tr("Не удалось открыть папку экземпляра"));
    if (error) g_error_free(error);
    g_free(uri);
}

void populate(App* app)
{
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(app->list)))
        gtk_list_box_remove(app->list, child);
    auto entries = list_instances(app->instance_dir);
    gtk_widget_set_visible(app->empty, entries.empty());
    gtk_widget_set_visible(app->scroll, !entries.empty());
    for (const auto& item : entries) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item.name.c_str());
        const auto subtitle = std::string(tr("Minecraft")) + " " + item.version;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
        auto* id = g_strdup(item.id.c_str());
        g_object_set_data_full(G_OBJECT(row), "instance-id", id, g_free);
        g_object_set_data(G_OBJECT(row), "launcher-app", app);

        auto* menu_button = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "view-more-symbolic");
        gtk_widget_set_tooltip_text(menu_button, tr("Дополнительные действия"));
        auto* popover = gtk_popover_new();
        auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(actions, 6);
        gtk_widget_set_margin_bottom(actions, 6);
        gtk_widget_set_margin_start(actions, 6);
        gtk_widget_set_margin_end(actions, 6);
        auto* folder = gtk_button_new_with_label(tr("Открыть папку"));
        g_signal_connect(folder, "clicked", G_CALLBACK(open_folder), row);
        gtk_box_append(GTK_BOX(actions), folder);
        auto* remove = gtk_button_new_with_label(tr("Удалить"));
        gtk_widget_add_css_class(remove, "destructive-action");
        g_signal_connect(remove, "clicked", G_CALLBACK(request_delete), row);
        gtk_box_append(GTK_BOX(actions), remove);
        gtk_popover_set_child(GTK_POPOVER(popover), actions);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button), popover);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), menu_button);
        gtk_list_box_append(app->list, GTK_WIDGET(row));
    }
    if (entries.empty()) {
        if (app->play_button) gtk_widget_set_sensitive(GTK_WIDGET(app->play_button), FALSE);
    } else {
        gtk_list_box_select_row(app->list, gtk_list_box_get_row_at_index(app->list, 0));
    }
}

static void instance_selected(GtkListBox*, GtkListBoxRow* row, gpointer data)
{
    auto* app = static_cast<App*>(data);
    if (app->play_button)
        gtk_widget_set_sensitive(GTK_WIDGET(app->play_button), row != nullptr);
}

static void play_selected(GtkButton*, gpointer data)
{
    auto* app = static_cast<App*>(data);
    if (auto* row = gtk_list_box_get_selected_row(app->list))
        launch_offline(nullptr, row);
}

static void delete_response(AdwAlertDialog* dialog, const char* response, gpointer data)
{
    auto* app = static_cast<App*>(data);
    if (response && std::string(response) == "delete") {
        auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(dialog), "instance-id"));
        const std::string instance_id = id ? id : "";
        if (app->running_instances.contains(instance_id)) {
            notify(app, tr("Сначала закройте запущенную игру"));
            return;
        }
        const fs::path target = app->instance_dir / instance_id;
        std::error_code error;
        const auto normalized_parent = fs::weakly_canonical(app->instance_dir, error);
        if (error || instance_id.empty() || instance_id == "." || instance_id == ".." ||
            instance_id.find_first_of("/\\") != std::string::npos || !fs::is_directory(target, error) || error ||
            fs::is_symlink(target, error) || error || !fs::exists(target / "instance.cfg", error) || error ||
            fs::weakly_canonical(target.parent_path(), error) != normalized_parent || error) {
            notify(app, tr("Экземпляр не найден или его путь некорректен"));
            return;
        }
        const auto removed_entries = fs::remove_all(target, error);
        if (error) notify(app, std::string(tr("Не удалось удалить экземпляр: ")) + error.message());
        else if (removed_entries == 0 || fs::exists(target, error) || error)
            notify(app, tr("Не удалось полностью удалить файлы экземпляра"));
        else {
            std::string cache_error;
            if (list_instances(app->instance_dir).empty()) {
                std::error_code cache_ec;
                fs::remove_all(app->root / "libraries", cache_ec);
                if (cache_ec) cache_error = cache_ec.message();
                cache_ec.clear();
                fs::remove_all(app->root / "assets", cache_ec);
                if (cache_ec) {
                    if (!cache_error.empty()) cache_error += "; ";
                    cache_error += cache_ec.message();
                }
            }
            populate(app);
            notify(app, cache_error.empty() ? tr("Экземпляр удалён") :
                   std::string(tr("Экземпляр удалён, но не весь общий кэш удалось очистить: ")) + cache_error);
        }
    }
}

static void request_delete(GtkButton* button, gpointer data)
{
    dismiss_actions(button);
    auto* row = GTK_LIST_BOX_ROW(data);
    auto* app = static_cast<App*>(g_object_get_data(G_OBJECT(row), "launcher-app"));
    auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "instance-id"));
    auto* title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
    auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(nullptr, tr("Файлы экземпляра и ярлыки будут удалены без возможности восстановления.")));
    adw_alert_dialog_format_heading(dialog, tr("Удалить экземпляр «%s»?"), title);
    adw_alert_dialog_add_responses(dialog, "cancel", tr("Отмена"), "delete", tr("Удалить"), nullptr);
    adw_alert_dialog_set_response_appearance(dialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_object_set_data_full(G_OBJECT(dialog), "instance-id", g_strdup(id), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(delete_response), app);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app->application))));
}


void bind_instance_list(App* app)
{
    g_signal_connect(app->list, "row-selected", G_CALLBACK(instance_selected), app);
}

void bind_play_button(GtkButton* button, App* app)
{
    g_signal_connect(button, "clicked", G_CALLBACK(play_selected), app);
}
