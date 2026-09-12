#include "gui/build_output_dialog.hpp"

#include <QFutureWatcher>
#include <QtConcurrent>

namespace pacsmith::gui {
namespace {
struct BuildOutputPoll {
    std::optional<JobStatus> job;
    QString output;
    QString error;
    qint64 offset{0};
};
}

BuildOutputDialog::BuildOutputDialog(ConnectionConfig connection, QString jobId, QWidget *parent)
    : CommandProgressDialog(parent), connection_(std::move(connection)), jobId_(std::move(jobId)) {
    setWindowModality(Qt::NonModal);
    setWindowTitle(QStringLiteral("Build Output"));
    setStatus(QStringLiteral("Loading build output…"));
    setJobTiming({});
    setCancelable(true, QStringLiteral("Cancel build"));
    showDetails();
    connect(this, &CommandProgressDialog::cancelRequested, this, &BuildOutputDialog::cancelBuild);
    connect(&pollTimer_, &QTimer::timeout, this, &BuildOutputDialog::poll);
    pollTimer_.start(250);
    poll();
}

void BuildOutputDialog::poll() {
    if (polling_ || isFinished()) return;
    polling_ = true;
    auto *watcher = new QFutureWatcher<BuildOutputPoll>(this);
    connect(watcher, &QFutureWatcher<BuildOutputPoll>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        polling_ = false;
        if (result.job) setJobTiming(result.job->startedAt, result.job->finishedAt);
        if (!result.error.isEmpty() || !result.job) {
            setStatus(QStringLiteral("Could not load build output; retrying… %1").arg(result.error));
            return;
        }
        appendOutput(result.output);
        offset_ = result.offset;
        const auto &job = *result.job;
        const bool success = job.status == QStringLiteral("succeeded");
        const bool canceled = job.status == QStringLiteral("interrupted");
        if (success || canceled || job.status == QStringLiteral("failed")) {
            pollTimer_.stop();
            markFinished(success, canceled ? QStringLiteral("Build canceled.")
                                  : success ? QStringLiteral("Build succeeded.")
                                            : QStringLiteral("Build failed. %1").arg(job.error));
        } else {
            setStatus(cancelPending_ ? QStringLiteral("Canceling build…")
                      : job.status == QStringLiteral("queued") ? QStringLiteral("Build queued…")
                      : job.message.isEmpty() ? QStringLiteral("Building…") : job.message);
        }
    });
    watcher->setFuture(QtConcurrent::run([connection = connection_, jobId = jobId_, offset = offset_] {
        LibraryClient client(connection);
        BuildOutputPoll result;
        result.offset = offset;
        result.job = client.getJob(jobId, &result.error);
        // Every new viewer starts at zero; subsequent reads continue at the daemon's byte offset.
        if (result.job) result.output = client.jobLog(jobId, offset, &result.offset, &result.error);
        return result;
    }));
}

void BuildOutputDialog::cancelBuild() {
    if (cancelPending_ || isFinished()) return;
    cancelPending_ = true;
    setCancelable(false);
    setStatus(QStringLiteral("Canceling build…"));
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty() && !isFinished()) {
            cancelPending_ = false;
            setCancelable(true, QStringLiteral("Cancel build"));
            setStatus(QStringLiteral("Could not cancel build: %1").arg(error));
        }
    });
    watcher->setFuture(QtConcurrent::run([connection = connection_, jobId = jobId_] {
        QString error;
        if (!LibraryClient(connection).cancelJob(jobId, &error) && error.isEmpty()) {
            error = QStringLiteral("The daemon did not accept the cancellation request.");
        }
        return error;
    }));
}

} // namespace pacsmith::gui
