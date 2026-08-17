#include "gui/shell_highlighter.hpp"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>

namespace pacsmith::gui {

ShellHighlighter::ShellHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
    const auto dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    QTextCharFormat comment;
    comment.setForeground(QColor(QStringLiteral("#6a9955")));
    QTextCharFormat variable;
    variable.setForeground(QColor(dark ? QStringLiteral("#4fc1ff") : QStringLiteral("#267f99")));
    variable.setFontWeight(QFont::Bold);
    QTextCharFormat keyword;
    keyword.setForeground(QColor(dark ? QStringLiteral("#c586c0") : QStringLiteral("#af00db")));
    keyword.setFontWeight(QFont::Bold);
    QTextCharFormat builtin;
    builtin.setForeground(QColor(dark ? QStringLiteral("#dcdcaa") : QStringLiteral("#795e26")));
    QTextCharFormat string;
    string.setForeground(QColor(dark ? QStringLiteral("#ce9178") : QStringLiteral("#a31515")));
    rules_ = {
        {QRegularExpression(QStringLiteral("#[^\\n]*$")), comment},
        {QRegularExpression(QStringLiteral("\\$\\{?[A-Za-z_][A-Za-z0-9_]*\\}?|\\$[0-9@*#?$!-]|\\b[A-Za-z_][A-Za-z0-9_]*(?==)")),
         variable},
        {QRegularExpression(QStringLiteral(
             "\\b(if|then|elif|else|fi|for|in|do|done|while|until|case|esac|select|function|"
             "return|local|break|continue)\\b")),
         keyword},
        {QRegularExpression(QStringLiteral("\\b(exec|export|unset|shift|trap|wait|readonly|declare|source)\\b")),
         builtin},
        {QRegularExpression(QStringLiteral("'[^']*'|\"(?:\\\\.|[^\"])*\"")), string}};
}

void ShellHighlighter::highlightBlock(const QString &text) {
    for (const auto &rule : rules_) {
        auto matches = rule.expression.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()),
                      rule.format);
        }
    }
}

} // namespace pacsmith::gui
