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

} // namespace pacsmith
