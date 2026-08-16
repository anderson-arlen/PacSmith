#pragma once

#include "core/import_progress.hpp"

#include <QObject>
#include <QString>

#include <filesystem>

namespace pacsmith::gui {

class ReanalyzeWorker final : public QObject {
    Q_OBJECT
public:
    ReanalyzeWorker(std::filesystem::path projectsRoot, QString projectId,
                    QString releaseId, QObject *parent = nullptr);

public slots:
    void run();

signals:
    void progressChanged(const QString &description);
    void completed(const QString &projectId, const QString &releaseId,
                   const QString &error);

private:
    [[nodiscard]] static QString descriptionFor(const ImportProgress &progress);

    std::filesystem::path projectsRoot_;
    QString projectId_;
    QString releaseId_;
};

} // namespace pacsmith::gui
