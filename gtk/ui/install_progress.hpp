#pragma once

#include "../app/app.hpp"
#include "../minecraft/installer.hpp"

#include <memory>

struct InstallProgress;

std::shared_ptr<InstallProgress> show_install_progress(App* app);
void queue_install_progress(const std::shared_ptr<InstallProgress>& progress,
                            const Minecraft::ProgressInfo& info);
void close_install_progress(const std::shared_ptr<InstallProgress>& progress);
