#include "core_tests.hpp"

#include "core/remote_import_service.hpp"

#include <QtTest>

#include <QRegularExpression>
#include <QUrl>

void CoreTests::parsesRemoteGitHubImportUrls() {
    QString error;
    const auto exact = pacsmith::RemoteImportService::parseGitHubUrl(
        QUrl(QStringLiteral("https://github.com/subframe7536/maple-font/releases/download/v7.9/MapleMono-NF-unhinted.zip")),
        {}, false, &error);
    QVERIFY2(exact.has_value(), qPrintable(error));
    QCOMPARE(exact->owner, QStringLiteral("subframe7536"));
    QCOMPARE(exact->repository, QStringLiteral("maple-font"));
    QCOMPARE(exact->requestedTag, QStringLiteral("v7.9"));
    QCOMPARE(exact->assetRegex,
             QRegularExpression::escape(QStringLiteral("MapleMono-NF-unhinted.zip")));
    QCOMPARE(exact->includePrereleases, false);

    const auto tagged = pacsmith::RemoteImportService::parseGitHubUrl(
        QUrl(QStringLiteral("https://www.github.com/vendor/product/releases/tag/2.4.1")),
        QStringLiteral("product-.*\\.AppImage"), true, &error);
    QVERIFY2(tagged.has_value(), qPrintable(error));
    QCOMPARE(tagged->owner, QStringLiteral("vendor"));
    QCOMPARE(tagged->repository, QStringLiteral("product"));
    QCOMPARE(tagged->requestedTag, QStringLiteral("2.4.1"));
    QCOMPARE(tagged->assetRegex, QStringLiteral("product-.*\\.AppImage"));
    QCOMPARE(tagged->includePrereleases, true);

    const auto repository = pacsmith::RemoteImportService::parseGitHubUrl(
        QUrl(QStringLiteral("https://github.com/vendor/product.git")), {}, false, &error);
    QVERIFY2(repository.has_value(), qPrintable(error));
    QCOMPARE(repository->repository, QStringLiteral("product"));
    QCOMPARE(repository->assetRegex, QStringLiteral(".*"));
    QVERIFY(repository->requestedTag.isEmpty());
}

void CoreTests::rejectsUnsafeRemoteGitHubImportUrls() {
    for (const auto &value : {
             QStringLiteral("http://github.com/vendor/product"),
             QStringLiteral("https://example.com/vendor/product"),
             QStringLiteral("https://user:secret@github.com/vendor/product"),
             QStringLiteral("https://github.com:8443/vendor/product"),
             QStringLiteral("https://github.com/vendor/product#fragment"),
             QStringLiteral("https://github.com/vendor")}) {
        QString error;
        QVERIFY2(!pacsmith::RemoteImportService::parseGitHubUrl(
                      QUrl(value), {}, false, &error).has_value(), qPrintable(value));
        QVERIFY2(!error.isEmpty(), qPrintable(value));
    }

    QString error;
    QVERIFY(!pacsmith::RemoteImportService::parseGitHubUrl(
                 QUrl(QStringLiteral("https://github.com/vendor/product")),
                 QStringLiteral("["), false, &error).has_value());
    QVERIFY(error.contains(QStringLiteral("invalid")));
}
