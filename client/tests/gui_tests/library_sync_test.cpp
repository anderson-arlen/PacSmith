#include "gui/main_window/main_window.hpp"

#include <QFrame>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

namespace pacsmith::gui {

class LibrarySyncTest final : public QObject {
    Q_OBJECT
private slots:
    void staleDraftSurvivesRefresh_data() {
        QTest::addColumn<bool>("initialDeletion");
        QTest::addColumn<QString>("refresh");
        for (const bool deleted : {false, true}) {
            for (const auto &refresh : {"unrelated", "empty", "selected", "deleted", "full-deleted"}) {
                const auto name = QByteArray(deleted ? "deleted-" : "changed-") + refresh;
                QTest::newRow(name.constData()) << deleted << QString::fromLatin1(refresh);
            }
        }
    }

    void staleDraftSurvivesRefresh() {
        QFETCH(bool, initialDeletion);
        QFETCH(QString, refresh);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto configHome = qgetenv("XDG_CONFIG_HOME");
        const auto dataHome = qgetenv("XDG_DATA_HOME");
        const auto restoreEnvironment = qScopeGuard([&] {
            if (configHome.isNull()) qunsetenv("XDG_CONFIG_HOME");
            else qputenv("XDG_CONFIG_HOME", configHome);
            if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME");
            else qputenv("XDG_DATA_HOME", dataHome);
        });
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        ConnectionConfig connection;
        connection.socketPath = directory.filePath(QStringLiteral("not-running.sock"));
        QVERIFY(connection.save());
        AppSettingsStore settings(directory.filePath(QStringLiteral("settings")));
        MainWindow window(settings);

        Project selected;
        selected.id = QStringLiteral("selected");
        selected.displayName = QStringLiteral("Original package");
        Project other;
        other.id = QStringLiteral("other");
        other.displayName = QStringLiteral("Other package");
        window.project_ = selected;
        window.projectCache_.insert(selected.id, selected);
        window.projectCache_.insert(other.id, other);
        const auto draft = QStringLiteral("Unsaved local recipe");
        window.pkgbuildEditor_->setPlainText(draft);
        window.pkgbuildEditor_->document()->setModified(true);

        auto updated = selected;
        updated.displayName = QStringLiteral("First external change");
        if (initialDeletion) window.applyEventProjects({}, {}, {}, {selected.id}, false);
        else window.applyEventProjects({updated}, {}, {}, {}, false);
        QVERIFY(window.projectStale_);
        QCOMPARE(window.pendingExternalDeletion_, initialDeletion);

        bool expectedDeletion = initialDeletion;
        if (refresh == QStringLiteral("unrelated")) {
            other.displayName = QStringLiteral("Updated other package");
            window.applyEventProjects({other}, {}, {}, {}, false);
            QCOMPARE(window.projectCache_.value(other.id).displayName, other.displayName);
        } else if (refresh == QStringLiteral("empty")) {
            window.applyEventProjects({}, {}, {QStringLiteral("repository")}, {}, false);
        } else if (refresh == QStringLiteral("selected")) {
            updated.displayName = QStringLiteral("Second external change");
            window.applyEventProjects({updated}, {}, {}, {}, false);
            expectedDeletion = false;
        } else if (refresh == QStringLiteral("deleted")) {
            window.applyEventProjects({}, {}, {}, {selected.id}, false);
            expectedDeletion = true;
        } else {
            window.applyEventProjects({other}, {}, {}, {}, true);
            expectedDeletion = true;
        }

        QVERIFY(window.projectStale_);
        QVERIFY(window.project_.has_value());
        QCOMPARE(window.project_->displayName, selected.displayName);
        QCOMPARE(window.pkgbuildEditor_->toPlainText(), draft);
        QVERIFY(window.pkgbuildEditor_->document()->isModified());
        QCOMPARE(window.pendingExternalDeletion_, expectedDeletion);
        QCOMPARE(window.pendingExternalProject_.has_value(), !expectedDeletion);
        if (!expectedDeletion) QCOMPARE(window.pendingExternalProject_->displayName, updated.displayName);
        QVERIFY(!window.externalChangeBanner_->isHidden());
        QCOMPARE(window.externalReloadButton_->text(), expectedDeletion
            ? QStringLiteral("Leave package") : QStringLiteral("Reload"));
    }
};

}

QTEST_MAIN(pacsmith::gui::LibrarySyncTest)
#include "library_sync_test.moc"
