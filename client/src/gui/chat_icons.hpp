#pragma once
#include <QIcon>

namespace pacsmith::gui {
enum class ChatIcon { Settings, Image, Send, Stop };
QIcon chatIcon(ChatIcon kind);
}
