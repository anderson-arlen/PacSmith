#include "../server_ai_host.hpp"
#include "core/acp_conversations.hpp"
#include <QComboBox>
#include <QScopeGuard>
#include "gui/main_window/main_window.hpp"
#include "gui/acp_chat_widget.hpp"
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTabBar>
#include <QFrame>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include "gui/chat_transcript.hpp"
#include <QVBoxLayout>

using namespace pacsmith;
using namespace pacsmith::gui;

class ScreenChatTest final : public QObject {
    Q_OBJECT
private slots:
    void singleHarnessSettingsAutosave() {
        QTemporaryDir directory;
        const auto configHome = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        ConnectionConfig connection;
        connection.socketPath = directory.filePath(QStringLiteral("not-running.sock"));
        QVERIFY(connection.save());
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        HarnessProfile harness;
        harness.name = QStringLiteral("Codex"); harness.executable = QStringLiteral("codex-acp");
        tests::ServerAiHost host(harness);
        bool inspected = false;
        {
            MainWindow window(store); window.show();
            QTimer inspect;
            inspect.setInterval(50);
            connect(&inspect, &QTimer::timeout, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                inspect.stop();
                const auto close = qScopeGuard([dialog] { dialog->reject(); });
                auto *page = dialog->findChild<QWidget *>(QStringLiteral("aiHarnessPage"));
                QVERIFY(page != nullptr);
                for (auto *tabs : dialog->findChildren<QTabWidget *>()) if (tabs->indexOf(page) >= 0) tabs->setCurrentWidget(page);
                auto *registry = page->findChild<QPushButton *>(QStringLiteral("browseAcpRegistry"));
                auto *settings = page->findChild<QPushButton *>(QStringLiteral("harnessAgentSettings"));
                QVERIFY(registry && settings);
                QVERIFY(registry->parentWidget() != settings->parentWidget());
                QVERIFY(settings->text().isEmpty());
                QVERIFY(!settings->icon().isNull());
                QCOMPARE(settings->accessibleName(), QStringLiteral("Agent settings"));
                QVERIFY(page->findChildren<QComboBox *>().isEmpty());
                auto *name = page->findChild<QLineEdit *>(QStringLiteral("harnessName"));
                QTRY_VERIFY(name->isEnabled());
                QCOMPARE(name->text(), QStringLiteral("Codex"));
                name->setFocus();
                name->selectAll(); QTest::keyClicks(name, QStringLiteral("My agent"));
                QTest::keyClick(name, Qt::Key_Tab);
                QTRY_COMPARE(ServerAi(host.connection).harness()->name, QStringLiteral("My agent"));
                QCOMPARE(ServerAi(host.connection).harness()->executable, QStringLiteral("codex-acp"));
                if (qEnvironmentVariableIsSet("PACSMITH_TEST_HARNESS_SCREENSHOT")) dialog->grab().save(qEnvironmentVariable("PACSMITH_TEST_HARNESS_SCREENSHOT"));
                auto *executable = page->findChild<QLineEdit *>(QStringLiteral("harnessExecutable"));
                name->setFocus(); name->selectAll(); QTest::keyClick(name, Qt::Key_Backspace);
                executable->setFocus(); executable->selectAll(); QTest::keyClick(executable, Qt::Key_Backspace);
                QTest::keyClick(executable, Qt::Key_Tab);
                QTRY_VERIFY(!ServerAi(host.connection).harness());
                inspected = true;
            });
            inspect.start();
            QPushButton *settings = nullptr;
            for (auto *button : window.findChildren<QPushButton *>()) if (button->text() == QStringLiteral("Settings")) settings = button;
            QVERIFY(settings != nullptr);
            QTimer::singleShot(10000, &window, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
            settings->click();
            QVERIFY(inspected);
        }
        if (configHome.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", configHome);
    }
    void conversationsAreSharedAndSurviveClientClosure() {
        QTemporaryDir directory;
        const auto configHome = qgetenv("XDG_CONFIG_HOME");
        const auto dataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        ConnectionConfig connection;
        connection.socketPath = directory.filePath(QStringLiteral("not-running.sock"));
        QVERIFY(connection.save());
        const auto log = directory.filePath(QStringLiteral("requests.jsonl"));
        QFile script(directory.filePath(QStringLiteral("agent.py")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(import json,sys,uuid
session = None
pending = None
def reply(id, result):
 print(json.dumps(dict(jsonrpc='2.0',id=id,result=result)),flush=True)
for line in sys.stdin:
 msg=json.loads(line)
 method=msg.get('method')
 params=msg.get('params',{})
 if method=='initialize': result=dict(protocolVersion=1,agentCapabilities=dict(loadSession=True),authMethods=[])
 elif method=='session/new':
  session=str(uuid.uuid4())
  result=dict(sessionId=session)
 elif method=='session/load':
  session=params['sessionId']
  result={}
 elif method=='session/prompt':
  pending=msg['id']
  result=dict(stopReason='end_turn')
 elif method=='session/cancel':
  reply(pending,dict(stopReason='cancelled'))
  continue
 else: continue
 with open(sys.argv[1],'a') as record:
  record.write(json.dumps(dict(method=method,session=session))+'\n')
 if method=='session/prompt' and params['prompt'][0]['text'].endswith('hold'): continue
 reply(msg['id'],result)
)PY");
        script.close();
        HarnessProfile profile;
        profile.name = QStringLiteral("Test agent");
        profile.executable = QStringLiteral("/usr/bin/python3");
        profile.arguments = {script.fileName(), log};
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        QVERIFY(store.setHarness(profile));
        tests::ServerAiHost host(profile);
        connection=host.connection;
        AutomaticReviewRequest request{connection,profile,QStringLiteral("package-a"),{},QStringLiteral("Package A"),{}};
        auto first=std::make_unique<MainWindow>(store);
        auto *chat=first->openAiConversation(request);
        QVERIFY(chat != nullptr);
        QTRY_VERIFY(!chat->isBusy());
        QVERIFY(chat->submit(QStringLiteral("manual hold")));
        QTRY_VERIFY(chat->isBusy());
        MainWindow second(store);
        auto *shared=second.openAiConversation(request);
        QVERIFY(shared != nullptr);
        QCOMPARE(shared->objectName(),chat->objectName());
        auto *transcript=shared->findChild<ChatTranscript *>(QStringLiteral("aiTranscript"));
        QTRY_VERIFY(transcript->toPlainText().contains(QStringLiteral("manual hold")));
        QVERIFY(shared->isBusy());
        first.reset();
        const auto running=ServerAi(connection).request(QStringLiteral("GET"),QStringLiteral("/conversations/%1").arg(shared->objectName()));
        QVERIFY(running);QCOMPARE(running->value(QStringLiteral("status")).toString(),QStringLiteral("running"));
        shared->findChild<QPushButton *>(QStringLiteral("chatAction"))->click();
        QTRY_VERIFY(!shared->isBusy());
        const auto oldKey=shared->objectName();
        shared->findChild<QPushButton *>(QStringLiteral("newChat"))->click();
        QTRY_VERIFY(!shared->isBusy());
        QVERIFY(shared->objectName()!=oldKey);
        QVERIFY(shared->submit(QStringLiteral("new conversation")));
        QTRY_VERIFY(!shared->isBusy());
        MainWindow third(store);
        auto *latest=third.openAiConversation(request);
        QVERIFY(latest != nullptr);QCOMPARE(latest->objectName(),shared->objectName());
        QTRY_VERIFY(latest->findChild<ChatTranscript *>()->toPlainText().contains(QStringLiteral("new conversation")));
        QVERIFY(!latest->findChild<ChatTranscript *>()->toPlainText().contains(QStringLiteral("manual hold")));
        if(configHome.isNull())qunsetenv("XDG_CONFIG_HOME");else qputenv("XDG_CONFIG_HOME",configHome);
        if(dataHome.isNull())qunsetenv("XDG_DATA_HOME");else qputenv("XDG_DATA_HOME",dataHome);
    }

    void availableOnMainWindowWithoutDialogInjection() {
        QTemporaryDir directory;
        const auto configHome = qgetenv("XDG_CONFIG_HOME");
        const auto dataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        ConnectionConfig connection;
        connection.socketPath = directory.filePath(QStringLiteral("not-running.sock"));
        QVERIFY(connection.save());
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        HarnessProfile profile;
        profile.name = QStringLiteral("Test agent");
        const auto requests = directory.filePath(QStringLiteral("prompt.txt"));
        QFile script(directory.filePath(QStringLiteral("agent.py")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(import json,sys
for line in sys.stdin:
 msg=json.loads(line)
 method=msg.get('method')
 if method=='initialize': result=dict(protocolVersion=1,agentCapabilities={},authMethods=[])
 elif method=='session/new': result=dict(sessionId='test-screen-chat')
 elif method=='session/prompt':
  with open(sys.argv[1],'w') as record: record.write(msg['params']['prompt'][0]['text'])
  result=dict(stopReason='end_turn')
 else: continue
 print(json.dumps(dict(jsonrpc='2.0',id=msg['id'],result=result)),flush=True)
)PY");
        script.close();
        profile.executable = QStringLiteral("/usr/bin/python3");
        profile.arguments = {script.fileName(), requests};
        tests::ServerAiHost host(profile);
        {
            MainWindow window(store);
            window.resize(1450, 850); window.show();
            const auto panes = window.findChildren<QSplitter *>();
            QVERIFY(panes.size() >= 6);
            for (const auto *pane : panes) QCOMPARE(pane->handleWidth(), 12);
            auto *ask = window.findChild<QPushButton *>(QStringLiteral("globalAskAi"));
            QVERIFY(ask != nullptr); QVERIFY(ask->isEnabled()); QVERIFY(ask->isVisible());
            QVERIFY(ask->isCheckable()); QVERIFY(!ask->isChecked());
            QCOMPARE(ask->accessibleName(), QStringLiteral("Ask AI"));
            QVERIFY(!ask->icon().isNull());
            QCOMPARE(ask->text(), QStringLiteral("Ask AI"));
            QVERIFY(window.statusBar()->isAncestorOf(ask));
            auto *connectionButton = window.findChild<QPushButton *>(QStringLiteral("libraryConnection"));
            QVERIFY(connectionButton != nullptr);
            const auto isBesideConnection = [&] {
                const auto connectionEdge = connectionButton->mapTo(&window, connectionButton->rect().topRight());
                const auto askEdge = ask->mapTo(&window, ask->rect().topLeft());
                return askEdge.x() > connectionEdge.x() && askEdge.x() - connectionEdge.x() <= 8 &&
                       qAbs(askEdge.y() - connectionEdge.y()) <= 2;
            };
            QTRY_VERIFY(isBesideConnection());
            ask->click();
            auto *dock = window.findChild<QDockWidget *>(QStringLiteral("aiDock"));
            QVERIFY(dock != nullptr); QVERIFY(dock->isVisible());
            QVERIFY(ask->isChecked());
            QVERIFY(dock->findChildren<QTabBar *>().isEmpty());
            QVERIFY(dock->titleBarWidget() != nullptr);
            QCOMPARE(dock->titleBarWidget()->height(), 0);
            QVERIFY(dock->findChild<QFrame *>(QStringLiteral("aiDivider")) == nullptr);
            QTRY_COMPARE(window.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, &window), 12);
            QTRY_VERIFY(dock->geometry().left() > window.centralWidget()->geometry().right());
            const auto beforeResize = dock->width();
            const auto centralBeforeResize = window.centralWidget()->width();
            const QPoint grip((window.centralWidget()->geometry().right() + dock->geometry().left()) / 2,
                              dock->geometry().center().y());
            QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, grip);
            const auto destination = grip - QPoint(70, 0);
            QTest::mouseMove(&window, destination);
            // Qt applies dock resizing on a timer while the mouse button is held.
            QTRY_VERIFY2(dock->width() > beforeResize + 40,
                         qPrintable(QStringLiteral("Dock %1 -> %2, center %3, minimum %4, grip %5")
                             .arg(beforeResize).arg(dock->width()).arg(centralBeforeResize)
                             .arg(window.centralWidget()->minimumSizeHint().width()).arg(grip.x())));
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, destination);
            QVERIFY(window.centralWidget()->width() < centralBeforeResize - 40);
            QTRY_VERIFY(isBesideConnection());
            auto *splitter = qobject_cast<QSplitter *>(window.centralWidget());
            QVERIFY(splitter != nullptr);
            splitter->setSizes({0, 900});
            QVERIFY(ask->isVisible());
            ask->click();
            QVERIFY(!dock->isVisible());
            QVERIFY(!ask->isChecked());
            ask->click();
            QVERIFY(dock->isVisible());
            QVERIFY(ask->isChecked());
            ask->click();
            QVERIFY(!dock->isVisible()); QVERIFY(!ask->isChecked());
            ask->click();
            QVERIFY(dock->isVisible()); QVERIFY(ask->isChecked());
            splitter->setSizes({260, 920});
            auto *chat = window.findChild<AcpChatWidget *>();
            QVERIFY(chat != nullptr); QTRY_VERIFY(!chat->isBusy());
            auto *transcript = chat->findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QVERIFY(transcript->toPlainText().isEmpty());
            QVERIFY(!QFile::exists(requests));
            QVERIFY(chat->findChild<QLabel *>(QStringLiteral("aiScreenContext")) == nullptr);
            auto *composer = chat->findChild<QPlainTextEdit *>(QStringLiteral("aiComposer"));
            QTRY_VERIFY(composer->hasFocus());
            QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("What can I do here?"));
            QCOMPARE(composer->toPlainText(), QStringLiteral("What can I do here?"));
            QSignalSpy completed(chat, &AcpChatWidget::completed);
            const auto sentPrompt = [&] {
                QFile record(requests);
                return record.open(QIODevice::ReadOnly) ? QString::fromUtf8(record.readAll()) : QString{};
            };
            QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
            QTest::keyClicks(composer, QStringLiteral("Explain the options."));
            QCOMPARE(composer->toPlainText(), QStringLiteral("What can I do here?\nExplain the options."));
            QVERIFY(!QFile::exists(requests));
            QTest::keyClick(composer, Qt::Key_Return);
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(sentPrompt().contains(QStringLiteral("Library overview")));
            QVERIFY(sentPrompt().contains(QStringLiteral("What can I do here?")));
            QVERIFY(sentPrompt().contains(QStringLiteral("What can I do here?\nExplain the options.")));
            QVERIFY(!transcript->toPlainText().contains(QStringLiteral("Current screen context")));
            auto *dialog = new QDialog(&window);
            dialog->setWindowTitle(QStringLiteral("Settings"));
            auto *layout = new QVBoxLayout(dialog);
            auto *tabs = new QTabWidget(dialog);
            tabs->addTab(new QWidget(tabs), QStringLiteral("General"));
            auto *credentials = new QWidget(tabs);
            auto *fields = new QVBoxLayout(credentials);
            auto *secret = new QLineEdit(QStringLiteral("not-for-the-agent"), credentials);
            secret->setEchoMode(QLineEdit::Password); fields->addWidget(secret);
            tabs->addTab(credentials, QStringLiteral("Credentials"));
            layout->addWidget(tabs);
            dialog->setModal(true); dialog->show();
            QPushButton *dialogAsk = nullptr;
            for (auto *button : dialog->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Ask AI about this screen")) dialogAsk = button;
            }
            QVERIFY(dialogAsk == nullptr);
            QVERIFY(dialog->findChild<QDialog *>(QStringLiteral("aiChatWindow")) == nullptr);
            delete dialog;
            QVERIFY(window.isAncestorOf(chat));
            ask->click(); QVERIFY(!dock->isVisible());
            ask->click(); QVERIFY(dock->isVisible());
            QTRY_VERIFY(composer->hasFocus());
            QVERIFY(!chat->isBusy());
            QVERIFY(chat->submit(QStringLiteral("Back to the library")));
            QTRY_COMPARE(completed.count(), 2);
            QVERIFY(sentPrompt().contains(QStringLiteral("Library overview")));
            QVERIFY(!sentPrompt().contains(QStringLiteral("Settings / Credentials")));
            QVERIFY(!chat->isBusy());
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_SCREENSHOT")) window.grab().save(qEnvironmentVariable("PACSMITH_TEST_SCREENSHOT"));
        }
        if (configHome.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", configHome);
        if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", dataHome);
    }
};
QTEST_MAIN(ScreenChatTest)
#include "screen_chat_test.moc"
