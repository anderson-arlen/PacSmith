#include "core_tests.hpp"

#include "core/ai_service.hpp"
#include "core/ai_model_catalog_service.hpp"
#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/apt_repository.hpp"
#include "core/apt_update_service.hpp"
#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/credential_store.hpp"
#include "core/chatgpt_auth.hpp"
#include "core/dependency_parser.hpp"
#include "core/deb_analyzer.hpp"
#include "core/github_update_service.hpp"
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
#include "core/rpm_repository.hpp"
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

void CoreTests::parsesAiModelCatalog() {
    QString error;
    const auto models = pacsmith::AiModelCatalogService::parseModelIds(
        QByteArrayLiteral(R"({"object":"list","data":[{"id":"gpt-z"},{"id":"gpt-a"},{"id":"gpt-a"}]})"),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(models, QStringList({QStringLiteral("gpt-a"), QStringLiteral("gpt-z")}));

    const auto invalid = pacsmith::AiModelCatalogService::parseModelIds(
        QByteArrayLiteral(R"({"object":"list"})"), &error);
    QVERIFY(invalid.isEmpty());
    QVERIFY(!error.isEmpty());
}

void CoreTests::parsesChatGptCredentialsAndCatalog() {
    const QJsonObject claims{
        {QStringLiteral("https://api.openai.com/profile"),
         QJsonObject{{QStringLiteral("email"), QStringLiteral("user@example.com")}}},
        {QStringLiteral("https://api.openai.com/auth"),
         QJsonObject{{QStringLiteral("chatgpt_account_id"), QStringLiteral("acct-test")},
                     {QStringLiteral("chatgpt_plan_type"), QStringLiteral("plus")}}}};
    const auto payload = QJsonDocument(claims).toJson(QJsonDocument::Compact).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const auto token = QByteArrayLiteral("header.") + payload + QByteArrayLiteral(".signature");
    const QJsonObject response{{QStringLiteral("access_token"), QString::fromLatin1(token)},
                               {QStringLiteral("refresh_token"), QStringLiteral("refresh-test")},
                               {QStringLiteral("expires_in"), 3600}};
    QString error;
    const auto credentials = pacsmith::parseChatGptTokenResponse(
        QJsonDocument(response).toJson(QJsonDocument::Compact), {}, &error);
    QVERIFY2(credentials.has_value(), qPrintable(error));
    QCOMPARE(credentials->accountId, QStringLiteral("acct-test"));
    QCOMPARE(credentials->email, QStringLiteral("user@example.com"));
    QCOMPARE(credentials->planType, QStringLiteral("plus"));
    QVERIFY(credentials->expiresAtMs > QDateTime::currentMSecsSinceEpoch());
    const auto restored = pacsmith::ChatGptCredentials::fromSerialized(credentials->serialize(), &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->refreshToken, QStringLiteral("refresh-test"));

    const auto models = pacsmith::AiModelCatalogService::parseChatGptModelIds(
        QByteArrayLiteral(
            R"({"models":[{"slug":"gpt-visible","visibility":"list"},{"slug":"gpt-hidden","visibility":"hide"},{"slug":"gpt-disabled","show_in_picker":false},{"id":"gpt-fallback"}]})"),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(models, QStringList({QStringLiteral("gpt-visible"), QStringLiteral("gpt-fallback")}));
}

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

void CoreTests::encryptsCredentialsWithAge() {
    if (!pacsmith::CredentialStore::ageAvailable()) QSKIP("age is not installed");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.path() + QStringLiteral("/secrets.age");
    const QString password = QStringLiteral("pacsmith automated test password");
    {
        pacsmith::CredentialStore store(path);
        QString error;
        QVERIFY(!store.unlockAge(password, &error));
        QVERIFY2(store.createAge(password, &error), qPrintable(error));
        QVERIFY(store.hasAgeFile());
        QVERIFY(store.ageUnlocked());
        QVERIFY2(store.store(QStringLiteral("openai"), pacsmith::CredentialSource::Age,
                             QStringLiteral("test-secret-value"), password, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        const auto encrypted = QFileInfo(path).size();
        QVERIFY(encrypted > 0);
    }
    {
        pacsmith::CredentialStore store(path);
        QString error;
        QVERIFY2(store.unlockAge(password, &error), qPrintable(error));
        const auto secret = store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error);
        QVERIFY2(secret.has_value(), qPrintable(error));
        QCOMPARE(*secret, QStringLiteral("test-secret-value"));
        QVERIFY2(store.store(QStringLiteral("github"), pacsmith::CredentialSource::Age,
                             QStringLiteral("gh-token"), {}, &error), qPrintable(error));
        const auto github = store.load(QStringLiteral("github"), pacsmith::CredentialSource::Age, &error);
        QVERIFY2(github.has_value(), qPrintable(error));
        QCOMPARE(*github, QStringLiteral("gh-token"));
        QVERIFY2(store.remove(QStringLiteral("openai"), pacsmith::CredentialSource::Age,
                              {}, &error), qPrintable(error));
        QVERIFY(!store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error));
        store.lockAge();
        QVERIFY(!store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error));
    }
}

void CoreTests::usesInjectedGithubTokenWhenAgeIsLocked() {
    const auto previous = qgetenv("PACSMITH_GITHUB_TOKEN");
    QVERIFY(qputenv("PACSMITH_GITHUB_TOKEN", QByteArrayLiteral("injected-github-token")));
    pacsmith::CredentialStore store(QStringLiteral("/nonexistent/pacsmith-secrets.age"));
    QVERIFY(!store.ageUnlocked());
    QString error;
    const auto token = store.load(QStringLiteral("github"), pacsmith::CredentialSource::Age, &error);
    QVERIFY2(token.has_value(), qPrintable(error));
    QCOMPARE(*token, QStringLiteral("injected-github-token"));
    if (previous.isEmpty()) qunsetenv("PACSMITH_GITHUB_TOKEN");
    else QVERIFY(qputenv("PACSMITH_GITHUB_TOKEN", previous));
}

