#pragma once

#include <adwaita.h>
#include <filesystem>
#include <set>
#include <string>

struct App {
    AdwApplication* application = nullptr;
    GtkListBox* list = nullptr;
    GtkWidget* scroll = nullptr;
    AdwToastOverlay* toasts = nullptr;
    GtkWidget* empty = nullptr;
    GtkButton* play_button = nullptr;
    GtkEntry* username_entry = nullptr;
    std::filesystem::path root;
    std::filesystem::path instance_dir;
    std::string language;
    std::set<std::string> running_instances;
};

void notify(App* app, const std::string& message);
void activate(GApplication* application, gpointer data);
