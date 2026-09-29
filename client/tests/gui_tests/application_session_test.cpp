#include "../server_ai_host.hpp"
#include "gui/application_session.hpp"
#include "gui/main_window/main_window.hpp"
#include <QDialog>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

namespace pacsmith::gui {
class ApplicationSessionTest final : public QObject {
    Q_OBJECT
  private slots:
    void sharedApprovalsDoNotOpenWorkbench() {
        QTemporaryDir directory;
        QFile script(directory.filePath(QStringLiteral("agent.py")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(import json,sys
pending=None
for line in sys.stdin:
 m=json.loads(line);method=m.get('method')
 if method=='initialize': result=dict(protocolVersion=1,agentCapabilities={})
 elif method=='session/new': result=dict(sessionId='review')
 elif method=='session/prompt':
  pending=m['id']
  print(json.dumps(dict(jsonrpc='2.0',id='permission',method='session/request_permission',params=dict(sessionId='review',toolCall=dict(title='Build package'),options=[dict(optionId='yes',kind='allow_once',name='Allow')]))),flush=True)
  continue
 elif m.get('id')=='permission':
  m['id']=pending;result=dict(stopReason='end_turn')
 else: continue
 print(json.dumps(dict(jsonrpc='2.0',id=m['id'],result=result)),flush=True)
)PY");
        script.close();
        HarnessProfile profile;
        profile.name = QStringLiteral("Test");
        profile.executable = QStringLiteral("/usr/bin/python3");
        profile.arguments = {script.fileName()};
        tests::ServerAiHost host(profile);
        ServerAi api(host.connection);
        const auto conversation = api.request(QStringLiteral("POST"), QStringLiteral("/conversations"));
        QVERIFY(conversation);
        const auto id = conversation->value(QStringLiteral("id")).toString();
        const QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"), QStringLiteral("Review")}}};
        QVERIFY(api.request(QStringLiteral("POST"), QStringLiteral("/conversations/%1/prompt").arg(id),
                            {{QStringLiteral("content"), content}}));
        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        ApplicationSession first(store), second(store);
        QTRY_VERIFY(!api.request(QStringLiteral("GET"), QStringLiteral("/permissions"))
                         ->value(QStringLiteral("permissions"))
                         .toArray()
                         .isEmpty());
        first.refreshPermissions();
        second.refreshPermissions();
        QTRY_COMPARE(first.permissionDialogs_.size(), 1);
        QTRY_COMPARE(second.permissionDialogs_.size(), 1);
        QVERIFY(!first.window_);
        QVERIFY(!second.window_);
        auto *button = second.permissionDialogs_.constBegin().value()->findChild<QPushButton *>();
        QVERIFY(button);
        button->click();
        QTRY_VERIFY(api.request(QStringLiteral("GET"), QStringLiteral("/permissions"))
                        ->value(QStringLiteral("permissions"))
                        .toArray()
                        .isEmpty());
        first.refreshPermissions();
        second.refreshPermissions();
        QTRY_VERIFY(first.permissionDialogs_.isEmpty());
        QTRY_VERIFY(second.permissionDialogs_.isEmpty());
        QVERIFY(!first.window_);
        QVERIFY(!second.window_);
    }
};
} // namespace pacsmith::gui
QTEST_MAIN(pacsmith::gui::ApplicationSessionTest)
#include "application_session_test.moc"
