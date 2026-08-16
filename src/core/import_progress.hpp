#pragma once

#include <QtTypes>

#include <functional>

namespace pacsmith {

enum class ImportStage {
    ValidatingSource,
    ReadingDebContainer,
    ReadingControlArchive,
    ReadingPayloadArchive,
    PreparingProject,
    CopyingSource,
    GeneratingPkgbuild,
    SavingProject
};

struct ImportProgress {
    ImportStage stage{ImportStage::ValidatingSource};
    qsizetype entriesProcessed{0};
};

using ImportProgressCallback = std::function<void(const ImportProgress &)>;

} // namespace pacsmith
