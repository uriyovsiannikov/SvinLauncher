#include "app.hpp"

void notify(App* app, const std::string& message)
{
    adw_toast_overlay_add_toast(app->toasts, adw_toast_new(message.c_str()));
}
