#include "gui/desktop_entry_highlighter.hpp"

#include <QColor>
#include <QRegularExpression>
#include <QTextCharFormat>

#include <algorithm>
#include <limits>

namespace pacsmith::gui {

DesktopEntryHighlighter::DesktopEntryHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {}

void DesktopEntryHighlighter::highlightBlock(const QString &text) {
    const auto toFormatLength = [](const qsizetype value) {
        return static_cast<int>(std::min(value, static_cast<qsizetype>(std::numeric_limits<int>::max())));
    };
    QTextCharFormat comment;
    comment.setForeground(QColor(QStringLiteral("#7f8c8d")));
    if (text.trimmed().startsWith(QLatin1Char('#'))) {
        setFormat(0, toFormatLength(text.size()), comment);
        return;
    }
    QTextCharFormat section;
    section.setForeground(QColor(QStringLiteral("#56b6c2")));
    section.setFontWeight(QFont::Bold);
    static const QRegularExpression sectionPattern(QStringLiteral(R"(^\s*\[[^\]]+\])"));
    const auto sectionMatch = sectionPattern.match(text);
    if (sectionMatch.hasMatch()) {
        setFormat(toFormatLength(sectionMatch.capturedStart()), toFormatLength(sectionMatch.capturedLength()), section);
    }
    QTextCharFormat key;
    key.setForeground(QColor(QStringLiteral("#61afef")));
    static const QRegularExpression keyPattern(QStringLiteral(R"(^\s*([A-Za-z][A-Za-z0-9-]*(?:\[[^\]]+\])?)\s*=)"));
    const auto keyMatch = keyPattern.match(text);
    if (keyMatch.hasMatch()) {
        setFormat(toFormatLength(keyMatch.capturedStart(1)), toFormatLength(keyMatch.capturedLength(1)), key);
        QTextCharFormat value;
        value.setForeground(QColor(QStringLiteral("#98c379")));
        const auto valueStart = keyMatch.capturedEnd();
        setFormat(toFormatLength(valueStart), toFormatLength(text.size() - valueStart), value);
    }
}

} // namespace pacsmith::gui
