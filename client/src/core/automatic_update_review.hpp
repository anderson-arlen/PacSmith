#pragma once

#include "core/library_client.hpp"

namespace pacsmith {

struct AutomaticReviewRequest {
    ConnectionConfig connection;
    HarnessProfile profile;
    QString projectId;
    QString releaseId;
    QString title;
    QString prompt;
};

QList<AutomaticReviewRequest> claimPendingUpdateReviews(const LibraryClient &client,
    const QList<Project> &summaries, const AppSettings &settings,
    const QString &reviewDirectory = {}, bool recoverInterrupted = false);

} // namespace pacsmith
