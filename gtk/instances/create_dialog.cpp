#include "create_dialog.hpp"
#include "instance_list.hpp"
#include "../ui/install_progress.hpp"
#include "../app/app.hpp"
#include "../app/i18n.hpp"
#include "../minecraft/installer.hpp"
#include "../minecraft/version_catalog.hpp"

#include <adwaita.h>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <utility>

namespace fs = std::filesystem;

struct VersionLoad {
    App* app;
    AdwAlertDialog* dialog;
    GtkStack* stack;
    GtkDropDown* dropdown;
    GtkStringList* versions;
};

struct InstallRequest {
    App* app;
    fs::path directory;
    std::string id, name, version;
    std::shared_ptr<InstallProgress> progress;
};

static void create_response(AdwAlertDialog* dialog, const char* response, gpointer data)
{
    auto* app = static_cast<App*>(data);
    if (response && std::string(response) == "create") {
        auto* name = GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "name-entry"));
        auto* version_stack = GTK_STACK(g_object_get_data(G_OBJECT(dialog), "version-stack"));
        auto* version_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "version-entry"));
        auto* version_dropdown = GTK_DROP_DOWN(g_object_get_data(G_OBJECT(dialog), "version-dropdown"));
        std::string name_text = gtk_editable_get_text(GTK_EDITABLE(name));
        std::string version_text;
        if (std::string(gtk_stack_get_visible_child_name(version_stack)) == "versions") {
            auto* selected = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(version_dropdown));
            if (selected) version_text = gtk_string_object_get_string(selected);
        } else {
            version_text = gtk_editable_get_text(GTK_EDITABLE(version_entry));
        }
        if (!name_text.empty() && std::regex_match(version_text, std::regex("[0-9]+\\.[0-9]+(\\.[0-9]+)?"))) {
            std::string id = std::regex_replace(name_text, std::regex("[^A-Za-z0-9_.-]+"), "-");
            if (id.empty() || id == "." || id == "..") id = "instance";
            std::string base = id;
            for (unsigned suffix = 2; fs::exists(app->instance_dir / id); ++suffix)
                id = base + "-" + std::to_string(suffix);
            fs::path path = app->instance_dir / id;
            std::error_code error;
            fs::create_directories(app->instance_dir, error);
            if (error || !fs::create_directory(path, error) || error) {
                notify(app, error ? error.message() : tr("Экземпляр с таким именем уже существует"));
                return;
            }
            for (auto& c : name_text) if (c == '\n' || c == '\r' || c == '=') c = ' ';
            auto* request = new InstallRequest{app, path, id, name_text, version_text, show_install_progress(app)};
            auto* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* result, gpointer user_data) {
                std::unique_ptr<InstallRequest> request(static_cast<InstallRequest*>(user_data));
                auto* error_text = static_cast<std::string*>(g_task_propagate_pointer(G_TASK(result), nullptr));
                std::unique_ptr<std::string> error(error_text);
                close_install_progress(request->progress);
                if (error && error->empty()) {
                    populate(request->app);
                    notify(request->app, tr("Minecraft установлен, экземпляр готов к офлайн-запуску"));
                } else {
                    std::error_code cleanup_error;
                    fs::remove_all(request->directory, cleanup_error);
                    populate(request->app);
                    notify(request->app, error ? *error : tr("Не удалось установить Minecraft"));
                }
            }, request);
            g_task_set_task_data(task, request, nullptr);
            g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer task_data, GCancellable*) {
                auto* request = static_cast<InstallRequest*>(task_data);
                std::string error;
                Minecraft::installVanilla(request->version, request->directory, request->directory, request->name,
                    [progress = request->progress](const Minecraft::ProgressInfo& update) {
                        queue_install_progress(progress, update);
                    }, error);
                g_task_return_pointer(task, new std::string(std::move(error)), [](gpointer value) { delete static_cast<std::string*>(value); });
            });
            g_object_unref(task);
        } else notify(app, tr("Укажите название и версию вида 1.21.1"));
    }
}

static bool create_form_is_valid(AdwAlertDialog* dialog)
{
    auto* name = GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "name-entry"));
    auto* stack = GTK_STACK(g_object_get_data(G_OBJECT(dialog), "version-stack"));
    auto* entry = GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "version-entry"));
    auto* dropdown = GTK_DROP_DOWN(g_object_get_data(G_OBJECT(dialog), "version-dropdown"));
    const char* name_text = gtk_editable_get_text(GTK_EDITABLE(name));
    std::string version_text;
    const char* page = gtk_stack_get_visible_child_name(stack);
    if (page && std::string(page) == "versions") {
        auto* selected = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(dropdown));
        if (selected) version_text = gtk_string_object_get_string(selected);
    } else if (page && std::string(page) == "manual") {
        version_text = gtk_editable_get_text(GTK_EDITABLE(entry));
    }
    return name_text && *name_text && std::regex_match(version_text, std::regex("[0-9]+\\.[0-9]+(\\.[0-9]+)?"));
}

