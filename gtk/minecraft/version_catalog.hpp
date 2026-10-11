#pragma once

#include <gio/gio.h>
#include <string>
#include <vector>

namespace Minecraft::VersionCatalog {
struct Result {
    std::vector<std::string> versions;
    std::string error;
};

Result fetch_releases(GCancellable* cancellable);
}
