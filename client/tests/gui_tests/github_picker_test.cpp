#include "gui/main_window/support.hpp"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

namespace pacsmith::gui {

class GitHubPickerTest final : public QObject {
    Q_OBJECT
private slots:
    void suggestedRulesTrackVersionChanges_data() {
        QTest::addColumn<QString>("original");
        QTest::addColumn<QStringList>("matching");
        QTest::addColumn<QStringList>("excluded");
        QTest::newRow("compact-alpha-arch")
            << QStringLiteral("toughfix-0.1.0alpha1-1-x86_64.pkg.tar.zst")
            << QStringList{"toughfix-0.1.0alpha1-1-x86_64.pkg.tar.zst",
                           "toughfix-0.1.0alpha2-1-x86_64.pkg.tar.zst",
                           "toughfix-0.1.0alpha3-1-x86_64.pkg.tar.zst",
                           "toughfix-0.1.0-1-x86_64.pkg.tar.zst"}
            << QStringList{"toughfix-0.1.0alpha3-1-aarch64.pkg.tar.zst",
                           "toughfix-0.1.0alpha3-1-x86_64.pkg.tar.zst.sig",
                           "other-0.1.0alpha3-1-x86_64.pkg.tar.zst"};
        QTest::newRow("compact-beta-deb")
            << QStringLiteral("tool_1.2.3beta1_amd64.deb")
            << QStringList{"tool_1.2.3beta2_amd64.deb", "tool_1.2.3_amd64.deb"}
            << QStringList{"tool_1.2.3beta2_arm64.deb", "tool_1.2.3beta2_amd64.rpm"};
        QTest::newRow("compact-rc-rpm")
            << QStringLiteral("tool-1.2.3rc1.x86_64.rpm")
            << QStringList{"tool-1.2.3rc2.x86_64.rpm", "tool-1.2.3.x86_64.rpm"}
            << QStringList{"tool-1.2.3rc2.aarch64.rpm", "tool-1.2.3rc2.x86_64.deb"};
        QTest::newRow("hyphen-alpha-archive")
            << QStringLiteral("tool-1.2.3-alpha.1.tar.gz")
            << QStringList{"tool-1.2.3-alpha.2.tar.gz", "tool-1.2.3.tar.gz"}
            << QStringList{"tool-1.2.3-alpha.2.zip", "tool-1.2.3-alpha.2.tar.gz.sig"};
        QTest::newRow("underscore-beta-archive")
            << QStringLiteral("tool-1.2.3_beta1.tar.xz")
            << QStringList{"tool-1.2.3_beta2.tar.xz", "tool-1.2.3.tar.xz"}
            << QStringList{"tool-1.2.3_beta2.zip", "tool-1.2.3_beta2.tar.xz.asc"};
        QTest::newRow("preview-archive")
            << QStringLiteral("tool-1.2.3-preview.1.tar.gz")
            << QStringList{"tool-1.2.3-preview.2.tar.gz", "tool-1.2.3.tar.gz"}
            << QStringList{"tool-1.2.3-preview.2.zip"};
        QTest::newRow("stable-deb")
            << QStringLiteral("tool_1.2.3_amd64.deb")
            << QStringList{"tool_1.2.4_amd64.deb"}
            << QStringList{"tool_1.2.4_arm64.deb", "tool_1.2.4_amd64.deb.sig"};
        QTest::newRow("unversioned")
            << QStringLiteral("MapleMono-NF-unhinted.zip")
            << QStringList{"MapleMono-NF-unhinted.zip"}
            << QStringList{"MapleMono-NF-hinted.zip", "MapleMono-NF-unhinted.tar.gz"};
    }

    void suggestedRulesTrackVersionChanges() {
        QFETCH(QString, original);
        QFETCH(QStringList, matching);
        QFETCH(QStringList, excluded);
        const QRegularExpression pattern(
            QRegularExpression::anchoredPattern(suggestedAssetRegex(original)));
        QVERIFY2(pattern.isValid(), qPrintable(pattern.errorString()));
        QVERIFY2(pattern.match(original).hasMatch(), qPrintable(original));
        for (const auto &asset : matching) {
            QVERIFY2(pattern.match(asset).hasMatch(), qPrintable(asset));
        }
        for (const auto &asset : excluded) {
            QVERIFY2(!pattern.match(asset).hasMatch(), qPrintable(asset));
        }
    }

    void artifactColorsFollowTheme_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void artifactColorsFollowTheme() {
        QFETCH(bool, dark);
        const auto originalPalette = QApplication::palette();
        const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(originalPalette); });
        auto palette = originalPalette;
        palette.setColor(QPalette::Base, dark ? QColor("#202020") : QColor("#ffffff"));
        palette.setColor(QPalette::Text, dark ? QColor("#ffffff") : QColor("#202020"));
        palette.setColor(QPalette::Highlight, dark ? QColor("#86c5ff") : QColor("#215b94"));
        palette.setColor(QPalette::HighlightedText, dark ? QColor("#101010") : QColor("#ffffff"));
        QApplication::setPalette(palette);
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            const auto closeDialog = qScopeGuard([&] { dialog->reject(); });
            auto *list = dialog->findChild<QListWidget *>();
            auto *expression = dialog->findChild<QLineEdit *>();
            QVERIFY(list != nullptr);
            QVERIFY(expression != nullptr);
            QCOMPARE(list->count(), 3);
            expression->setText(QStringLiteral("first\\.deb"));
            QCOMPARE(list->item(0)->background(), list->palette().brush(QPalette::Highlight));
            QCOMPARE(list->item(0)->foreground(), list->palette().brush(QPalette::HighlightedText));
            for (int row = 1; row < list->count(); ++row) {
                QVERIFY(!list->item(row)->data(Qt::BackgroundRole).isValid());
                QVERIFY(!list->item(row)->data(Qt::ForegroundRole).isValid());
            }
            QVERIFY(!list->item(2)->flags().testFlag(Qt::ItemIsEnabled));
            expression->setText(QStringLiteral("second\\.rpm"));
            QVERIFY(!list->item(0)->data(Qt::BackgroundRole).isValid());
            QVERIFY(!list->item(0)->data(Qt::ForegroundRole).isValid());
            QCOMPARE(list->item(1)->background(), list->palette().brush(QPalette::Highlight));
            QCOMPARE(list->item(1)->foreground(), list->palette().brush(QPalette::HighlightedText));
            inspected = true;
        });
        const auto choice = chooseGitHubAssetRule(nullptr,
            {QStringLiteral("first.deb"), QStringLiteral("second.rpm"), QStringLiteral("SHA256SUMS")}, false);
        QVERIFY(!choice.has_value());
        QVERIFY(inspected);
    }
};

}

QTEST_MAIN(pacsmith::gui::GitHubPickerTest)
#include "github_picker_test.moc"
