#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace pacsmith::gui {

class PkgbuildHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit PkgbuildHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression expression;
        QTextCharFormat format;
    };
    QList<Rule> rules_;
};

} // namespace pacsmith::gui
