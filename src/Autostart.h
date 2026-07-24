#pragma once

#include "Messages.h"

namespace Autostart {

// Cached after the first query; the app is the sole owner of the task.
bool IsEnabled();
bool SetEnabled(bool enable, ErrorMsg& error);

}  // namespace Autostart
