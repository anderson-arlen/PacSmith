#pragma once

#include <QStringList>
#include <QTextStream>

namespace pacsmith {
class LibraryClient;
}

int runConnectCommand(const QStringList &arguments, QTextStream &out, QTextStream &errorStream);
int runServerCommand(const QStringList &arguments, pacsmith::LibraryClient &library,
                     QTextStream &out, QTextStream &errorStream);
int runClientsCommand(const QStringList &arguments, pacsmith::LibraryClient &library,
                      QTextStream &out, QTextStream &errorStream);
