#pragma once

#include <QSyntaxHighlighter>

namespace pacsmith::gui {

class DesktopEntryHighlighter final : public QSyntaxHighlighter {
public:
    explicit DesktopEntryHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;
};

} // namespace pacsmith::gui