static void refresh_create_response(AdwAlertDialog* dialog)
{
    adw_alert_dialog_set_response_enabled(dialog, "create", create_form_is_valid(dialog));
}

static void create_form_changed(GtkEditable*, gpointer data)
{
    refresh_create_response(ADW_ALERT_DIALOG(data));
}

static void create_version_selected(GObject*, GParamSpec*, gpointer data)
{
    refresh_create_response(ADW_ALERT_DIALOG(data));
}

static void fetch_versions_thread(GTask* task, gpointer, gpointer task_data, GCancellable* cancellable)
{
    auto result = Minecraft::VersionCatalog::fetch_releases(cancellable);
    g_task_return_pointer(task, new Minecraft::VersionCatalog::Result(std::move(result)),
                          [](gpointer data) { delete static_cast<Minecraft::VersionCatalog::Result*>(data); });
    (void)task_data;
}

static void versions_loaded(GObject*, GAsyncResult* async_result, gpointer data)
{
    std::unique_ptr<VersionLoad> request(static_cast<VersionLoad*>(data));
    GError* error = nullptr;
    std::unique_ptr<Minecraft::VersionCatalog::Result> result(static_cast<Minecraft::VersionCatalog::Result*>(g_task_propagate_pointer(G_TASK(async_result), &error)));
    if (result && result->error.empty()) {
        for (const auto& version : result->versions) gtk_string_list_append(request->versions, version.c_str());
        gtk_drop_down_set_model(request->dropdown, G_LIST_MODEL(request->versions));
        gtk_drop_down_set_selected(request->dropdown, 0);
        gtk_stack_set_visible_child_name(request->stack, "versions");
        refresh_create_response(request->dialog);
    } else {
        gtk_stack_set_visible_child_name(request->stack, "manual");
        refresh_create_response(request->dialog);
        notify(request->app, result ? result->error : (error ? error->message : tr("Не удалось загрузить список версий")));
    }
    if (error) g_error_free(error);
    g_object_unref(request->dialog);
    g_object_unref(request->stack);
    g_object_unref(request->dropdown);
    g_object_unref(request->versions);
}

static void load_minecraft_versions(App* app, AdwAlertDialog* dialog, GtkStack* stack,
                                   GtkDropDown* dropdown, GtkStringList* versions)
{
    auto* request = new VersionLoad{app,
                                    ADW_ALERT_DIALOG(g_object_ref(dialog)),
                                    GTK_STACK(g_object_ref(stack)),
                                    GTK_DROP_DOWN(g_object_ref(dropdown)),
                                    GTK_STRING_LIST(g_object_ref(versions))};
    auto* task = g_task_new(nullptr, nullptr, versions_loaded, request);
    g_task_run_in_thread(task, fetch_versions_thread);
    g_object_unref(task);
}

void create_instance(GtkButton*, gpointer data)
{
    auto* app = static_cast<App*>(data);
    auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Новый экземпляр"), tr("Создайте чистый экземпляр Minecraft Java Edition.")));
    adw_alert_dialog_add_responses(dialog, "cancel", tr("Отмена"), "create", tr("Создать"), nullptr);
    adw_alert_dialog_set_default_response(dialog, "create");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    adw_alert_dialog_set_response_enabled(dialog, "create", FALSE);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    auto* name = GTK_ENTRY(gtk_entry_new());
    auto* version_entry = GTK_ENTRY(gtk_entry_new());
    auto* version_stack = GTK_STACK(gtk_stack_new());
    auto* loading = gtk_label_new(tr("Загружаю список версий…"));
    auto* versions = gtk_string_list_new(nullptr);
    auto* dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(g_object_ref(versions)), nullptr));
    gtk_entry_set_placeholder_text(name, tr("Название"));
    gtk_entry_set_placeholder_text(version_entry, tr("Версия Minecraft, например 1.21.1 (можно ввести вручную)"));
    gtk_stack_add_named(version_stack, loading, "loading");
    gtk_stack_add_named(version_stack, GTK_WIDGET(dropdown), "versions");
    gtk_stack_add_named(version_stack, GTK_WIDGET(version_entry), "manual");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(name));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(version_stack));
    adw_alert_dialog_set_extra_child(dialog, box);
    g_object_set_data(G_OBJECT(dialog), "name-entry", name);
    g_object_set_data(G_OBJECT(dialog), "version-entry", version_entry);
    g_object_set_data(G_OBJECT(dialog), "version-stack", version_stack);
    g_object_set_data(G_OBJECT(dialog), "version-dropdown", dropdown);
    g_signal_connect(name, "changed", G_CALLBACK(create_form_changed), dialog);
    g_signal_connect(version_entry, "changed", G_CALLBACK(create_form_changed), dialog);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(create_version_selected), dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(create_response), app);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app->application))));
    load_minecraft_versions(app, dialog, version_stack, dropdown, versions);
    g_object_unref(versions);
}

