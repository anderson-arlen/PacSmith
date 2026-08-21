#include "gui/pkgbuild_highlighter.hpp"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>

namespace pacsmith::gui {
namespace {

int unquotedCommentStart(const QString &text) {
    QChar quote;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto character = text.at(index);
        if (!quote.isNull()) {
            if (quote == QLatin1Char('"') && character == QLatin1Char('\\') &&
                index + 1 < text.size()) {
                ++index;
                continue;
            }
            if (character == quote) quote = {};
            continue;
        }
        if (character == QLatin1Char('\'') || character == QLatin1Char('"')) quote = character;
        else if (character == QLatin1Char('#')) return static_cast<int>(index);
    }
    return -1;
}

} // namespace

PkgbuildHighlighter::PkgbuildHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
    const auto dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    commentFormat_.setForeground(QColor(QStringLiteral("#6a9955")));
    QTextCharFormat variable;
    variable.setForeground(QColor(dark ? QStringLiteral("#4fc1ff") : QStringLiteral("#267f99")));
    variable.setFontWeight(QFont::Bold);
    QTextCharFormat pacsmith = variable;
    pacsmith.setFontItalic(true);
    QTextCharFormat keyword;
    keyword.setForeground(QColor(dark ? QStringLiteral("#c586c0") : QStringLiteral("#af00db")));
    keyword.setFontWeight(QFont::Bold);
    QTextCharFormat string;
    string.setForeground(QColor(dark ? QStringLiteral("#ce9178") : QStringLiteral("#a31515")));
    QTextCharFormat function;
    function.setForeground(QColor(dark ? QStringLiteral("#dcdcaa") : QStringLiteral("#795e26")));
    rules_ = {
        {QRegularExpression(QStringLiteral("\\$\\{?[A-Za-z_][A-Za-z0-9_]*\\}?|\\b[A-Za-z_][A-Za-z0-9_]*(?==)")), variable},
        {QRegularExpression(QStringLiteral("\\b(pkgname|pkgver|pkgrel|epoch|pkgdesc|arch|url|license|depends|source|noextract|sha256sums)\\b")), variable},
        {QRegularExpression(QStringLiteral("\\b(package|local|while|until|for|in|do|done|case|esac|if|then|elif|else|fi|return|function)\\b")), keyword},
        {QRegularExpression(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\(\\s*\\)\\s*\\{)")), function},
        {QRegularExpression(QStringLiteral("'[^']*'|\"(?:\\\\.|[^\"])*\"")), string},
        {QRegularExpression(QStringLiteral("\\$\\{?_PACSMITH_[A-Z0-9_]+\\}?|\\b_PACSMITH_[A-Z0-9_]+")), pacsmith}};
}

void PkgbuildHighlighter::highlightBlock(const QString &text) {
    const auto comment = unquotedCommentStart(text);
    const auto code = comment < 0 ? text : text.left(comment);
    for (const auto &rule : rules_) {
        auto matches = rule.expression.globalMatch(code);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), rule.format);
        }
    }
    if (comment >= 0) {
        setFormat(comment, static_cast<int>(text.size() - comment), commentFormat_);
    }
}

} // namespace pacsmith::gui
