#include "app.hpp"
#include "i18n.hpp"
#include "settings.hpp"
#include "../instances/create_dialog.hpp"
#include "../instances/instance_list.hpp"
#include "../instances/instance_store.hpp"

#include <adwaita.h>

void activate(GApplication* application, gpointer data)
{
    auto* app = static_cast<App*>(data);
    auto* window = adw_application_window_new(GTK_APPLICATION(application));
    gtk_window_set_title(GTK_WINDOW(window), "SvinLauncher");
    gtk_window_set_icon_name(GTK_WINDOW(window), SVIN_APP_ID);
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 560);
    auto* toolbar = adw_toolbar_view_new();
    auto* header = ADW_HEADER_BAR(adw_header_bar_new());
    auto* add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, tr("Создать экземпляр"));
    g_signal_connect(add, "clicked", G_CALLBACK(create_instance), app);
    adw_header_bar_pack_start(header, add);
    auto* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, tr("Обновить список"));
    g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(populate), app);
    adw_header_bar_pack_end(header, refresh);
    auto* settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, tr("Настройки лаунчера"));
    g_signal_connect(settings, "clicked", G_CALLBACK(show_settings), app);
    adw_header_bar_pack_end(header, settings);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(header));
    auto* overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    app->toasts = overlay;
    auto* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    app->empty = adw_status_page_new();
    gtk_widget_set_vexpand(app->empty, TRUE);
    adw_status_page_set_title(ADW_STATUS_PAGE(app->empty), tr("Нет экземпляров"));
    adw_status_page_set_description(ADW_STATUS_PAGE(app->empty), tr("Создайте vanilla экземпляр, чтобы начать играть."));
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(app->empty), "applications-games-symbolic");
    auto* create = gtk_button_new_with_label(tr("Создать экземпляр"));
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_halign(create, GTK_ALIGN_CENTER);
    g_signal_connect(create, "clicked", G_CALLBACK(create_instance), app);
    adw_status_page_set_child(ADW_STATUS_PAGE(app->empty), create);
    gtk_box_append(GTK_BOX(body), app->empty);
    auto* scroll = gtk_scrolled_window_new();
    app->scroll = scroll;
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(app->list), "boxed-list");
    gtk_list_box_set_show_separators(app->list, TRUE);
    gtk_widget_set_margin_start(GTK_WIDGET(app->list), 18);
    gtk_widget_set_margin_end(GTK_WIDGET(app->list), 18);
    gtk_widget_set_margin_top(GTK_WIDGET(app->list), 12);
    gtk_list_box_set_selection_mode(app->list, GTK_SELECTION_SINGLE);
    bind_instance_list(app);
    populate(app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(app->list));
    gtk_box_append(GTK_BOX(body), scroll);
    adw_toast_overlay_set_child(overlay, body);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(overlay));
    auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(footer, 12);
    gtk_widget_set_margin_bottom(footer, 12);
    gtk_widget_set_margin_start(footer, 18);
    gtk_widget_set_margin_end(footer, 18);
    app->username_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->username_entry, tr("Имя игрока (оффлайн)"));
    gtk_entry_set_max_length(app->username_entry, 16);
    gtk_widget_set_hexpand(GTK_WIDGET(app->username_entry), TRUE);
    const auto saved_player_name = read_setting(app->root / "svinlauncher.cfg", "PlayerName");
    if (!saved_player_name.empty()) gtk_editable_set_text(GTK_EDITABLE(app->username_entry), saved_player_name.c_str());
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(app->username_entry));
    app->play_button = GTK_BUTTON(gtk_button_new_with_label(tr("Играть")));
    gtk_widget_add_css_class(GTK_WIDGET(app->play_button), "suggested-action");
    gtk_widget_set_size_request(GTK_WIDGET(app->play_button), 150, 48);
    gtk_widget_set_halign(GTK_WIDGET(app->play_button), GTK_ALIGN_END);
    bind_play_button(app->play_button, app);
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(app->play_button));
    gtk_widget_set_sensitive(GTK_WIDGET(app->play_button), gtk_list_box_get_selected_row(app->list) != nullptr);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), footer);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar);
    gtk_window_present(GTK_WINDOW(window));
}
