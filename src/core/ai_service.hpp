#pragma once

#include "core/app_settings.hpp"
#include "core/chatgpt_auth.hpp"
#include "core/model.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>

class QNetworkReply;

namespace pacsmith {

struct AiInformationRequest {
    QString id;
    QString kind;
    QString argument;
    QString reason;
};

struct AiFieldChange {
    QString field;
    QString value;
    QString rationale;
};

struct AiFindingResolution {
    QString evidenceFingerprint;
    ScriptDisposition disposition{ScriptDisposition::Unresolved};
    QString summary;
    QString rationale;
};

struct AiResolution {
    bool success{false};
    QString error;
    QString errorDetails;
    QString provider;
    QString model;
    QList<AiInformationRequest> informationRequests;
    QList<AiFieldChange> changes;
    QList<AiFindingResolution> findingResolutions;
    QString lifecycleScript;
    QString rationale;
};

[[nodiscard]] QJsonObject aiRequestOptions(const AiSettings &settings);
[[nodiscard]] QJsonValue aiRequestInput(AiProviderKind provider, const QString &prompt);
[[nodiscard]] QJsonObject aiResponseSchema(const PackageRelease &release,
                                           bool allowFindingResolutions = true);

class AiAnalysisService final : public QObject {
    Q_OBJECT
public:
    explicit AiAnalysisService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    void start(const PackageRelease &release, const AiSettings &settings, const QString &credential = {});
    void startGitHubAssetRule(const PackageRelease &release, const QStringList &availableAssets,
                              const QString &preferredAsset, const AiSettings &settings,
                              const QString &credential = {});
    void cancel();

signals:
    void progressChanged(const QString &message);
    void activityChanged(const QString &message);
    void responseProgress(qint64 bytesReceived, qint64 outputCharacters);
    void requestAvailable(int round, const QByteArray &requestBody);
    void responseDelta(int round, const QString &text);
    void credentialUpdated(const QString &serializedCredentials);
    void finished(const pacsmith::AiResolution &resolution);

private:
    enum class TaskMode { PackageResolution, GitHubAssetRule };
    void startHttpRequest();
    void refreshChatGptCredentials(const ChatGptCredentials &credentials);
    void sendAiRequest(const QString &bearer, const QString &accountId = {});
    void processHttpReply(QNetworkReply *reply);
    void processStreamChunk(const QByteArray &chunk);
    void parseAndFinish(const QByteArray &json);
    [[nodiscard]] QString prompt() const;
    QNetworkAccessManager network_;
    QNetworkReply *reply_{nullptr};
    bool running_{false};
    PackageRelease project_;
    TaskMode taskMode_{TaskMode::PackageResolution};
    QStringList githubAssets_;
    QString preferredGithubAsset_;
    AiSettings settings_;
    QString credential_;
    QByteArray requestBody_;
    QByteArray responseBody_;
    QByteArray streamBuffer_;
    QString streamedText_;
    int requestRound_{0};
    bool receivedResponseContent_{false};
    bool receivedReasoningActivity_{false};
    bool terminalResponseScheduled_{false};
    qsizetype consecutiveWhitespaceCharacters_{0};
    bool whitespaceSuppressionNotified_{false};
    QString streamError_;
    QString limitError_;
    QTimer deadline_;
};

struct AiApplyResult {
    QStringList manualConflicts;
    QStringList errors;
    bool changed{false};
};

class AiResolutionApplier final {
public:
    [[nodiscard]] static QStringList manualConflicts(const PackageRelease &project,
                                                     const AiResolution &resolution);
    [[nodiscard]] static QStringList explicitApprovalRequired(
        const PackageRelease &project, const AiResolution &resolution);
    [[nodiscard]] static AiApplyResult apply(PackageRelease &project, const AiResolution &resolution,
                                             const QSet<QString> &approvedUserFields = {});
};

class SystemInformationBroker final {
public:
    [[nodiscard]] static QJsonObject execute(const AiInformationRequest &request);
    [[nodiscard]] static QStringList repositoryPackageNames(QString *error = nullptr);
};

} // namespace pacsmith

Q_DECLARE_METATYPE(pacsmith::AiResolution)
Q_DECLARE_METATYPE(pacsmith::AiInformationRequest)
