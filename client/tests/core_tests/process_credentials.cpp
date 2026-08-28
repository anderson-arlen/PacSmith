#include "core_tests.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/dependency_parser.hpp"
#include "core/deb_analyzer.hpp"
#include "core/lifecycle_validator.hpp"
#include "core/path_safety.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/pkgbuild_install_plan.hpp"
#include "core/project_store/project_store.hpp"
#include "core/repository_trust.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/rpm_analyzer.hpp"
#include "core/script_evidence.hpp"
#include "core/source_analyzer.hpp"
#include "core/terminal_install_service.hpp"

#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <algorithm>
#include <filesystem>

void CoreTests::buildsExternalTerminalCommandsSafely() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList terminalNames{
        QStringLiteral("xdg-terminal-exec"), QStringLiteral("konsole"),
        QStringLiteral("gnome-terminal"), QStringLiteral("xfce4-terminal"),
        QStringLiteral("mate-terminal"), QStringLiteral("kitty"),
        QStringLiteral("alacritty"), QStringLiteral("foot"), QStringLiteral("xterm")};
    const auto packageArgument = QStringLiteral("/tmp/vendor package;$(touch should-not-run).pkg.tar.zst");
    const QStringList helperArguments{QStringLiteral("_install-session"),
                                      QStringLiteral("--package"), packageArgument};
    for (const auto &terminalName : terminalNames) {
        const auto terminalPath = temporary.filePath(terminalName);
        QFile terminal(terminalPath);
        QVERIFY(terminal.open(QIODevice::WriteOnly));
        QCOMPARE(terminal.write("#!/bin/sh\n"), 10);
        terminal.close();
        QVERIFY(QFile::setPermissions(terminalPath, QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner |
                                                       QFileDevice::ExeOwner));
        QString error;
        const auto command = pacsmith::TerminalLauncher::commandFor(
            terminalPath, {}, QStringLiteral("/usr/bin/true"), helperArguments, &error);
        QVERIFY2(command.has_value(), qPrintable(error));
        QCOMPARE(command->program, terminalPath);
        QVERIFY(command->arguments.size() >= helperArguments.size() + 1);
        const auto helperIndex = command->arguments.size() - helperArguments.size() - 1;
        QCOMPARE(command->arguments.at(helperIndex), QStringLiteral("/usr/bin/true"));
        QCOMPARE(command->arguments.sliced(helperIndex + 1), helperArguments);
        QVERIFY(!command->arguments.contains(QStringLiteral("sh")));
        QVERIFY(!command->arguments.contains(QStringLiteral("-c")));
        QCOMPARE(command->arguments.last(), packageArgument);
    }

    const auto kittyPath = temporary.filePath(QStringLiteral("kitty"));
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("TERMINAL"), kittyPath);
    QString error;
    const auto resolved = pacsmith::TerminalLauncher::resolve(
        QStringLiteral("/usr/bin/true"), helperArguments, environment, &error);
    QVERIFY2(resolved.has_value(), qPrintable(error));
    QCOMPARE(resolved->program, kittyPath);
}

void CoreTests::buildsNonInteractivePacmanArgumentsSafely() {
    const auto hostilePath = QStringLiteral("/tmp/vendor package;$(touch nope).pkg.tar.zst");
    QCOMPARE(pacsmith::InstallService::privilegeProgram(
                 pacsmith::InstallPrivilegeMode::TtySudo),
             QStringLiteral("/usr/bin/sudo"));
    QCOMPARE(pacsmith::InstallService::privilegeProgram(
                 pacsmith::InstallPrivilegeMode::Polkit),
             QStringLiteral("/usr/bin/pkexec"));
    QCOMPARE(pacsmith::InstallService::installArguments(hostilePath, true),
             QStringList({QStringLiteral("/usr/bin/pacman"), QStringLiteral("--noconfirm"),
                          QStringLiteral("-U"), QStringLiteral("--"), hostilePath}));
    QCOMPARE(pacsmith::InstallService::uninstallArguments(QStringLiteral("vendor-bin"), true),
             QStringList({QStringLiteral("/usr/bin/pacman"), QStringLiteral("--noconfirm"),
                          QStringLiteral("-R"), QStringLiteral("--"),
                          QStringLiteral("vendor-bin")}));
    const auto interactive = pacsmith::InstallService::installArguments(hostilePath, false);
    QVERIFY(!interactive.contains(QStringLiteral("--noconfirm")));
    QVERIFY(!interactive.contains(QStringLiteral("sh")));
    QVERIFY(!interactive.contains(QStringLiteral("-c")));
    QCOMPARE(interactive.last(), hostilePath);

    QString optionError;
    const auto ttyMode = pacsmith::parseInstallPrivilegeOptions({}, &optionError);
    QVERIFY(ttyMode.has_value());
    QCOMPARE(*ttyMode, pacsmith::InstallPrivilegeMode::TtySudo);
    const auto polkitMode = pacsmith::parseInstallPrivilegeOptions(
        {QStringLiteral("--polkit")}, &optionError);
    QVERIFY(polkitMode.has_value());
    QCOMPARE(*polkitMode, pacsmith::InstallPrivilegeMode::Polkit);
    QVERIFY(!pacsmith::parseInstallPrivilegeOptions(
                 {QStringLiteral("/tmp/untrusted.pkg.tar.zst")}, &optionError).has_value());
    QVERIFY(optionError.contains(QStringLiteral("only supported")));
    QVERIFY(!pacsmith::parseInstallPrivilegeOptions(
                 {QStringLiteral("--polkit"), QStringLiteral("--extra")},
                 &optionError).has_value());
}

void CoreTests::buildsRebuildableMakepkgArguments() {
    const auto arguments = pacsmith::BuildService::makepkgArguments();
    QCOMPARE(arguments, QStringList({QStringLiteral("--force"), QStringLiteral("--nodeps")}));
    QVERIFY(!arguments.contains(QStringLiteral("--skipchecksums")));
    QVERIFY(!arguments.contains(QStringLiteral("--skippgpcheck")));
}

void CoreTests::validatesInstallSessionProtocol() {
    const auto token = QString(64, QLatin1Char('a'));
    const pacsmith::InstallSessionEvent output{QStringLiteral("output"), token,
                                               QStringLiteral("line one\n$() ; quotes '\"\n")};
    QString error;
    const auto restoredOutput = pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(pacsmith::InstallSessionProtocol::encode(output).trimmed()), &error);
    QVERIFY2(restoredOutput.has_value(), qPrintable(error));
    QCOMPARE(restoredOutput->type, output.type);
    QCOMPARE(restoredOutput->token, token);
    QCOMPARE(restoredOutput->text, output.text);

    const pacsmith::InstallSessionEvent finished{QStringLiteral("finished"), token, {}, 17,
                                                 QProcess::CrashExit, true};
    const auto restoredFinished = pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(pacsmith::InstallSessionProtocol::encode(finished).trimmed()), &error);
    QVERIFY2(restoredFinished.has_value(), qPrintable(error));
    QCOMPARE(restoredFinished->exitCode, 17);
    QCOMPARE(restoredFinished->exitStatus, QProcess::CrashExit);
    QVERIFY(restoredFinished->canceled);

    QVERIFY(!pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(QByteArrayLiteral(R"({"type":"output","token":"short","text":"x"})")),
        &error));
    QVERIFY(error.contains(QStringLiteral("token")));
    QVERIFY(!pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(QByteArrayLiteral(
            R"({"type":"run-command","token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})")),
        &error));
    QVERIFY(error.contains(QStringLiteral("Unknown")));
}
