#pragma once

#include <QScrollArea>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QTimer>

class QVBoxLayout;

namespace pacsmith::gui {
class ChatTranscript final : public QScrollArea {
    Q_OBJECT
public:
    explicit ChatTranscript(QWidget *parent = nullptr);
    void message(const QString &kind, const QString &text, bool continuation = false, const QJsonArray &images = {});
    void setImageDirectory(const QString &directory) { imageDirectory_ = directory; }
    void restoreLegacyImages(const QJsonArray &images);
    void toolCall(const QJsonObject &update);
    void beginTurn();
    void endTurn(bool succeeded);
    void clear();
    void restore(const QJsonArray &entries);
    void restoreLegacy(const QString &text);
    [[nodiscard]] QJsonArray entries() const { return entries_; }
    [[nodiscard]] QString toPlainText() const;
signals:
    void changed();
protected:
    void changeEvent(QEvent *event) override;
private:
    void renderRow(int index);
    void followOutput(bool follow);
    QJsonArray entries_;
    QString imageDirectory_;
    QHash<QString, int> tools_;
    QWidget *contents_{nullptr};
    QVBoxLayout *rows_{nullptr};
    QTimer animation_;
    int angle_{0};
};
} // namespace pacsmith::gui
