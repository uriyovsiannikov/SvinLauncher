#pragma once

#include "../app/app.hpp"

void populate(App* app);
void bind_instance_list(App* app);
void bind_play_button(GtkButton* button, App* app);
void dismiss_actions(GtkButton* button);
