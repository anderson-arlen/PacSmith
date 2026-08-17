#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace pacsmith::gui {

class ShellHighlighter final : public QSyntaxHighlighter {
public:
    explicit ShellHighlighter(QTextDocument *document);

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
