#include "core/library_events.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace pacsmith {

QList<ServerEvent> SseParser::feed(const QByteArray &chunk) {
    QList<ServerEvent> events;
    pending_.append(chunk);
    qsizetype newline = -1;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        auto line = pending_.left(newline);
        pending_.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        consumeLine(std::move(line), events);
    }
    return events;
}

void SseParser::consumeLine(QByteArray line, QList<ServerEvent> &events) {
    if (line.isEmpty()) {
        dispatch(events);
        return;
    }
    if (line.startsWith(':')) return;
    const auto colon = line.indexOf(':');
    auto field = colon < 0 ? line : line.left(colon);
    auto value = colon < 0 ? QByteArray{} : line.mid(colon + 1);
    if (value.startsWith(' ')) value.remove(0, 1);
    if (field == "event") eventName_ = value;
    else if (field == "id") eventId_ = value;
    else if (field == "data") {
        if (!eventData_.isEmpty()) eventData_.append('\n');
        eventData_.append(value);
    }
}

void SseParser::dispatch(QList<ServerEvent> &events) {
    if (eventData_.isEmpty()) {
        eventName_.clear();
        eventId_.clear();
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(eventData_, &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
        const auto object = document.object();
        ServerEvent event;
        event.name = QString::fromUtf8(eventName_);
        event.sequence = object.value(QStringLiteral("sequence")).toInteger();
        if (event.sequence == 0) event.sequence = eventId_.toLongLong();
        for (const auto topic : object.value(QStringLiteral("topics")).toArray()) {
            event.topics.append(topic.toString());
        }
        event.projectId = object.value(QStringLiteral("project_id")).toString();
        event.projectName = object.value(QStringLiteral("project_name")).toString();
        event.packageName = object.value(QStringLiteral("package_name")).toString();
        event.releaseId = object.value(QStringLiteral("release_id")).toString();
        event.jobId = object.value(QStringLiteral("job_id")).toString();
        event.jobKind = object.value(QStringLiteral("job_kind")).toString();
        event.jobStatus = object.value(QStringLiteral("job_status")).toString();
        events.append(std::move(event));
    }
    eventName_.clear();
    eventId_.clear();
    eventData_.clear();
}

QString jobStatusMessage(const ServerEvent &event) {
    const auto name = !event.projectName.isEmpty() ? event.projectName : event.packageName;
    const auto named = [&](const QString &withName, const QString &withoutName) {
        return name.isEmpty() ? withoutName : withName.arg(name);
    };
    if (event.jobKind == QStringLiteral("build")) {
        if (event.jobStatus == QStringLiteral("queued")) {
            return named(QStringLiteral("Queued package %1 for building…"),
                         QStringLiteral("Package build queued…"));
        }
        if (event.jobStatus == QStringLiteral("running")) {
            return named(QStringLiteral("Building package %1…"), QStringLiteral("Building package…"));
        }
        if (event.jobStatus == QStringLiteral("succeeded")) {
            return named(QStringLiteral("Finished building package %1"),
                         QStringLiteral("Package build finished"));
        }
        if (event.jobStatus == QStringLiteral("failed")) {
            return named(QStringLiteral("Building package %1 failed"),
                         QStringLiteral("Package build failed"));
        }
        if (event.jobStatus == QStringLiteral("interrupted")) {
            return named(QStringLiteral("Building package %1 was canceled"),
                         QStringLiteral("Package build was canceled"));
        }
    }
    if (event.jobKind == QStringLiteral("reanalyze")) {
        if (event.jobStatus == QStringLiteral("queued")) {
            return named(QStringLiteral("Queued package %1 for reanalysis…"),
                         QStringLiteral("Package reanalysis queued…"));
        }
        if (event.jobStatus == QStringLiteral("running")) {
            return named(QStringLiteral("Reanalyzing package %1…"),
                         QStringLiteral("Reanalyzing package…"));
        }
        if (event.jobStatus == QStringLiteral("succeeded")) {
            return named(QStringLiteral("Finished reanalyzing package %1"),
                         QStringLiteral("Package reanalysis finished"));
        }
        if (event.jobStatus == QStringLiteral("failed")) {
            return named(QStringLiteral("Reanalyzing package %1 failed"),
                         QStringLiteral("Package reanalysis failed"));
        }
        if (event.jobStatus == QStringLiteral("interrupted")) {
            return named(QStringLiteral("Reanalyzing package %1 was canceled"),
                         QStringLiteral("Package reanalysis was canceled"));
        }
    }
    if (event.jobKind == QStringLiteral("update_check")) {
        if (event.jobStatus == QStringLiteral("queued")) return QStringLiteral("Update check queued…");
        if (event.jobStatus == QStringLiteral("running")) {
            return named(QStringLiteral("Checking package %1 for updates…"),
                         QStringLiteral("Checking packages for updates…"));
        }
        if (event.jobStatus == QStringLiteral("succeeded")) return QStringLiteral("Update check finished");
        if (event.jobStatus == QStringLiteral("failed")) return QStringLiteral("Update check failed");
        if (event.jobStatus == QStringLiteral("interrupted")) return QStringLiteral("Update check was canceled");
    }
    if (event.jobKind == QStringLiteral("import")) {
        if (event.jobStatus == QStringLiteral("queued")) return QStringLiteral("Package import queued…");
        if (event.jobStatus == QStringLiteral("running")) return QStringLiteral("Importing package…");
        if (event.jobStatus == QStringLiteral("succeeded")) return QStringLiteral("Package import finished");
        if (event.jobStatus == QStringLiteral("failed")) return QStringLiteral("Package import failed");
        if (event.jobStatus == QStringLiteral("interrupted")) return QStringLiteral("Package import was canceled");
    }
    if (event.jobStatus == QStringLiteral("queued")) return QStringLiteral("Package operation queued…");
    if (event.jobStatus == QStringLiteral("running")) return QStringLiteral("Package operation running…");
    if (event.jobStatus == QStringLiteral("succeeded")) return QStringLiteral("Package operation finished");
    if (event.jobStatus == QStringLiteral("failed")) return QStringLiteral("Package operation failed");
    if (event.jobStatus == QStringLiteral("interrupted")) return QStringLiteral("Package operation was canceled");
    return {};
}

LibraryEventStream::LibraryEventStream(ConnectionConfig config, QObject *parent)
    : QObject(parent), config_(std::move(config)) {
    qRegisterMetaType<ServerEvent>();
}

LibraryEventStream::~LibraryEventStream() { stop(); }

void LibraryEventStream::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](const std::stop_token stopToken) { run(stopToken); });
}

void LibraryEventStream::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

void LibraryEventStream::run(const std::stop_token stopToken) {
    using namespace std::chrono_literals;
    auto delay = 250ms;
    constexpr auto maximumDelay = 30000ms;
    std::mutex waitMutex;
    std::condition_variable_any waitCondition;
    while (!stopToken.stop_requested()) {
        SseParser parser;
        bool received = false;
        HttpTransport transport(config_);
        const auto result = transport.stream(
            QStringLiteral("/api/v1/events"), stopToken,
            [&](const QByteArray &chunk) {
                const auto events = parser.feed(chunk);
                for (const auto &event : events) {
                    if (!received) emit connectionChanged(true, {});
                    received = true;
                    emit eventReceived(event);
                }
                return !stopToken.stop_requested();
            });
        if (stopToken.stop_requested()) break;
        emit connectionChanged(false, result.error);
        if (received) delay = 250ms;
        std::unique_lock lock(waitMutex);
        waitCondition.wait_for(lock, stopToken, delay, [] { return false; });
        delay = std::min(delay * 2, maximumDelay);
    }
}

} // namespace pacsmith
