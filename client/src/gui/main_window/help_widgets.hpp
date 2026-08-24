#pragma once

#include "core/model.hpp"

#include <QString>

class QFrame;
class QLabel;
class QWidget;

namespace pacsmith::gui {

void setSettingsSectionHelp(QLabel *label, const QString &summary, const QString &details,
                            const QString &commands = {});
QLabel *settingsSectionHelp(QWidget *parent, const QString &summary, const QString &details,
                            const QString &commands = {});
QLabel *pageIntroduction(const QString &summary, QWidget *parent, const QString &details = {});
QFrame *settingsStatusFrame(QWidget *parent);
void applySourcePackageTypeHelp(QLabel *label, SourcePackageType type);

} // namespace pacsmith::gui
