#include "core_tests.hpp"
#include "core/model.hpp"

#include <QTest>

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
