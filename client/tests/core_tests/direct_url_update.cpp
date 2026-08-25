#include "core_tests.hpp"

#include "core/direct_url_update_service.hpp"

#include <QTest>

void CoreTests::comparesDirectUrlValidators() {
    using pacsmith::DirectUrlValidatorComparison;
    using pacsmith::DirectUrlValidators;

    const DirectUrlValidators stored{QStringLiteral("\"etag-a\""),
                                     QStringLiteral("Mon, 24 Aug 2026 12:00:00 GMT"),
                                     100, QStringLiteral("x-amz-version-id"),
                                     QStringLiteral("version-a")};
    auto remote = stored;
    QCOMPARE(pacsmith::DirectUrlUpdateService::compareValidators(stored, remote),
             DirectUrlValidatorComparison::Unchanged);

    remote.etag = QStringLiteral("\"etag-b\"");
    QCOMPARE(pacsmith::DirectUrlUpdateService::compareValidators(stored, remote),
             DirectUrlValidatorComparison::Changed);

    auto noEtagStored = stored;
    auto noEtagRemote = stored;
    noEtagStored.etag.clear();
    noEtagRemote.etag.clear();
    noEtagRemote.vendorValue = QStringLiteral("version-b");
    QCOMPARE(pacsmith::DirectUrlUpdateService::compareValidators(
                 noEtagStored, noEtagRemote),
             DirectUrlValidatorComparison::Changed);

    noEtagStored.vendorName.clear();
    noEtagStored.vendorValue.clear();
    noEtagRemote.vendorName.clear();
    noEtagRemote.vendorValue.clear();
    noEtagRemote.lastModified = noEtagStored.lastModified;
    noEtagRemote.contentLength = 101;
    QCOMPARE(pacsmith::DirectUrlUpdateService::compareValidators(
                 noEtagStored, noEtagRemote),
             DirectUrlValidatorComparison::Changed);

    QCOMPARE(pacsmith::DirectUrlUpdateService::compareValidators(
                 {}, DirectUrlValidators{QStringLiteral("\"new\""), {}, -1, {}, {}}),
             DirectUrlValidatorComparison::NoCommonValidator);
}

void CoreTests::schedulesDirectUrlFullContentChecks() {
    pacsmith::UpdateConfiguration update;
    update.directUrlFullCheckIntervalHours = 24 * 7;
    const auto now = QDateTime::fromString(
        QStringLiteral("2026-08-25T12:00:00Z"), Qt::ISODate);
    QVERIFY(pacsmith::DirectUrlUpdateService::fullContentCheckDue(update, now));

    update.directUrlLastFullCheck = now.addDays(-6);
    QVERIFY(!pacsmith::DirectUrlUpdateService::fullContentCheckDue(update, now));
    update.directUrlLastFullCheck = now.addDays(-7);
    QVERIFY(pacsmith::DirectUrlUpdateService::fullContentCheckDue(update, now));

    update.directUrlFullCheckIntervalHours = 0;
    QVERIFY(!pacsmith::DirectUrlUpdateService::fullContentCheckDue(update, now));
}

void CoreTests::persistsDirectUrlCheckState() {
    pacsmith::UpdateConfiguration update;
    update.strategy = pacsmith::UpdateStrategy::DirectUrl;
    update.url = QStringLiteral("https://vendor.example/download/latest");
    update.directUrlEtag = QStringLiteral("\"opaque-etag\"");
    update.directUrlLastModified = QStringLiteral("Tue, 25 Aug 2026 12:00:00 GMT");
    update.directUrlContentLength = 1517706;
    update.directUrlVendorValidatorName = QStringLiteral("x-amz-version-id");
    update.directUrlVendorValidator = QStringLiteral("object-version");
    update.directUrlLastSha256 = QString(64, QLatin1Char('a'));
    update.directUrlLastFullCheck = QDateTime::fromString(
        QStringLiteral("2026-08-25T12:00:00Z"), Qt::ISODate);
    update.directUrlFullCheckIntervalHours = 24 * 30;

    const auto restored = pacsmith::UpdateConfiguration::fromJson(update.toJson());
    QCOMPARE(restored.strategy, pacsmith::UpdateStrategy::DirectUrl);
    QCOMPARE(restored.url, update.url);
    QCOMPARE(restored.directUrlEtag, update.directUrlEtag);
    QCOMPARE(restored.directUrlLastModified, update.directUrlLastModified);
    QCOMPARE(restored.directUrlContentLength, update.directUrlContentLength);
    QCOMPARE(restored.directUrlVendorValidatorName,
             update.directUrlVendorValidatorName);
    QCOMPARE(restored.directUrlVendorValidator, update.directUrlVendorValidator);
    QCOMPARE(restored.directUrlLastSha256, update.directUrlLastSha256);
    QCOMPARE(restored.directUrlLastFullCheck, update.directUrlLastFullCheck);
    QCOMPARE(restored.directUrlFullCheckIntervalHours,
             update.directUrlFullCheckIntervalHours);
}
