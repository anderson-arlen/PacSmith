#pragma once

#include "core/http_transport.hpp"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QStringList>

#include <thread>

namespace pacsmith {

struct ServerEvent {
    qint64 sequence{0};
    QString name;
    QStringList topics;
    QString projectId;
    QString projectName;
    QString packageName;
    QString releaseId;
    QString jobId;
    QString jobKind;
    QString jobStatus;
    QString jobMessage;
    int jobCurrent{0};
    int jobTotal{0};
    int jobFailedItems{0};
    int jobPausedItems{0};
};

[[nodiscard]] QString jobStatusMessage(const ServerEvent &event);

class SseParser final {
public:
    [[nodiscard]] QList<ServerEvent> feed(const QByteArray &chunk);

private:
    void consumeLine(QByteArray line, QList<ServerEvent> &events);
    void dispatch(QList<ServerEvent> &events);

    QByteArray pending_;
    QByteArray eventName_;
    QByteArray eventId_;
    QByteArray eventData_;
};

class LibraryEventStream final : public QObject {
    Q_OBJECT
public:
    explicit LibraryEventStream(ConnectionConfig config, QObject *parent = nullptr);
    ~LibraryEventStream() override;

    void start();
    void stop();

signals:
    void eventReceived(const pacsmith::ServerEvent &event);
    void connectionChanged(bool connected, const QString &error);

private:
    void run(std::stop_token stopToken);

    ConnectionConfig config_;
    std::jthread worker_;
};

} // namespace pacsmith

Q_DECLARE_METATYPE(pacsmith::ServerEvent)
