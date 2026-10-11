#include "install_progress.hpp"
#include "../app/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <string>

struct InstallProgress {
    GtkWindow* window = nullptr;
    GtkLabel* label = nullptr;
    GtkLabel* detail = nullptr;
    GtkProgressBar* bar = nullptr;
    guint pulse_source = 0;
    bool indeterminate = true;
    bool finished = false;
    std::string last_stage;
    uint64_t last_transferred = 0;
    gint64 last_sample_time = 0;
    double speed_bytes_per_second = 0.0;
};

struct InstallProgressUpdate {
    std::shared_ptr<InstallProgress> progress;
    Minecraft::ProgressInfo info;
};

static gboolean pulse_install_progress(gpointer data)
{
    auto* progress = static_cast<std::shared_ptr<InstallProgress>*>(data);
    if ((*progress)->finished) return G_SOURCE_REMOVE;
    if ((*progress)->indeterminate) gtk_progress_bar_pulse((*progress)->bar);
    return G_SOURCE_CONTINUE;
}

static gboolean prevent_install_progress_close(GtkWindow*, gpointer)
{
    return TRUE;
}

static std::string format_duration(uint64_t seconds)
{
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remainder = seconds % 60;
    if (hours) return std::to_string(hours) + " " + tr("ч") + " " + std::to_string(minutes) + " " + tr("мин");
    if (minutes) return std::to_string(minutes) + " " + tr("мин") + " " + std::to_string(remainder) + " " + tr("с");
    return std::to_string(remainder) + " " + tr("с");
}

void queue_install_progress(const std::shared_ptr<InstallProgress>& progress,
                                   const Minecraft::ProgressInfo& info)
{
    auto* update = new InstallProgressUpdate{progress, info};
    auto* source = g_idle_source_new();
    g_source_set_callback(source, [](gpointer data) -> gboolean {
        auto* update = static_cast<InstallProgressUpdate*>(data);
        auto& progress = *update->progress;
        if (progress.finished) return G_SOURCE_REMOVE;
        const auto& info = update->info;
        gtk_label_set_text(progress.label, info.stage.c_str());
        std::string detail = info.current_file.empty() ? std::string{} : info.current_file;
        const auto now = g_get_monotonic_time();
        if (!progress.last_sample_time || progress.last_stage != info.stage ||
            info.transferred_bytes < progress.last_transferred) {
            progress.speed_bytes_per_second = 0.0;
            progress.last_sample_time = now;
            progress.last_transferred = info.transferred_bytes;
            progress.last_stage = info.stage;
        } else if (now - progress.last_sample_time >= 500000) {
            const auto elapsed = static_cast<double>(now - progress.last_sample_time) / 1000000.0;
            const auto instantaneous = static_cast<double>(info.transferred_bytes - progress.last_transferred) / elapsed;
            progress.speed_bytes_per_second = progress.speed_bytes_per_second == 0.0 ? instantaneous :
                progress.speed_bytes_per_second * 0.65 + instantaneous * 0.35;
            progress.last_sample_time = now;
            progress.last_transferred = info.transferred_bytes;
        }
        if (progress.speed_bytes_per_second > 0.0) {
            auto* rate = g_format_size(static_cast<guint64>(progress.speed_bytes_per_second));
            detail += (detail.empty() ? "" : " · ") + std::string(rate) + "/с";
            g_free(rate);
            if (info.total_bytes > info.transferred_bytes) {
                const auto remaining = static_cast<uint64_t>(std::ceil(
                    static_cast<double>(info.total_bytes - info.transferred_bytes) / progress.speed_bytes_per_second));
                detail += " · " + std::string(tr("Осталось")) + " " + format_duration(remaining);
            }
        }
        if (info.total_files) {
            detail += (detail.empty() ? "" : " · ") + std::string(tr("Файлов: ")) +
                std::to_string(info.completed_files) + "/" + std::to_string(info.total_files);
        }
        gtk_label_set_text(progress.detail, detail.c_str());
        progress.indeterminate = info.total_bytes == 0 && info.total_files == 0;
        if (progress.indeterminate) {
            gtk_progress_bar_set_text(progress.bar, "");
        } else if (info.total_bytes) {
            const auto fraction = std::min(1.0, static_cast<double>(info.transferred_bytes) /
                                                   static_cast<double>(info.total_bytes));
            gtk_progress_bar_set_fraction(progress.bar, fraction);
            auto* done = g_format_size(info.transferred_bytes);
            auto* total = g_format_size(info.total_bytes);
            const auto text = std::string(done) + " / " + total;
            gtk_progress_bar_set_text(progress.bar, text.c_str());
            g_free(done);
            g_free(total);
        } else {
            gtk_progress_bar_set_fraction(progress.bar,
                static_cast<double>(info.completed_files) / static_cast<double>(info.total_files));
            const auto count = std::to_string(info.completed_files) + " / " + std::to_string(info.total_files) + " " + tr("файлов");
            gtk_progress_bar_set_text(progress.bar, count.c_str());
        }
        return G_SOURCE_REMOVE;
    }, update, [](gpointer data) { delete static_cast<InstallProgressUpdate*>(data); });
    g_source_attach(source, g_main_context_default());
    g_source_unref(source);
}

std::shared_ptr<InstallProgress> show_install_progress(App* app)
{
    auto progress = std::make_shared<InstallProgress>();
    progress->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_application(progress->window, GTK_APPLICATION(app->application));
    gtk_window_set_title(progress->window, tr("Установка Minecraft"));
    gtk_window_set_default_size(progress->window, 560, -1);
    gtk_window_set_modal(progress->window, TRUE);
    gtk_window_set_deletable(progress->window, FALSE);
    g_signal_connect(progress->window, "close-request", G_CALLBACK(prevent_install_progress_close), nullptr);
    if (auto* parent = gtk_application_get_active_window(GTK_APPLICATION(app->application)))
        gtk_window_set_transient_for(progress->window, parent);

    auto* box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_margin_top(GTK_WIDGET(box), 24);
    gtk_widget_set_margin_bottom(GTK_WIDGET(box), 24);
    gtk_widget_set_margin_start(GTK_WIDGET(box), 24);
    gtk_widget_set_margin_end(GTK_WIDGET(box), 24);
    progress->label = GTK_LABEL(gtk_label_new(tr("Подготовка установки…")));
    gtk_label_set_xalign(progress->label, 0.0f);
    progress->detail = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(progress->detail, 0.0f);
    gtk_label_set_ellipsize(progress->detail, PANGO_ELLIPSIZE_MIDDLE);
    progress->bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(progress->bar, TRUE);
    gtk_progress_bar_set_text(progress->bar, "");
    gtk_box_append(box, GTK_WIDGET(progress->label));
    gtk_box_append(box, GTK_WIDGET(progress->detail));
    gtk_box_append(box, GTK_WIDGET(progress->bar));
    gtk_window_set_child(progress->window, GTK_WIDGET(box));
    progress->pulse_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, pulse_install_progress,
        new std::shared_ptr<InstallProgress>(progress), [](gpointer data) {
            delete static_cast<std::shared_ptr<InstallProgress>*>(data);
        });
    gtk_window_present(progress->window);
    return progress;
}

void close_install_progress(const std::shared_ptr<InstallProgress>& progress)
{
    progress->finished = true;
    if (progress->pulse_source) {
        g_source_remove(progress->pulse_source);
        progress->pulse_source = 0;
    }
    gtk_window_destroy(progress->window);
}
