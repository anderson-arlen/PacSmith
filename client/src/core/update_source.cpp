#include "core/update_source.hpp"

namespace pacsmith {
namespace {

class ManualUpdateSource final : public UpdateSource {
public:
    [[nodiscard]] UpdateStrategy strategy() const noexcept override { return UpdateStrategy::Manual; }
    [[nodiscard]] UpdateCheckResult check(const PackageRelease &) const override {
        UpdateCheckResult result;
        result.supported = true;
        result.success = true;
        result.message = QStringLiteral("Manual update source; no automatic check performed");
        return result;
    }
};

class DirectUrlUpdateSource final : public UpdateSource {
public:
    [[nodiscard]] UpdateStrategy strategy() const noexcept override { return UpdateStrategy::DirectUrl; }
    [[nodiscard]] UpdateCheckResult check(const PackageRelease &project) const override {
        if (project.update.url.isEmpty()) {
            UpdateCheckResult result;
            result.supported = true;
            result.message = QStringLiteral("Direct URL is not configured");
            return result;
        }
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("Direct URL checks use the asynchronous validator and SHA256 service");
        return result;
    }
};

class AptUpdateSource final : public UpdateSource {
public:
    [[nodiscard]] UpdateStrategy strategy() const noexcept override { return UpdateStrategy::AptRepository; }
    [[nodiscard]] UpdateCheckResult check(const PackageRelease &) const override {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("APT repository checks use the asynchronous APT update service");
        return result;
    }
};

class RpmUpdateSource final : public UpdateSource {
public:
    [[nodiscard]] UpdateStrategy strategy() const noexcept override { return UpdateStrategy::RpmRepository; }
    [[nodiscard]] UpdateCheckResult check(const PackageRelease &) const override {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("RPM repository checks use the asynchronous RPM update service");
        return result;
    }
};

class GitHubUpdateSource final : public UpdateSource {
public:
    [[nodiscard]] UpdateStrategy strategy() const noexcept override { return UpdateStrategy::GitHubRelease; }
    [[nodiscard]] UpdateCheckResult check(const PackageRelease &) const override {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("GitHub release checks use the asynchronous GitHub update service");
        return result;
    }
};

} // namespace

std::unique_ptr<UpdateSource> UpdateSourceFactory::create(const UpdateStrategy strategy) {
    switch (strategy) {
    case UpdateStrategy::Manual: return std::make_unique<ManualUpdateSource>();
    case UpdateStrategy::DirectUrl: return std::make_unique<DirectUrlUpdateSource>();
    case UpdateStrategy::AptRepository: return std::make_unique<AptUpdateSource>();
    case UpdateStrategy::RpmRepository: return std::make_unique<RpmUpdateSource>();
    case UpdateStrategy::GitHubRelease: return std::make_unique<GitHubUpdateSource>();
    }
    return std::make_unique<ManualUpdateSource>();
}

} // namespace pacsmith
