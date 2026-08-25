#pragma once

#include <QString>
#include <QStringList>
#include <QTextStream>

namespace pacsmith::cli {

[[nodiscard]] QString packagedPluginDirectory();
int runAgentIntegrationCommand(const QStringList &arguments, QTextStream &out,
                               QTextStream &errorStream);

} // namespace pacsmith::cli
