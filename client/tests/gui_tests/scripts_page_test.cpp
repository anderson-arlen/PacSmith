#include "gui/main_window/main_window.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

namespace pacsmith::gui {

class ScriptsPageTest final : public QObject {
    Q_OBJECT
private slots:
    void importedSourceFollowsResponsibilitySelection() {
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

        PackageRelease release;
        release.id = QStringLiteral("release");
        const auto installContents = QStringLiteral("post_install() {\n  modprobe sg || true\n}\n");
        const auto removeContents = QStringLiteral("post_remove() {\n  udevadm control --reload-rules\n}\n");
        release.maintainerScripts.append({QStringLiteral(".INSTALL"), installContents, {}});
        release.maintainerScripts.append({QStringLiteral("postrm"), removeContents, {}});
        for (const auto &script : release.maintainerScripts) {
            ScriptFinding finding;
            finding.scriptName = script.name;
            finding.disposition = ScriptDisposition::Unresolved;
            release.scriptFindings.append(finding);
        }
        Project project;
        project.id = QStringLiteral("project");
        project.releases.append(release);
        window.project_ = project;
        window.currentReleaseId_ = release.id;
        window.populateScripts();

        QCOMPARE(window.scriptFindingsTable_->currentRow(), 0);
        QCOMPARE(window.scriptFindingSourceView_->toPlainText(), installContents);
        QVERIFY(window.scriptFindingSourceView_->isReadOnly());
        QVERIFY(window.lifecycleStatus_->text().contains(QStringLiteral("unresolved")));
        QVERIFY(window.currentRelease()->lifecycleScript.contents.isEmpty());
        QVERIFY(!window.useOriginalLifecycleButton_->isEnabled());
        window.currentRelease()->sourceType = SourcePackageType::ArchPackage;
        window.populateScripts();
        QVERIFY(window.useOriginalLifecycleButton_->isEnabled());

        window.scriptFindingsTable_->setCurrentCell(1, 0);
        QCOMPARE(window.scriptFindingSourceView_->toPlainText(), removeContents);
        QVERIFY(!window.useOriginalLifecycleButton_->isEnabled());
        window.populateScripts();
        QCOMPARE(window.scriptFindingsTable_->currentRow(), 1);
        QCOMPARE(window.scriptFindingSourceView_->toPlainText(), removeContents);

        window.currentRelease()->maintainerScripts.removeLast();
        window.populateScripts();
        QVERIFY(window.scriptFindingSourceView_->toPlainText().isEmpty());
        QVERIFY(window.scriptFindingSourceStatus_->text().contains(QStringLiteral("unavailable")));

        window.currentRelease()->scriptFindings.clear();
        window.populateScripts();
        QVERIFY(window.scriptFindingSourceView_->toPlainText().isEmpty());
        QVERIFY(window.lifecycleStatus_->text().contains(QStringLiteral("No privileged")));
    }
};

}

QTEST_MAIN(pacsmith::gui::ScriptsPageTest)
#include "scripts_page_test.moc"
