#include "gui/main_window/support.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QPushButton>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

namespace pacsmith::gui {

class PPAPickerTest final : public QObject {
    Q_OBJECT
private slots:
    void selectsPublishedReleaseAndPreservesPackageAndSigningIdentity() {
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto close = qScopeGuard([&] { dialog->reject(); });
            auto *suite = dialog->findChild<QComboBox *>(QStringLiteral("ppaSuite"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(suite && buttons);
            auto *ok = buttons->button(QDialogButtonBox::Ok);
            QCOMPARE(suite->count(), 2);
            QVERIFY(ok->isEnabled());
            suite->setCurrentText(QStringLiteral("jammy"));
            QCOMPARE(suite->currentText(), QStringLiteral("jammy"));
            suite->setCurrentText(QStringLiteral("noble"));
            QCOMPARE(suite->currentText(), QStringLiteral("noble"));
            inspected = true;
            dialog->accept();
            close.dismiss();
        });
        const auto fingerprint = QStringLiteral("2EC86B48E6A9F326623CD22FFF0E7BBEC491C6A1");
        const auto choice = choosePPAPackage(nullptr, {
            {QStringLiteral("ppa"), QStringLiteral("ppa:ubuntugis/ubuntugis-unstable")},
            {QStringLiteral("display_name"), QStringLiteral("UbuntuGIS")},
            {QStringLiteral("architecture"), QStringLiteral("amd64")},
            {QStringLiteral("package"), QStringLiteral("grass-core")},
            {QStringLiteral("repository_url"), QStringLiteral("https://ppa.launchpadcontent.net/ubuntugis/ubuntugis-unstable/ubuntu")},
            {QStringLiteral("signing_key_url"), QStringLiteral("https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x") + fingerprint},
            {QStringLiteral("signing_fingerprint"), fingerprint},
            {QStringLiteral("suites"), QJsonArray{QStringLiteral("jammy"), QStringLiteral("noble")}}});
        QVERIFY(inspected);
        QVERIFY(choice);
        QCOMPARE(choice->update.aptSuite, QStringLiteral("noble"));
        QCOMPARE(choice->update.aptPackageName, QStringLiteral("grass-core"));
        QCOMPARE(choice->update.aptArchitecture, QStringLiteral("amd64"));
        QCOMPARE(choice->update.aptComponent, QStringLiteral("main"));
        QCOMPARE(choice->update.trustedSigningFingerprint, fingerprint);
        QCOMPARE(choice->signingKeyUrl.host(), QStringLiteral("keyserver.ubuntu.com"));
    }
};

} // namespace pacsmith::gui

QTEST_MAIN(pacsmith::gui::PPAPickerTest)
#include "ppa_picker_test.moc"
