#pragma once

#include <libintl.h>

inline const char* tr(const char* message)
{
    return gettext(message);
}
