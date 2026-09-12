#pragma once
#include "core/app_settings.hpp"
#include <optional>
class QWidget;
namespace pacsmith::gui {
std::optional<HarnessProfile> chooseRegistryAgent(QWidget *parent);
}
