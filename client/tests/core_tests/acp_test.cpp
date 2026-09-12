#include "core/acp_client.hpp"
#include "core/acp_conversations.hpp"
#include "core/acp_registry.hpp"
#include <QBuffer>
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QDialog>
#include <QLabel>
#include <QPainter>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include "gui/acp_chat_widget.hpp"
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include "gui/chat_transcript.hpp"
#include "gui/agent_settings_dialog.hpp"
#include <QComboBox>
#include <QCheckBox>

using namespace pacsmith;

namespace {
QString fakeAgent(const QString &directory) {
    const auto path = QDir(directory).filePath(QStringLiteral("agent.py"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) qFatal("Cannot write fake agent");
    file.write(R"PY(import json,sys,os,subprocess
session = 'private-session'
prompt = None
options = [{'id':'model','name':'Model','type':'select','currentValue':'test-model','options':[{'name':'Test provider','options':[{'value':'test-model','name':'Test model'},{'value':'deliberate','name':'Deliberate model'}]}]}, {'id':'reasoning','name':'Reasoning effort','type':'select','currentValue':'low','description':'How much reasoning to use.','options':[{'value':'low','name':'Low'}]}, {'id':'provider.fast','name':'Fast mode','type':'boolean','currentValue':False,'description':'Faster responses.'}]
options.sort(key=lambda option: option['id']!='reasoning')
if 'codex-acp' in sys.argv: options.append(dict(id='mode',name='Mode',type='select',currentValue='read-only',options=[dict(value='read-only',name='Ask for approval')]))
def send(value):
 print(json.dumps(dict(jsonrpc='2.0', **value)), flush=True)
def reply(id, result):
 send(dict(id=id, result=result))
def update(kind, **values):
 send(dict(method='session/update',params=dict(sessionId=session, update=dict(sessionUpdate=kind, **values))))
for line in sys.stdin:
 msg=json.loads(line)
 if os.environ.get('PACSMITH_TEST_REQUESTS'):
  with open(os.environ['PACSMITH_TEST_REQUESTS'],'a') as record: record.write(line)
 method=msg.get('method')
 id=msg.get('id')
 params=msg.get('params',{})
 if method=='initialize':
  assert params['protocolVersion']==1
  assert 'fs' not in params['clientCapabilities']
  reply(id,dict(protocolVersion=1,agentCapabilities=dict(loadSession=True,promptCapabilities=dict(image='--images' in sys.argv)),authMethods=[]))
 elif method=='session/new':
  server=params['mcpServers'][0]
  assert params['mcpServers'][0]['name']=='pacsmith_session'
  reply(id,dict(sessionId=session,configOptions=options))
 elif method=='session/load':
  server=params['mcpServers'][0]
  assert params['sessionId']==session
  update('agent_message_chunk',content=dict(type='text',text='replayed'))
  reply(id,dict(configOptions=options))
 elif method=='session/set_config_option':
  if '--reject-fast' in sys.argv and params['configId']=='provider.fast':
   send(dict(id=id,error=dict(code=-32602,message='Fast mode is unavailable for this account.')))
   continue
  for option in options:
   if option['id']==params['configId']: option['currentValue']=params['value']
  if params['configId']=='model' and params['value']=='deliberate':
   next(option for option in options if option['id']=='reasoning')['options']=[dict(value='low',name='Low'),dict(value='high',name='High')]
  if isinstance(params['value'],bool): assert params['type']=='boolean'
  reply(id,dict(configOptions=options))
 elif method=='session/prompt':
  prompt=id
  send(dict(id='foreign',method='session/request_permission',params=dict(sessionId='personal-session',toolCall=dict(toolCallId='foreign-tool'),options=[dict(optionId='yes',kind='allow_once',name='Allow')])))
  text=params['prompt'][0]['text'].split('\n\n')[-1]
  if text=='name-session':
   assert 'set_session_description' in params['prompt'][0]['text']
   env=dict(os.environ, **{entry['name']:entry['value'] for entry in server['env']})
   messages=[dict(jsonrpc='2.0',id=1,method='initialize',params=dict(protocolVersion='2025-11-25')),dict(jsonrpc='2.0',method='notifications/initialized'),dict(jsonrpc='2.0',id=2,method='tools/call',params=dict(name='set_session_description',arguments=dict(description='Fix Slack automatic updates')))]
   result=subprocess.run([server['command']]+server['args'],env=env,input=''.join(json.dumps(message)+'\n' for message in messages),text=True,capture_output=True,check=True)
   response=json.loads(result.stdout.splitlines()[-1])['result']
   assert not response['isError'], result.stdout
  if text=='crash': sys.exit(7)
  if text=='invalid':
   print('not json',flush=True)
   continue
  send(dict(method='session/update',params=dict(sessionId='personal-session',update=dict(sessionUpdate='agent_message_chunk',content=dict(type='text',text='wrong session')))))
  update('agent_message_chunk',content=dict(type='text',text='Hello '))
  update('agent_message_chunk',content=dict(type='text',text='world'))
  if text=='hello-tool':
   update('tool_call',toolCallId='inspect-test',title='Inspect package',status='completed',content=[dict(type='content',content=dict(type='text',text='<b>Package details</b>'))])
  if text in ['permission','cancel','invalid-option']:
   update('tool_call',toolCallId='tool1',title='Inspect release',rawInput=dict(server='pacsmith_session',tool='get_project'),_meta=dict(is_mcp_tool_call=True))
   send(dict(id='permission1',method='session/request_permission',params=dict(sessionId=session,toolCall=dict(toolCallId='tool1'),options=[dict(optionId='yes',kind='allow_once',name='Allow once'),dict(optionId='allow_session',kind='allow_always',name='Allow for this session'),dict(optionId='allow_always',kind='allow_always',name='Always allow'),dict(optionId='no',kind='reject_once',name='Reject')])) )
  else: reply(id,dict(stopReason='end_turn'))
 elif method=='session/cancel': reply(prompt,dict(stopReason='cancelled'))
 elif id=='foreign':
  assert msg['result']['outcome']['outcome']=='cancelled'
 elif id=='permission1':
  outcome=msg['result']['outcome']
  update('agent_message_chunk',content=dict(type='text',text=json.dumps(outcome)))
  if outcome['outcome']=='selected': reply(prompt,dict(stopReason='end_turn'))
)PY");
    return path;
}
HarnessProfile profile(const QString &script) {
    HarnessProfile result;
    result.name = QStringLiteral("Test ACP");
    result.executable = QStringLiteral("/usr/bin/python3");
    result.arguments = {script};
    return result;
}
AcpEnvironment environment(const QString &directory) {
    return {directory, directory, QProcessEnvironment::systemEnvironment()};
}
}

class AcpTest final : public QObject {
    Q_OBJECT
private slots:
    void latestConversationSelectionPersistsAndMigrates() {
        QTemporaryDir directory;
        AcpConversations conversations(directory.path());
        QCOMPARE(conversations.latest(QStringLiteral("package-a")), QStringLiteral("package-a"));
        const auto first = AcpConversations::freshKey();
        const auto second = AcpConversations::freshKey();
        QVERIFY(first != second);
        QVERIFY(conversations.select(QStringLiteral("package-a"), first));
        QVERIFY(conversations.select(QStringLiteral("package-b"), second));
        QCOMPARE(AcpConversations(directory.path()).latest(QStringLiteral("package-a")), first);
        QCOMPARE(conversations.latest(QStringLiteral("package-b")), second);
        for (const auto &name : {QStringLiteral("old-release"), QStringLiteral("recent-release")}) {
            QFile file(directory.filePath(name + QStringLiteral(".json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
            QVERIFY(file.flush());
            QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(name == QStringLiteral("old-release") ? 1000 : 2000), QFileDevice::FileModificationTime));
        }
        QCOMPARE(conversations.latest(QStringLiteral("package-c"), {QStringLiteral("recent-release"), QStringLiteral("old-release")}), QStringLiteral("recent-release"));
        QVERIFY(!conversations.select(QStringLiteral("package-c"), first));
        const auto third = AcpConversations::freshKey();
        QVERIFY(conversations.select(QStringLiteral("package-c"), third));
        QCOMPARE(conversations.latest(QStringLiteral("package-c"), {QStringLiteral("recent-release")}), third);
        QVERIFY(!conversations.select(QStringLiteral("../outside"), first));
        QVERIFY(!conversations.select(QStringLiteral("package-a"), QStringLiteral("../outside")));
    }
    void sessionCatalogRetentionAndAttachments() {
        QTemporaryDir directory;
        AcpConversations conversations(directory.path());
        const auto now = QDateTime::currentDateTimeUtc();
        const auto write = [&](const QString &path, const QByteArray &bytes, const QDateTime &time) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) return false;
            return file.setFileTime(time, QFileDevice::FileModificationTime);
        };
        for (int i = 0; i < 12; ++i) {
            const auto key = QStringLiteral("chat-%1").arg(i);
            QVERIFY(conversations.select(QStringLiteral("slack"), key));
            QVERIFY(conversations.describe(key, QStringLiteral("Slack task %1").arg(i)));
            QVERIFY(write(directory.filePath(key + QStringLiteral(".json")), "{}", now.addSecs(i - 12)));
        }
        QVERIFY(conversations.select(QStringLiteral("signal"), QStringLiteral("signal-chat")));
        auto recent = conversations.recent(QStringLiteral("slack"));
        QCOMPARE(recent.size(), 10);
        QCOMPARE(recent.first().key, QStringLiteral("chat-11"));
        QCOMPARE(recent.last().description, QStringLiteral("Slack task 2"));
        QVERIFY(!conversations.describe(QStringLiteral("../outside"), QStringLiteral("bad")));
        QVERIFY(!conversations.describe(QStringLiteral("chat-11"), QString(121, QLatin1Char('x'))));
        QVERIFY(!conversations.describe(QStringLiteral("chat-11"), QStringLiteral("  ")));
        const auto old = now.addDays(-11);
        QVERIFY(write(directory.filePath(QStringLiteral("chat-11.json")), "{}", old));
        QVERIFY(write(directory.filePath(QStringLiteral("chat-10.json")), R"({"draftImages":[{"file":"shared"}]})", old));
        QVERIFY(write(directory.filePath(QStringLiteral("chat-9.json")), R"({"entries":[{"images":[{"file":"shared"}]}]})", now));
        QVERIFY(write(directory.filePath(QStringLiteral("chat-8.json")), R"({"draftImages":[{"file":"live"}]})", old));
        QLockFile live(directory.filePath(QStringLiteral("chat-8.lock")));
        QVERIFY(live.tryLock());
        QFile oldLease(directory.filePath(QStringLiteral("chat-8.lock")));
        QVERIFY(oldLease.open(QIODevice::ReadWrite));
        QVERIFY(oldLease.setFileTime(old, QFileDevice::FileModificationTime));
        oldLease.close();
        for (const auto &name : {QStringLiteral("shared"), QStringLiteral("orphan"), QStringLiteral("live")})
            QVERIFY(write(directory.filePath(QStringLiteral("images/") + name), "image", old));
        QVERIFY(write(directory.filePath(QStringLiteral("chat-9.json.old-archive")), "{}", old));
        QVERIFY(write(directory.filePath(QStringLiteral("legacy.json")), "{}", now));
        QVERIFY(write(directory.filePath(QStringLiteral("latest/legacy-package.json")), R"({"key":"legacy"})", old));
        conversations.cleanup(now);
        QCOMPARE(conversations.latest(QStringLiteral("legacy-package")), QStringLiteral("legacy"));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("chat-11.json"))));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("chat-10.json"))));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("chat-9.json.old-archive"))));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("images/orphan"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("images/shared"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("images/live"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("chat-8.json"))));
        QCOMPARE(conversations.latest(QStringLiteral("slack")), QStringLiteral("chat-9"));
        QCOMPARE(conversations.description(QStringLiteral("chat-9")), QStringLiteral("Slack task 9"));
        live.unlock();
        conversations.cleanup(now);
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("chat-8.json"))));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("images/live"))));
    }
    void agentNamesSessionThroughInjectedTool() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto agentProfile = profile(fakeAgent(directory.path()));
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("named-chat"), {});
            chat.setConversationScope(QStringLiteral("slack"));
            chat.resize(500, 650);
            chat.show();
            auto *title = chat.findChild<QComboBox *>(QStringLiteral("sessionTitle"));
            QVERIFY(title != nullptr);
            QCOMPARE(title->currentText(), QStringLiteral("New conversation"));
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("name-session")));
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY2(completed.first().first().toString().isEmpty(), qPrintable(completed.first().first().toString()));
            QTRY_COMPARE(title->currentText(), QStringLiteral("Fix Slack automatic updates"));
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_SESSION_SCREENSHOT")) chat.grab().save(qEnvironmentVariable("PACSMITH_TEST_SESSION_SCREENSHOT"));
        }
        {
            pacsmith::gui::AcpChatWidget restored(agentProfile, {}, QStringLiteral("named-chat"), {});
            restored.setConversationScope(QStringLiteral("slack"));
            QCOMPARE(restored.findChild<QComboBox *>(QStringLiteral("sessionTitle"))->currentText(), QStringLiteral("Fix Slack automatic updates"));
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void toolCallsUpdateOneRowAndRestore() {
        pacsmith::gui::ChatTranscript transcript;
        transcript.resize(650, 650); transcript.show();
        transcript.message(QStringLiteral("user"), QStringLiteral("Check automatic updates"));
        transcript.message(QStringLiteral("assistant"), QStringLiteral("I will inspect the update configuration."));
        transcript.toolCall({{QStringLiteral("toolCallId"), QStringLiteral("exec-123")}, {QStringLiteral("title"), QStringLiteral("Inspect update configuration")}, {QStringLiteral("status"), QStringLiteral("in_progress")}});
        const auto visibleCount = [&](const QString &name) {
            int count = 0;
            for (auto *widget : transcript.findChildren<QWidget *>(name)) if (widget->isVisible()) ++count;
            return count;
        };
        QTRY_COMPARE(visibleCount(QStringLiteral("toolSpinner")), 1);
        QTRY_COMPARE(visibleCount(QStringLiteral("toolIcon")), 1);
        const auto *toolRow = transcript.findChild<QWidget *>(QStringLiteral("toolCallRow"));
        const auto *wrench = toolRow->findChild<QWidget *>(QStringLiteral("toolIcon"));
        const auto *spinner = toolRow->findChild<QWidget *>(QStringLiteral("toolSpinner"));
        QVERIFY(wrench->mapTo(toolRow, QPoint()).x() < spinner->mapTo(toolRow, QPoint()).x());
        QVERIFY(spinner->mapTo(toolRow, QPoint()).x() > toolRow->width() - 30);
        if (qEnvironmentVariableIsSet("PACSMITH_TEST_SPINNER_SCREENSHOT")) transcript.grab().save(qEnvironmentVariable("PACSMITH_TEST_SPINNER_SCREENSHOT"));
        QCOMPARE(transcript.entries().size(), 3);
        transcript.toolCall({{QStringLiteral("toolCallId"), QStringLiteral("exec-123")}, {QStringLiteral("status"), QStringLiteral("completed")}});
        QCOMPARE(transcript.entries().size(), 3);
        QTRY_COMPARE(visibleCount(QStringLiteral("toolSpinner")), 0);
        QTRY_COMPARE(visibleCount(QStringLiteral("toolCallRow")), 1);
        QTRY_VERIFY(visibleCount(QStringLiteral("toolIcon")) == 1);
        QCOMPARE(transcript.entries().last().toObject().value(QStringLiteral("text")).toString(), QStringLiteral("Inspect update configuration"));
        QVERIFY(!transcript.toPlainText().contains(QStringLiteral("exec-123")));
        pacsmith::gui::ChatTranscript restored;
        restored.restore(transcript.entries());
        QCOMPARE(restored.entries(), transcript.entries());
        restored.restoreLegacy(QStringLiteral("You\nCheck updates\n\nAgent\nChecking.\n\nTool: Inspect configuration · in_progress\n\nTool: exec-123 · completed"));
        QCOMPARE(restored.entries().size(), 3);
        QVERIFY(!restored.toPlainText().contains(QStringLiteral("exec-123")));
        QVERIFY(restored.findChild<QWidget *>(QStringLiteral("toolIcon")) != nullptr);
        if (qEnvironmentVariableIsSet("PACSMITH_TEST_TRANSCRIPT_SCREENSHOT")) transcript.grab().save(qEnvironmentVariable("PACSMITH_TEST_TRANSCRIPT_SCREENSHOT"));
        transcript.beginTurn();
        transcript.toolCall({{QStringLiteral("toolCallId"), QStringLiteral("exec-123")}, {QStringLiteral("title"), QStringLiteral("Inspect again")}});
        transcript.toolCall({{QStringLiteral("toolCallId"), QStringLiteral("exec-456")}, {QStringLiteral("title"), QStringLiteral("Check settings")}});
        transcript.toolCall({{QStringLiteral("toolCallId"), QStringLiteral("exec-123")}, {QStringLiteral("status"), QStringLiteral("failed")}});
        QCOMPARE(transcript.entries().size(), 5);
        QCOMPARE(transcript.entries().at(3).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        transcript.endTurn(false);
        QCOMPARE(transcript.entries().last().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("interrupted"));
        QTRY_COMPARE(visibleCount(QStringLiteral("toolSpinner")), 0);
    }
    void rejectedDefaultsDoNotBlockTheSession() {
        QTemporaryDir directory;
        auto agentProfile = profile(fakeAgent(directory.path()));
        agentProfile.arguments.append(QStringLiteral("--reject-fast"));
        agentProfile.configDefaults = {{QStringLiteral("provider.fast"), true}, {QStringLiteral("removed.option"), QStringLiteral("old")}};
        AcpClient client;
        QSignalSpy ready(&client, &AcpClient::ready);
        QSignalSpy warnings(&client, &AcpClient::configurationWarning);
        QSignalSpy applied(&client, &AcpClient::configOptionApplied);
        client.start(agentProfile, environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(warnings.count(), 2);
        QVERIFY(!client.isBusy());
        client.setConfigOption(QStringLiteral("provider.fast"), true);
        QTRY_COMPARE(warnings.count(), 3);
        QCOMPARE(applied.count(), 0);
        QVERIFY(!client.isBusy());
        QVERIFY(client.isReady());
        QCOMPARE(client.configOptions().last().toObject().value(QStringLiteral("currentValue")).toBool(), false);
        client.setConfigOption(QStringLiteral("model"), QStringLiteral("deliberate"));
        QTRY_COMPARE(applied.count(), 1);
    }
    void legacyImagesStayWithTheirMessages() {
        pacsmith::gui::ChatTranscript transcript;
        const QJsonObject first{{QStringLiteral("file"), QStringLiteral("first")}, {QStringLiteral("name"), QStringLiteral("one.png")}};
        const QJsonObject second{{QStringLiteral("file"), QStringLiteral("second")}, {QStringLiteral("name"), QStringLiteral("two.png")}};
        transcript.message(QStringLiteral("user"), QStringLiteral("Explain this\n[Images: one.png]"));
        transcript.message(QStringLiteral("assistant"), QStringLiteral("Here is the explanation."));
        transcript.message(QStringLiteral("user"), QStringLiteral("\n[Images: two.png]"));
        transcript.restoreLegacyImages({first, second});
        QCOMPARE(transcript.entries().size(), 3);
        QCOMPARE(transcript.entries().first().toObject().value(QStringLiteral("text")).toString(), QStringLiteral("Explain this"));
        QCOMPARE(transcript.entries().first().toObject().value(QStringLiteral("images")).toArray(), QJsonArray{first});
        QCOMPARE(transcript.entries().last().toObject().value(QStringLiteral("images")).toArray(), QJsonArray{second});
        QVERIFY(transcript.entries().last().toObject().value(QStringLiteral("text")).toString().isEmpty());
        transcript.clear();
        transcript.restoreLegacyImages({first});
        QCOMPARE(transcript.entries().first().toObject().value(QStringLiteral("images")).toArray(), QJsonArray{first});
    }
    void composerActionSendsAndStops() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        {
            pacsmith::gui::AcpChatWidget chat(profile(fakeAgent(directory.path())), {}, QStringLiteral("composer-action"), {});
            chat.resize(460, 700); chat.show();
            auto *action = chat.findChild<QPushButton *>(QStringLiteral("chatAction"));
            auto *composer = chat.findChild<QPlainTextEdit *>(QStringLiteral("aiComposer"));
            auto *frame = chat.findChild<QWidget *>(QStringLiteral("chatComposerFrame"));
            QVERIFY(action != nullptr); QVERIFY(frame->isAncestorOf(action));
            QVERIFY(action->text().isEmpty()); QVERIFY(!action->icon().isNull());
            QCOMPARE(action->accessibleName(), QStringLiteral("Send message"));
            QTRY_VERIFY(action->mapTo(frame, action->rect().bottomRight()).x() >= frame->width() - 8);
            QVERIFY(action->mapTo(frame, action->rect().bottomRight()).y() >= frame->height() - 8);
            composer->setPlainText(QStringLiteral("cancel"));
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            action->click();
            QTRY_VERIFY(chat.isBusy());
            QCOMPARE(action->accessibleName(), QStringLiteral("Stop agent"));
            QVERIFY(action->isEnabled()); QVERIFY(composer->isReadOnly());
            // Wait until the request reaches the agent to exercise cancellation during a turn.
            auto *transcript = chat.findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QTRY_VERIFY(transcript->toPlainText().contains(QStringLiteral("Hello world")));
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_STOP_SCREENSHOT")) chat.grab().save(qEnvironmentVariable("PACSMITH_TEST_STOP_SCREENSHOT"));
            action->click();
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(!chat.isBusy()); QVERIFY(!composer->isReadOnly());
            QCOMPARE(action->accessibleName(), QStringLiteral("Send message"));
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void providerDefaultsAreDiscoveredSavedAndApplied() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        auto agentProfile = profile(fakeAgent(directory.path()));
        const auto requests = directory.filePath(QStringLiteral("requests.jsonl"));
        agentProfile.environment.insert(QStringLiteral("PACSMITH_TEST_REQUESTS"), requests);
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        QVERIFY(store.setHarness(agentProfile));
        {
            pacsmith::gui::AgentSettingsDialog dialog(agentProfile, {}, [&](const QJsonObject &values, QString *error) {
                agentProfile.configDefaults = values;
                return store.setHarness(agentProfile, error);
            });
            dialog.show();
            const auto combo = [&](const QString &id) -> QComboBox * {
                for (auto *box : dialog.findChildren<QComboBox *>(id)) if (box->isVisible() && box->isEnabled()) return box;
                return nullptr;
            };
            QTRY_VERIFY(combo(QStringLiteral("model")) != nullptr);
            auto *model = combo(QStringLiteral("model"));
            QCOMPARE(model->count(), 2);
            model->setCurrentIndex(1); model->activated(1);
            QTRY_COMPARE(store.load().harness->configDefaults.value(QStringLiteral("model")).toString(), QStringLiteral("deliberate"));
            QTRY_VERIFY(combo(QStringLiteral("reasoning")) != nullptr);
            auto *reasoning = combo(QStringLiteral("reasoning"));
            QCOMPARE(reasoning->count(), 2);
            reasoning->setCurrentIndex(1); reasoning->activated(1);
            QTRY_COMPARE(store.load().harness->configDefaults.value(QStringLiteral("reasoning")).toString(), QStringLiteral("high"));
            QCheckBox *fast = nullptr;
            for (auto *check : dialog.findChildren<QCheckBox *>(QStringLiteral("provider.fast"))) if (check->isVisible()) fast = check;
            QVERIFY(fast != nullptr); fast->click();
            QTRY_VERIFY(store.load().harness->configDefaults.value(QStringLiteral("provider.fast")).toBool());
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_DEFAULTS_SCREENSHOT")) dialog.grab().save(qEnvironmentVariable("PACSMITH_TEST_DEFAULTS_SCREENSHOT"));
        }
        QFile record(requests); QVERIFY(record.open(QIODevice::ReadOnly));
        QVERIFY(!record.readAll().contains("session/prompt")); record.close();
        agentProfile = *store.load().harness;
        AcpClient client;
        QSignalSpy ready(&client, &AcpClient::ready);
        client.start(agentProfile, environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        QJsonObject values;
        for (const auto &entry : client.configOptions()) { const auto option = entry.toObject(); values.insert(option.value(QStringLiteral("id")).toString(), option.value(QStringLiteral("currentValue"))); }
        QCOMPARE(values, agentProfile.configDefaults);
        client.close();
        QVERIFY(record.open(QIODevice::WriteOnly | QIODevice::Truncate)); record.close();
        agentProfile.environment.insert(QStringLiteral("PACSMITH_TEST_REQUESTS"), requests);
        auto resumedEnvironment = environment(directory.path());
        resumedEnvironment.process.insert(QStringLiteral("PACSMITH_TEST_REQUESTS"), requests);
        client.start(agentProfile, resumedEnvironment, acpMcpServers({}, QStringLiteral("/test/pacsmith")), QStringLiteral("private-session"));
        QTRY_COMPARE(ready.count(), 2);
        QVERIFY(record.open(QIODevice::ReadOnly));
        QVERIFY(!record.readAll().contains("session/set_config_option"));
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void composerKeyboardShortcuts() {
        pacsmith::gui::ChatImageInput input;
        int sends = 0;
        input.send = [&] { ++sends; };
        QTest::keyClick(&input, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(input.toPlainText(), QStringLiteral("\n"));
        QCOMPARE(sends, 0);
        QTest::keyClick(&input, Qt::Key_Return);
        QTest::keyClick(&input, Qt::Key_Enter, Qt::KeypadModifier);
        QCOMPARE(sends, 2);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, {}, true);
        QApplication::sendEvent(&input, &repeat);
        QCOMPARE(sends, 2);
        for (const auto modifier : {Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier}) {
            QTest::keyClick(&input, Qt::Key_Return, modifier);
        }
        QCOMPARE(sends, 2);
        QInputMethodEvent composing(QStringLiteral("test"), {});
        QApplication::sendEvent(&input, &composing);
        QTest::keyClick(&input, Qt::Key_Return);
        QCOMPARE(sends, 2);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("test"));
        QApplication::sendEvent(&input, &commit);
        QTest::keyClick(&input, Qt::Key_Return);
        QCOMPARE(sends, 3);
    }
    void registryProfilesAndOfflineCache() {
        QTemporaryDir directory;
        const QByteArray payload = R"({"agents":[{"id":"codex-acp","name":"Codex","version":"1.2.3","description":"Test","distribution":{"npx":{"package":"@agentclientprotocol/codex-acp@1.2.3","args":["--test"],"env":{"TEST_AGENT_SETTING":"1"}}}},{"id":"python","name":"Python agent","version":"2","distribution":{"uvx":{"package":"agent==2","args":["acp"]}}},{"id":"binary","name":"Binary","version":"1","distribution":{"binary":{"linux-x86_64":{}}}}]})";
        QString error;
        int fetches = 0;
        const auto fetch = [&](QString *) { ++fetches; return payload; };
        auto result = loadAcpRegistry(directory.path(), false, &error, fetch);
        QVERIFY2(result.has_value(), qPrintable(error));
        QCOMPARE(result->agents.size(), 3);
        QCOMPARE(fetches, 1);
        result = loadAcpRegistry(directory.path(), false, &error, fetch);
        QVERIFY(result->fromCache);
        QCOMPARE(fetches, 1);
        result = loadAcpRegistry(directory.path(), true, &error, [](QString *reason) { *reason = QStringLiteral("offline"); return QByteArray{}; });
        QVERIFY(result->fromCache);
        QVERIFY(result->notice.contains(QStringLiteral("offline")));
        const auto codex = acpRegistryProfile(result->agents.at(1), &error, QStringLiteral("/test/npx"), {});
        QVERIFY(codex.has_value());
        QCOMPARE(codex->arguments, (QStringList{QStringLiteral("-y"), QStringLiteral("@agentclientprotocol/codex-acp@1.2.3"), QStringLiteral("--test")}));
        QCOMPARE(codex->registryId, QStringLiteral("codex-acp"));
        QCOMPARE(codex->environment.value(QStringLiteral("TEST_AGENT_SETTING")), QStringLiteral("1"));
        QVERIFY(!acpRegistryProfile(result->agents.at(1), &error, {}, {}));
        QVERIFY(error.contains(QStringLiteral("requires npx")));
        QVERIFY(!acpRegistryProfile(result->agents.first(), &error, QStringLiteral("npx"), QStringLiteral("uvx")));
        const auto python = acpRegistryProfile(result->agents.last(), &error, {}, QStringLiteral("/test/uvx"));
        QVERIFY(python.has_value());
        QCOMPARE(python->arguments, (QStringList{QStringLiteral("agent==2"), QStringLiteral("acp")}));
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        error.clear();
        QVERIFY2(store.setHarness(*codex, &error), qPrintable(error));
        QCOMPARE(store.load().harness->environment, codex->environment);
        QCOMPARE(store.load().harness->registryVersion, QStringLiteral("1.2.3"));
        QVERIFY(!parseAcpRegistry(QByteArray("not JSON"), &error));
    }
    void questionUsesFreshScreenContextAndImages() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        auto agentProfile = profile(fakeAgent(directory.path()));
        agentProfile.arguments.append(QStringLiteral("--images"));
        const auto requests = directory.filePath(QStringLiteral("requests.jsonl"));
        agentProfile.environment.insert(QStringLiteral("PACSMITH_TEST_REQUESTS"), requests);
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("screen"), {});
            chat.resize(480, 800); chat.show();
            auto *composer = chat.findChild<QPlainTextEdit *>(QStringLiteral("aiComposer"));
            auto *newChat = chat.findChild<QPushButton *>(QStringLiteral("newChat"));
            auto *transcript = chat.findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QTRY_VERIFY(newChat->mapTo(&chat, QPoint()).y() < transcript->mapTo(&chat, QPoint()).y());
            for (const auto &name : {QStringLiteral("attachImage"), QStringLiteral("conversationSettings")}) {
                auto *button = chat.findChild<QPushButton *>(name);
                QVERIFY(button != nullptr); QVERIFY(!button->icon().isNull()); QVERIFY(button->text().isEmpty());
                QVERIFY(button->mapTo(&chat, QPoint()).x() < composer->mapTo(&chat, QPoint()).x());
            }
            auto *settings = chat.findChild<QPushButton *>(QStringLiteral("conversationSettings"));
            auto *attach = chat.findChild<QPushButton *>(QStringLiteral("attachImage"));
            QCOMPARE(settings->mapTo(&chat, QPoint()).x(), attach->mapTo(&chat, QPoint()).x());
            QVERIFY(settings->mapTo(&chat, QPoint()).y() < attach->mapTo(&chat, QPoint()).y());
            for (const auto *button : chat.findChildren<QPushButton *>()) QVERIFY(button->text() != QStringLiteral("Sent images"));
            QString screen = QStringLiteral("Dependencies");
            chat.setContextProvider([&] { return std::optional{pacsmith::gui::ChatScreenContext{screen, QStringLiteral("Current screen: ") + screen}}; });
            QVERIFY(!chat.isBusy());
            QVERIFY(!QFile::exists(requests));
            QImage screenshot(480, 260, QImage::Format_RGB32); screenshot.fill(QColor(QStringLiteral("#182C38")));
            {
                QPainter painter(&screenshot);
                painter.setPen(QColor(QStringLiteral("#80D4E5")));
                auto font = painter.font(); font.setPixelSize(24); painter.setFont(font);
                painter.drawText(24, 52, QStringLiteral("Package update status"));
                painter.setPen(Qt::white); font.setPixelSize(18); painter.setFont(font);
                painter.drawText(24, 110, QStringLiteral("Slack — review needed"));
                painter.drawText(24, 150, QStringLiteral("Signal — up to date"));
            }
            QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(screenshot.save(&buffer, "PNG"));
            QApplication::clipboard()->setImage(screenshot);
            composer->setPlainText(QStringLiteral("What does this mean?"));
            composer->setFocus();
            QTest::keyClick(composer, Qt::Key_V, Qt::ControlModifier);
            QTRY_VERIFY(chat.findChild<QLabel *>(QStringLiteral("draftImage")) != nullptr);
            QCOMPARE(composer->toPlainText(), QStringLiteral("What does this mean?"));
            screen = QStringLiteral("Repository");
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QTest::keyClick(composer, Qt::Key_Return);
            QTRY_COMPARE(completed.count(), 1);
            const auto userMessage = transcript->entries().first().toObject();
            QCOMPARE(userMessage.value(QStringLiteral("text")).toString(), QStringLiteral("What does this mean?"));
            QCOMPARE(userMessage.value(QStringLiteral("images")).toArray().size(), 1);
            auto *inlineImage = transcript->findChild<QPushButton *>(QStringLiteral("chatImage"));
            QVERIFY(inlineImage != nullptr); QVERIFY(inlineImage->isVisible());
            inlineImage->click();
            auto *preview = transcript->findChild<QDialog *>(QStringLiteral("chatImagePreview"));
            QVERIFY(preview != nullptr); QVERIFY(preview->isVisible()); preview->close();
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_INLINE_IMAGE_SCREENSHOT")) chat.grab().save(qEnvironmentVariable("PACSMITH_TEST_INLINE_IMAGE_SCREENSHOT"));
            QFile record(requests); QVERIFY(record.open(QIODevice::ReadOnly));
            QJsonArray sent;
            for (const auto &line : record.readAll().split('\n')) {
                const auto message = QJsonDocument::fromJson(line).object();
                if (message.value(QStringLiteral("method")).toString() == QStringLiteral("session/prompt")) sent = message.value(QStringLiteral("params")).toObject().value(QStringLiteral("prompt")).toArray();
            }
            QCOMPARE(sent.size(), 2);
            QVERIFY(sent.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("Repository")));
            QVERIFY(!sent.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("Dependencies")));
            QVERIFY(sent.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("What does this mean?")));
            QCOMPARE(sent.last().toObject().value(QStringLiteral("mimeType")).toString(), QStringLiteral("image/png"));
            QCOMPARE(QByteArray::fromBase64(sent.last().toObject().value(QStringLiteral("data")).toString().toLatin1()), bytes);
            composer->setFocus();
            QTest::keyClick(composer, Qt::Key_V, Qt::ControlModifier);
            QTest::keyClick(composer, Qt::Key_Return);
            QTRY_COMPARE(completed.count(), 2);
            const auto imageOnly = transcript->entries().at(2).toObject();
            QVERIFY(imageOnly.value(QStringLiteral("text")).toString().isEmpty());
            QCOMPARE(imageOnly.value(QStringLiteral("images")).toArray().size(), 1);
        }
        {
            pacsmith::gui::AcpChatWidget reopened(agentProfile, {}, QStringLiteral("screen"), {});
            auto *transcript = reopened.findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QCOMPARE(transcript->entries().first().toObject().value(QStringLiteral("images")).toArray().size(), 1);
            QCOMPARE(transcript->findChildren<QPushButton *>(QStringLiteral("chatImage")).size(), 2);
            QVERIFY(reopened.findChild<QLabel *>(QStringLiteral("draftImage")) == nullptr);
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void unsupportedImagesStayInComposer() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        {
            pacsmith::gui::AcpChatWidget chat(profile(fakeAgent(directory.path())), {}, QStringLiteral("no-images"), {});
            QImage screenshot(10, 10, QImage::Format_RGB32); screenshot.fill(Qt::blue);
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); QVERIFY(screenshot.save(&buffer, "PNG"));
            QVERIFY(chat.attachImage(bytes, QStringLiteral("screen.png")));
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("Explain this")));
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(completed.first().first().toString().contains(QStringLiteral("does not support images")));
            QCOMPARE(chat.findChild<QPlainTextEdit *>(QStringLiteral("aiComposer"))->toPlainText(), QStringLiteral("Explain this"));
            int removals = 0;
            for (auto *button : chat.findChildren<QPushButton *>()) if (button->text() == QStringLiteral("Remove")) ++removals;
            QCOMPARE(removals, 1);
            QVERIFY(!chat.attachImage(QByteArray("invalid"), QStringLiteral("not-an-image.png")));
        }
        {
            pacsmith::gui::AcpChatWidget reopened(profile(fakeAgent(directory.path())), {}, QStringLiteral("no-images"), {});
            QCOMPARE(reopened.findChild<QPlainTextEdit *>(QStringLiteral("aiComposer"))->toPlainText(), QStringLiteral("Explain this"));
            int removals = 0;
            for (auto *button : reopened.findChildren<QPushButton *>()) if (button->text() == QStringLiteral("Remove")) ++removals;
            QCOMPARE(removals, 1);
            QVERIFY(!reopened.isBusy());
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void chatPersistsAndStartsFresh() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto agentProfile = profile(fakeAgent(directory.path()));
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("test"), {});
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            chat.resize(600, 700);
            chat.show();
            QVERIFY(chat.submit(QStringLiteral("hello-tool")));
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(completed.first().first().toString().isEmpty());
            QVERIFY(!chat.isBusy());
            auto *transcript = chat.findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QVERIFY(transcript != nullptr);
            QVERIFY(transcript->toPlainText().contains(QStringLiteral("Hello world")));
            QCOMPARE(transcript->findChildren<QWidget *>(QStringLiteral("toolCallRow")).size(), 1);
            QVERIFY(transcript->toPlainText().contains(QStringLiteral("Inspect package")));
            QVERIFY(transcript->toPlainText().contains(QStringLiteral("<b>Package details</b>")));
            QVERIFY(transcript->findChild<QWidget *>(QStringLiteral("toolIcon")) != nullptr);
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_TOOL_SCREENSHOT")) chat.grab().save(qEnvironmentVariable("PACSMITH_TEST_TOOL_SCREENSHOT"));
        }
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("test"), {});
            auto *transcript = chat.findChild<pacsmith::gui::ChatTranscript *>(QStringLiteral("aiTranscript"));
            QCOMPARE(transcript->toPlainText().count(QStringLiteral("Hello world")), 1);
            QCOMPARE(transcript->findChildren<QWidget *>(QStringLiteral("toolCallRow")).size(), 1);
            QVERIFY(transcript->findChild<QWidget *>(QStringLiteral("toolIcon")) != nullptr);
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("follow-up")));
            QTRY_COMPARE(completed.count(), 1);
            QCOMPARE(transcript->toPlainText().count(QStringLiteral("Hello world")), 2);
            QVERIFY(!transcript->toPlainText().contains(QStringLiteral("replayed")));
            int defaultsReads = 0;
            chat.setDefaultsProvider([&] { ++defaultsReads; return QJsonObject{{QStringLiteral("model"), QStringLiteral("deliberate")}}; });
            chat.findChild<QPushButton *>(QStringLiteral("newChat"))->click();
            QTRY_VERIFY(!chat.isBusy());
            QCOMPARE(defaultsReads, 1);
            QVERIFY(transcript->toPlainText().isEmpty());
            QFile record(QDir(acpDataDirectory()).filePath(QStringLiteral("conversations/%1.json").arg(chat.objectName())));
            QVERIFY(record.open(QIODevice::ReadOnly));
            QVERIFY(QJsonDocument::fromJson(record.readAll()).object().value(QStringLiteral("transcript")).toString().isEmpty());
            QFile previousChat(QDir(acpDataDirectory()).filePath(QStringLiteral("conversations/test.json")));
            QVERIFY(previousChat.open(QIODevice::ReadOnly));
            QVERIFY(QJsonDocument::fromJson(previousChat.readAll()).object().value(QStringLiteral("transcript")).toString().contains(QStringLiteral("Hello world")));
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME");
        else qputenv("XDG_DATA_HOME", previous);
    }
    void migratesOnlyPreviouslySelectedHarness() {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("settings.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"harnessProfiles":[{"name":"First","executable":"first-acp","protocol":"acp"},{"name":"Selected","executable":"selected-acp","protocol":"acp","default":true,"configDefaults":{"model":"saved-model"}},{"name":"Third","executable":"third-acp","protocol":"acp"}]})");
        file.close();
        AppSettingsStore store(directory.path());
        auto settings = store.load();
        QVERIFY(settings.harness.has_value());
        QCOMPARE(settings.harness->name, QStringLiteral("Selected"));
        QCOMPARE(settings.harness->configDefaults.value(QStringLiteral("model")).toString(), QStringLiteral("saved-model"));
        QVERIFY(store.save(settings));
        QCOMPARE(store.load().harness->name, QStringLiteral("Selected"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!saved.contains(QStringLiteral("harnessProfiles")));
        QVERIFY(!saved.value(QStringLiteral("harness")).toObject().contains(QStringLiteral("default")));
    }
    void ignoresRetiredLaunchProfiles() {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("settings.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"harnessProfiles":[{"name":"Old terminal","executable":"kitty","arguments":["codex","{prompt}"]},{"name":"Unmarked adapter","executable":"codex-acp"},{"name":"Retired","executable":"old-agent","protocol":"legacy"},{"name":"ACP","executable":"codex-acp","protocol":"acp"}]})");
        file.close();
        AppSettingsStore store(directory.path());
        const auto settings = store.load();
        QVERIFY(settings.harness.has_value());
        QCOMPARE(settings.harness->name, QStringLiteral("ACP"));
        QVERIFY(store.save(settings));
        QVERIFY(store.load().harness.has_value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!saved.contains(QStringLiteral("harnessProfiles")));
        QCOMPARE(saved.value(QStringLiteral("harness")).toObject().value(QStringLiteral("protocol")).toString(), QStringLiteral("acp"));
    }
    void isolateCodexStorage() {
        QTemporaryDir directory;
        const auto personal = directory.filePath(QStringLiteral("personal"));
        QVERIFY(QDir().mkpath(personal));
        QFile auth(QDir(personal).filePath(QStringLiteral("auth.json")));
        QVERIFY(auth.open(QIODevice::WriteOnly)); auth.write("fake-login"); auth.close();
        QFile history(QDir(personal).filePath(QStringLiteral("history.jsonl")));
        QVERIFY(history.open(QIODevice::WriteOnly)); history.write("personal conversation"); history.close();
        auto parent = QProcessEnvironment::systemEnvironment();
        parent.insert(QStringLiteral("CODEX_HOME"), personal);
        parent.insert(QStringLiteral("CODEX_SQLITE_HOME"), personal);
        parent.insert(QStringLiteral("CODEX_CONFIG"), QStringLiteral("{\"sqlite_home\":\"personal\"}"));
        parent.insert(QStringLiteral("CODEX_THREAD_ID"), QStringLiteral("personal-thread"));
        HarnessProfile codex;
        codex.executable = QStringLiteral("npx");
        codex.arguments = {QStringLiteral("--yes"), QStringLiteral("@agentclientprotocol/codex-acp")};
        QVERIFY(isCodexAcp(codex));
        QString error;
        const auto isolated = prepareAcpEnvironment(codex, directory.filePath(QStringLiteral("pacsmith")), parent, &error);
        QVERIFY2(isolated.has_value(), qPrintable(error));
        const auto home = isolated->process.value(QStringLiteral("CODEX_HOME"));
        QVERIFY(home != personal);
        QCOMPARE(isolated->process.value(QStringLiteral("CODEX_SQLITE_HOME")), home);
        QCOMPARE(QJsonDocument::fromJson(isolated->process.value(QStringLiteral("CODEX_CONFIG")).toUtf8()).object().value(QStringLiteral("sqlite_home")).toString(), home);
        QCOMPARE(isolated->process.value(QStringLiteral("INITIAL_AGENT_MODE")), QStringLiteral("read-only"));
        QVERIFY(!isolated->process.contains(QStringLiteral("CODEX_THREAD_ID")));
        QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("auth.json"))).isSymLink());
        QVERIFY(!QFileInfo::exists(QDir(home).filePath(QStringLiteral("history.jsonl"))));
        QVERIFY(history.open(QIODevice::ReadOnly)); QCOMPARE(history.readAll(), QByteArray("personal conversation"));
        QCOMPARE(parent.value(QStringLiteral("CODEX_HOME")), personal);
        codex.arguments.append(QStringLiteral("sqlite_home=personal"));
        QVERIFY(!prepareAcpEnvironment(codex, directory.filePath(QStringLiteral("pacsmith")), parent, &error));
    }
    void streamsAndResumesOnlyRequestedSession() {
        QTemporaryDir directory;
        const auto agentProfile = profile(fakeAgent(directory.path()));
        AcpClient client;
        QSignalSpy ready(&client, &AcpClient::ready);
        QSignalSpy updates(&client, &AcpClient::updated);
        QSignalSpy finished(&client, &AcpClient::turnFinished);
        QSignalSpy errors(&client, &AcpClient::failed);
        client.start(agentProfile, environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(client.sessionId(), QStringLiteral("private-session"));
        client.prompt(QStringLiteral("hello"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(updates.count(), 2);
        QCOMPARE(updates.at(0).at(0).toJsonObject().value(QStringLiteral("content")).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("Hello "));
        QCOMPARE(errors.count(), 0);
        client.close();
        client.start(agentProfile, environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")), QStringLiteral("private-session"));
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(updates.count(), 3);
        QVERIFY(updates.last().at(1).toBool());
    }
    void rememberedPermissionsAreScopedAndRevocable() {
        QTemporaryDir directory;
        const QJsonObject identity{{QStringLiteral("endpoint"), QStringLiteral("library-a")}};
        AcpToolPermissions permissions(directory.path(), identity);
        QJsonObject params{{QStringLiteral("sessionId"), QStringLiteral("session-a")},
            {QStringLiteral("toolCall"), QJsonObject{
                {QStringLiteral("_meta"), QJsonObject{{QStringLiteral("is_mcp_tool_call"), true}}},
                {QStringLiteral("rawInput"), QJsonObject{{QStringLiteral("server"), QStringLiteral("pacsmith_session")}, {QStringLiteral("tool"), QStringLiteral("start_build")}}}}},
            {QStringLiteral("options"), QJsonArray{
                QJsonObject{{QStringLiteral("optionId"), QStringLiteral("once")}, {QStringLiteral("kind"), QStringLiteral("allow_once")}},
                QJsonObject{{QStringLiteral("optionId"), QStringLiteral("allow_session")}, {QStringLiteral("kind"), QStringLiteral("allow_always")}},
                QJsonObject{{QStringLiteral("optionId"), QStringLiteral("allow_always")}, {QStringLiteral("kind"), QStringLiteral("allow_always")}}}}};
        QString error;
        QVERIFY(permissions.remember(params, QStringLiteral("once"), &error));
        QVERIFY(permissions.remembered(params).isEmpty());
        QVERIFY(permissions.remember(params, QStringLiteral("allow_session"), &error));
        QCOMPARE(AcpToolPermissions(directory.path(), identity).remembered(params), QStringLiteral("once"));
        auto otherSession = params; otherSession.insert(QStringLiteral("sessionId"), QStringLiteral("session-b"));
        QVERIFY(permissions.remembered(otherSession).isEmpty());
        QVERIFY(permissions.remember(params, QStringLiteral("allow_always"), &error));
        QCOMPARE(AcpToolPermissions(directory.path(), identity).remembered(otherSession), QStringLiteral("once"));
        QVERIFY(AcpToolPermissions(directory.path(), {{QStringLiteral("endpoint"), QStringLiteral("library-b")}}).remembered(params).isEmpty());
        for (const auto &tool : {QStringLiteral("uninstall_package"), QStringLiteral("start_build")}) {
            auto foreign = params;
            auto call = foreign.value(QStringLiteral("toolCall")).toObject();
            call.insert(QStringLiteral("rawInput"), QJsonObject{{QStringLiteral("server"), tool == QStringLiteral("start_build") ? QStringLiteral("another_server") : QStringLiteral("pacsmith_session")}, {QStringLiteral("tool"), tool}});
            foreign.insert(QStringLiteral("toolCall"), call);
            QVERIFY(permissions.remembered(foreign).isEmpty());
        }
        auto shell = params;
        shell.insert(QStringLiteral("toolCall"), QJsonObject{{QStringLiteral("title"), QStringLiteral("start_build")}, {QStringLiteral("rawInput"), QJsonObject{{QStringLiteral("command"), QStringLiteral("echo example")}}}});
        QVERIFY(permissions.remember(shell, QStringLiteral("allow_always"), &error));
        QVERIFY(permissions.remembered(shell).isEmpty());
        QVERIFY(permissions.clear(&error));
        QVERIFY(permissions.remembered(params).isEmpty());
    }
    void chatApprovalButtonsAndAlwaysAllowSurviveRestart() {
        QTemporaryDir directory;
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        auto agentProfile = profile(fakeAgent(directory.path()));
        agentProfile.arguments.append(QStringLiteral("codex-acp"));
        const auto choice = [](QWidget &chat, const QString &id) -> QPushButton * {
            for (auto *button : chat.findChildren<QPushButton *>(QStringLiteral("permissionChoice")))
                if (button->isEnabled() && button->isVisible() && button->property("optionId").toString() == id) return button;
            return nullptr;
        };
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("permission-a"), {});
            chat.resize(500, 850); chat.show();
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("permission")));
            QTRY_VERIFY(choice(chat, QStringLiteral("allow_always")) != nullptr);
            QVERIFY(choice(chat, QStringLiteral("yes")) != nullptr);
            QVERIFY(choice(chat, QStringLiteral("allow_session")) != nullptr);
            QVERIFY(choice(chat, QStringLiteral("no")) != nullptr);
            if (qEnvironmentVariableIsSet("PACSMITH_TEST_PERMISSION_SCREENSHOT")) chat.grab().save(qEnvironmentVariable("PACSMITH_TEST_PERMISSION_SCREENSHOT"));
            choice(chat, QStringLiteral("allow_always"))->click();
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(completed.first().first().toString().isEmpty());
        }
        {
            pacsmith::gui::AcpChatWidget chat(agentProfile, {}, QStringLiteral("permission-b"), {});
            chat.show();
            QSignalSpy requested(&chat, &pacsmith::gui::AcpChatWidget::approvalRequired);
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("permission")));
            QTRY_COMPARE(completed.count(), 1);
            QCOMPARE(requested.count(), 0);
            QVERIFY(chat.findChild<pacsmith::gui::ChatTranscript *>()->toPlainText().contains(QStringLiteral("Allowed by saved permission")));
        }
        {
            ConnectionConfig other; other.socketPath = directory.filePath(QStringLiteral("other.sock"));
            pacsmith::gui::AcpChatWidget chat(agentProfile, other, QStringLiteral("permission-c"), {});
            chat.show();
            QSignalSpy completed(&chat, &pacsmith::gui::AcpChatWidget::completed);
            QVERIFY(chat.submit(QStringLiteral("permission")));
            QTRY_VERIFY(choice(chat, QStringLiteral("no")) != nullptr);
            choice(chat, QStringLiteral("no"))->click();
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(chat.findChild<pacsmith::gui::ChatTranscript *>()->toPlainText().contains(QStringLiteral("Reject")));
        }
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", previous);
    }
    void fixedPermissionModeIsHidden() {
        QTemporaryDir directory;
        AcpClient client;
        auto agentProfile = profile(fakeAgent(directory.path()));
        agentProfile.arguments.append(QStringLiteral("codex-acp"));
        QSignalSpy ready(&client, &AcpClient::ready);
        client.start(agentProfile, environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        pacsmith::gui::AgentSettingsDialog dialog(&client, true);
        dialog.show();
        QVERIFY(dialog.findChild<QComboBox *>(QStringLiteral("mode")) == nullptr);
        QVERIFY(dialog.findChild<QComboBox *>(QStringLiteral("model")) != nullptr);
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Agent settings"));
    }
    void permissionsAndCancellation() {
        QTemporaryDir directory;
        AcpClient client;
        QSignalSpy ready(&client, &AcpClient::ready);
        QSignalSpy permissions(&client, &AcpClient::permissionRequested);
        QSignalSpy updates(&client, &AcpClient::updated);
        QSignalSpy finished(&client, &AcpClient::turnFinished);
        client.start(profile(fakeAgent(directory.path())), environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        client.prompt(QStringLiteral("permission"));
        QTRY_COMPARE(permissions.count(), 1);
        const auto params = permissions.first().at(1).toJsonObject();
        QCOMPARE(params.value(QStringLiteral("toolCall")).toObject().value(QStringLiteral("rawInput")).toObject().value(QStringLiteral("server")).toString(), QStringLiteral("pacsmith_session"));
        client.answerPermission(permissions.first().at(0).value<QJsonValue>(), QStringLiteral("yes"));
        QTRY_COMPARE(finished.count(), 1);
        client.prompt(QStringLiteral("cancel"));
        QTRY_COMPARE(permissions.count(), 2);
        client.cancel();
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(finished.last().first().toString(), QStringLiteral("cancelled"));
        QVERIFY(!client.isBusy());
        QVERIFY(updates.last().at(0).toJsonObject().value(QStringLiteral("content")).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("cancelled")));
        client.prompt(QStringLiteral("invalid-option"));
        QTRY_COMPARE(permissions.count(), 3);
        const auto updateCount = updates.count();
        client.answerPermission(permissions.last().at(0).value<QJsonValue>(), QStringLiteral("not-offered"));
        QTRY_VERIFY(updates.count() > updateCount);
        QVERIFY(updates.last().at(0).toJsonObject().value(QStringLiteral("content")).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("cancelled")));
        client.cancel();
        QTRY_COMPARE(finished.count(), 3);
    }
    void failedStartsCanRetryAndBadProtocolStops() {
        QTemporaryDir directory;
        AcpClient client;
        QSignalSpy errors(&client, &AcpClient::failed);
        QSignalSpy ready(&client, &AcpClient::ready);
        HarnessProfile missing;
        missing.executable = directory.filePath(QStringLiteral("missing"));
        client.start(missing, environment(directory.path()), {});
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!client.isBusy());
        client.start(profile(fakeAgent(directory.path())), environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 1);
        client.prompt(QStringLiteral("invalid"));
        QTRY_COMPARE(errors.count(), 2);
        QVERIFY(!client.isBusy());
        QVERIFY(!client.isReady());
        client.start(profile(fakeAgent(directory.path())), environment(directory.path()), acpMcpServers({}, QStringLiteral("/test/pacsmith")));
        QTRY_COMPARE(ready.count(), 2);
        client.prompt(QStringLiteral("crash"));
        QTRY_COMPARE(errors.count(), 3);
        QVERIFY(!client.isBusy());
    }
    void conversationKeysPinLibraryAndRelease() {
        ConnectionConfig first;
        first.socketPath = QStringLiteral("/one.sock");
        auto second = first; second.socketPath = QStringLiteral("/two.sock");
        const auto agent = profile(QStringLiteral("agent.py"));
        const auto key = acpConversationKey(first, agent, QStringLiteral("p"), QStringLiteral("r"));
        QVERIFY(key != acpConversationKey(second, agent, QStringLiteral("p"), QStringLiteral("r")));
        QVERIFY(key != acpConversationKey(first, agent, QStringLiteral("p"), QStringLiteral("r2")));
        const auto server = acpMcpServers(first, QStringLiteral("/test/pacsmith")).first().toObject();
        QCOMPARE(server.value(QStringLiteral("args")).toArray(), QJsonArray{QStringLiteral("mcp")});
        QVERIFY(server.value(QStringLiteral("env")).toArray().contains(QJsonObject{{QStringLiteral("name"), QStringLiteral("PACSMITH_MCP_SOCKET")}, {QStringLiteral("value"), first.socketPath}}));
    }
};
QTEST_MAIN(AcpTest)
#include "acp_test.moc"
