#include "core/acp_conversations.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <utility>
#include <QJsonArray>
#include <QLockFile>
#include <QSet>
#include <algorithm>

namespace pacsmith {
namespace {
QJsonObject readRecord(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
bool writeRecord(const QString &path, const QJsonObject &record, QString *error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
        file.write(bytes) == bytes.size() && file.commit()) return true;
    if (error) *error = file.errorString();
    return false;
}
void imageReferences(const QJsonValue &value, QSet<QString> &references) {
    if (value.isArray()) for (const auto &child : value.toArray()) imageReferences(child, references);
    if (!value.isObject()) return;
    const auto object = value.toObject();
    const auto file = object.value(QStringLiteral("file")).toString();
    if (!file.isEmpty()) references.insert(file);
    for (const auto &child : object) imageReferences(child, references);
}
bool validKey(const QString &key) {
    static const QRegularExpression pattern(QStringLiteral("\\A[a-zA-Z0-9_-]+\\z"));
    return pattern.match(key).hasMatch();
}
}

AcpConversations::AcpConversations(QString directory) : directory_(std::move(directory)) {}

QString AcpConversations::latest(const QString &scope, const QStringList &legacyKeys) const {
    if (!validKey(scope)) return {};
    QFile selection(QDir(directory_).filePath(QStringLiteral("latest/%1.json").arg(scope)));
    if (selection.open(QIODevice::ReadOnly)) {
        const auto key = QJsonDocument::fromJson(selection.readAll()).object().value(QStringLiteral("key")).toString();
        if (validKey(key) && (QFile::exists(QDir(directory_).filePath(key + QStringLiteral(".json"))) ||
            QFile::exists(QDir(directory_).filePath(QStringLiteral("sessions/%1.json").arg(key))))) return key;
    }
    const auto sessions = recent(scope, 1);
    if (!sessions.isEmpty()) return sessions.first().key;
    QString key = scope;
    QDateTime newest;
    for (const auto &candidate : legacyKeys) {
        if (!validKey(candidate)) continue;
        const QFileInfo file(QDir(directory_).filePath(candidate + QStringLiteral(".json")));
        if (file.isFile() && (!newest.isValid() || file.lastModified() > newest)) {
            key = candidate;
            newest = file.lastModified();
        }
    }
    return key;
}

bool AcpConversations::select(const QString &scope, const QString &key, QString *error) const {
    if (!validKey(scope) || !validKey(key)) {
        if (error != nullptr) *error = QStringLiteral("Invalid conversation key.");
        return false;
    }
    const auto metadataPath = QDir(directory_).filePath(QStringLiteral("sessions/%1.json").arg(key));
    auto metadata = readRecord(metadataPath);
    if (metadata.contains(QStringLiteral("scope")) && metadata.value(QStringLiteral("scope")).toString() != scope) {
        if (error) *error = QStringLiteral("This conversation belongs to a different package, agent, or library.");
        return false;
    }
    if (!metadata.contains(QStringLiteral("scope"))) {
        metadata.insert(QStringLiteral("scope"), scope);
        if (!writeRecord(metadataPath, metadata, error)) return false;
    }
    const auto directory = QDir(directory_).filePath(QStringLiteral("latest"));
    if (!QDir().mkpath(directory)) {
        if (error != nullptr) *error = QStringLiteral("Could not create conversation storage.");
        return false;
    }
    QSaveFile file(QDir(directory).filePath(scope + QStringLiteral(".json")));
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("key"), key}}).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

QList<AcpConversation> AcpConversations::recent(const QString &scope, int limit) const {
    QList<AcpConversation> result;
    if (!validKey(scope) || limit <= 0) return result;
    for (const auto &file : QDir(QDir(directory_).filePath(QStringLiteral("sessions"))).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)) {
        const auto record = readRecord(file.filePath());
        if (record.value(QStringLiteral("scope")).toString() != scope) continue;
        const QFileInfo transcript(QDir(directory_).filePath(file.completeBaseName() + QStringLiteral(".json")));
        result.append({file.completeBaseName(), record.value(QStringLiteral("description")).toString(),
                       transcript.exists() ? transcript.lastModified() : file.lastModified()});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.updatedAt == b.updatedAt ? a.key < b.key : a.updatedAt > b.updatedAt;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}
QString AcpConversations::description(const QString &key) const {
    if (!validKey(key)) return {};
    return readRecord(QDir(directory_).filePath(QStringLiteral("sessions/%1.json").arg(key))).value(QStringLiteral("description")).toString();
}
bool AcpConversations::describe(const QString &key, const QString &description, QString *error) const {
    const auto text = description.simplified();
    const auto path = QDir(directory_).filePath(QStringLiteral("sessions/%1.json").arg(key));
    if (!validKey(key) || text.isEmpty() || text.size() > 120 || !QFile::exists(path)) {
        if (error) *error = QStringLiteral("Provide a description of 1–120 characters for the current PacSmith session.");
        return false;
    }
    auto record = readRecord(path);
    record.insert(QStringLiteral("description"), text);
    return writeRecord(path, record, error);
}
void AcpConversations::cleanup(const QDateTime &now) const {
    const QDir directory(directory_);
    const auto cutoff = now.addDays(-10);
    QSet<QString> retainedImages;
    // Open widgets hold a lease so cleanup cannot remove a live conversation or its drafts.
    for (const auto &file : directory.entryInfoList({QStringLiteral("*.json*")}, QDir::Files | QDir::NoSymLinks)) {
        const auto key = file.fileName().section(QStringLiteral(".json"), 0, 0);
        QLockFile lease(directory.filePath(key + QStringLiteral(".lock")));
        lease.setStaleLockTime(0);
        if (file.lastModified() < cutoff && lease.tryLock(0) && QFile::remove(file.filePath())) {
            if (file.fileName() == key + QStringLiteral(".json"))
                QFile::remove(directory.filePath(QStringLiteral("sessions/%1.json").arg(key)));
        } else imageReferences(readRecord(file.filePath()), retainedImages);
    }
    for (const auto &file : QDir(directory.filePath(QStringLiteral("sessions"))).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)) {
        QLockFile lease(directory.filePath(file.completeBaseName() + QStringLiteral(".lock")));
        lease.setStaleLockTime(0);
        if (!QFile::exists(directory.filePath(file.fileName())) && file.lastModified() < cutoff && lease.tryLock(0)) QFile::remove(file.filePath());
    }
    for (const auto &file : QDir(directory.filePath(QStringLiteral("latest"))).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)) {
        const auto key = readRecord(file.filePath()).value(QStringLiteral("key")).toString();
        if (!validKey(key) || (!QFile::exists(directory.filePath(QStringLiteral("sessions/%1.json").arg(key))) &&
            !QFile::exists(directory.filePath(key + QStringLiteral(".json"))))) QFile::remove(file.filePath());
    }
    for (const auto &file : QDir(directory.filePath(QStringLiteral("images"))).entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        if (file.lastModified() < cutoff && !retainedImages.contains(file.fileName())) QFile::remove(file.filePath());
    }
}

QString AcpConversations::freshKey() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

} // namespace pacsmith
