#pragma once

#include "core/import_progress.hpp"
#include "core/project_store.hpp"

#include <QObject>
#include <QString>

#include <filesystem>

namespace pacsmith::gui {

class ImportWorker final : public QObject {
    Q_OBJECT
public:
    ImportWorker(std::filesystem::path projectsRoot, QString sourcePath,
                 ImportOptions options = {}, QObject *parent = nullptr);

public slots:
    void run();

signals:
    void progressChanged(const QString &description);
    void completed(const QString &projectId, const QString &releaseId,
                   const QString &error);

private:
    [[nodiscard]] QString descriptionFor(const ImportProgress &progress) const;

    std::filesystem::path projectsRoot_;
    QString sourcePath_;
    ImportOptions options_;
};

} // namespace pacsmith::gui
