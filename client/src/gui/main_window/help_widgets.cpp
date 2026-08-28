#include "gui/main_window/help_widgets.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {

QLabel *helpBodyLabel(QWidget *parent, const QString &text) {
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setMinimumWidth(480);
    label->setMaximumWidth(560);
    label->setText(text);
    return label;
}

class CopyableCommandLabel final : public QLabel {
public:
    CopyableCommandLabel(const QString &command, QWidget *parent)
        : QLabel(parent), command_(command) {
        setWordWrap(true);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setCursor(Qt::PointingHandCursor);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setMinimumWidth(480);
        setMaximumWidth(560);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setText(QStringLiteral("$ %1").arg(command));
        setToolTip(QStringLiteral("Click to copy"));
        setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(127, 127, 127, 40); border-radius: 4px; "
            "padding: 8px; }"));
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            QApplication::clipboard()->setText(command_);
            QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("Copied"), this);
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    QString command_;
};

void guardHelpSource(QDialog *dialog, QLabel *source) {
    QPointer<QLabel> guard(source);
    const auto flags = source != nullptr ? source->textInteractionFlags() : Qt::TextInteractionFlags{};
    if (source != nullptr) {
        source->clearFocus();
        source->setTextInteractionFlags(Qt::NoTextInteraction);
    }
    QObject::connect(dialog, &QDialog::finished, qApp, [guard, flags] {
        QTimer::singleShot(0, qApp, [guard, flags] {
            if (guard != nullptr) guard->setTextInteractionFlags(flags);
        });
    });
}

void finishHelpDialog(QDialog *dialog, QWidget *host) {
    dialog->setMinimumWidth(520);
    dialog->adjustSize();
    if (dialog->width() < 520) dialog->resize(520, dialog->height());
    if (host != nullptr && host->screen() != nullptr) {
        const auto cap = host->screen()->availableGeometry().height() * 3 / 4;
        if (dialog->height() > cap) dialog->resize(dialog->width(), cap);
    }
    dialog->open();
}

} // namespace

QLabel *pageIntroduction(const QString &summary, QWidget *parent, const QString &details) {
    return settingsSectionHelp(parent, summary, details);
}
QFrame *settingsStatusFrame(QWidget *parent) {
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("settingsStatusPanel"));
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setStyleSheet(QStringLiteral(
        "QFrame#settingsStatusPanel { background-color: rgba(52, 152, 219, 28); "
        "border: 1px solid rgba(52, 152, 219, 150); border-radius: 6px; } "
        "QFrame#settingsStatusPanel QLabel { background: transparent; border: none; }"));
    return frame;
}

void showHelpDetails(QWidget *host, QLabel *source, const QString &details, const QString &commands) {
    auto *dialog = new QDialog(host);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("More information"));
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(dialog);

    QString before = details;
    QString after;
    if (!commands.isEmpty()) {
        const auto index = details.indexOf(commands);
        if (index >= 0) {
            before = details.left(index);
            after = details.mid(index + commands.size());
        }
    }

    auto addCommands = [&](QWidget *parent, QVBoxLayout *parentLayout) {
        const auto lines = commands.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        bool hinted = false;
        for (const auto &line : lines) {
            const auto command = line.trimmed();
            if (command.isEmpty()) continue;
            if (!hinted) {
                auto *hint = new QLabel(QStringLiteral("Click a command to copy it."), parent);
                hint->setWordWrap(true);
                auto font = hint->font();
                if (font.pointSize() >= 10) font.setPointSize(font.pointSize() - 1);
                hint->setFont(font);
                parentLayout->addWidget(hint);
                hinted = true;
            }
            if (command.startsWith(QLatin1Char('#'))) {
                auto *comment = new QLabel(command, parent);
                comment->setWordWrap(true);
                comment->setTextInteractionFlags(Qt::TextSelectableByMouse);
                parentLayout->addWidget(comment);
                continue;
            }
            parentLayout->addWidget(new CopyableCommandLabel(command, parent));
        }
    };

    if (commands.trimmed().isEmpty()) {
        layout->addWidget(helpBodyLabel(dialog, details));
    } else {
        auto *scroll = new QScrollArea(dialog);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        if (!before.trimmed().isEmpty()) contentLayout->addWidget(helpBodyLabel(content, before.trimmed()));
        addCommands(content, contentLayout);
        if (!after.trimmed().isEmpty()) contentLayout->addWidget(helpBodyLabel(content, after.trimmed()));
        contentLayout->addStretch(1);
        scroll->setWidget(content);
        layout->addWidget(scroll);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(buttons);

    guardHelpSource(dialog, source);
    finishHelpDialog(dialog, host);
}

void setSettingsSectionHelp(QLabel *label, const QString &summary, const QString &details,
                            const QString &commands) {
    if (label == nullptr) return;
    label->setProperty("helpDetails", details);
    label->setProperty("helpCommands", commands);
    if (details.isEmpty()) {
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setText(summary);
        return;
    }
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(false);
    auto text = summary.toHtmlEscaped();
    if (!text.isEmpty()) text += QLatin1Char(' ');
    text += QStringLiteral("<a href=\"learn-more\">Learn more</a>");
    label->setText(text);
}

QLabel *settingsSectionHelp(QWidget *parent, const QString &summary, const QString &details,
                            const QString &commands) {
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    auto font = label->font();
    if (font.pointSize() >= 10) font.setPointSize(font.pointSize() - 1);
    label->setFont(font);
    label->setForegroundRole(QPalette::PlaceholderText);
    setSettingsSectionHelp(label, summary, details, commands);
    QObject::connect(label, &QLabel::linkActivated, label, [label](const QString &) {
        const auto more = label->property("helpDetails").toString();
        if (more.isEmpty()) return;
        const auto commandText = label->property("helpCommands").toString();
        QPointer<QLabel> source(label);
        QTimer::singleShot(0, label, [source, more, commandText] {
            if (source.isNull()) return;
            auto *host = source->window() != nullptr ? source->window() : static_cast<QWidget *>(source.data());
            showHelpDetails(host, source, more, commandText);
        });
    });
    return label;
}
void applySourcePackageTypeHelp(QLabel *label, const SourcePackageType type) {
    switch (type) {
    case SourcePackageType::Debian:
        setSettingsSectionHelp(
            label, QStringLiteral("Official Debian/Ubuntu binary package."),
            QStringLiteral("PacSmith inspects control metadata, maintainer scripts, and the data archive without executing anything."));
        return;
    case SourcePackageType::Rpm:
        setSettingsSectionHelp(
            label, QStringLiteral("Official RPM binary package."),
            QStringLiteral("PacSmith reads the header and cpio payload statically and never runs RPM scriptlets."));
        return;
    case SourcePackageType::ArchPackage:
        setSettingsSectionHelp(
            label, QStringLiteral("Existing Arch package."),
            QStringLiteral("PacSmith reads .PKGINFO and the payload, then rebuilds a new package with PacSmith identity metadata."));
        return;
    case SourcePackageType::Archive:
        setSettingsSectionHelp(
            label, QStringLiteral("tar, zip, or 7z application bundle."),
            QStringLiteral("PacSmith maps the inspected layout into an Arch package, usually under /opt."));
        return;
    case SourcePackageType::AppImage:
        setSettingsSectionHelp(
            label, QStringLiteral("Type 2 AppImage."),
            QStringLiteral("PacSmith decomposes the AppDir with unsquashfs; it does not run the image, FUSE-mount it, or keep the embedded updater."));
        return;
    case SourcePackageType::ElfBinary:
        setSettingsSectionHelp(
            label, QStringLiteral("Standalone Linux executable."),
            QStringLiteral("Identified from its ELF header. PacSmith installs it to an explicit /usr/bin path and never runs it during analysis."));
        return;
    case SourcePackageType::Unknown:
        setSettingsSectionHelp(
            label,
            QStringLiteral("This release has been recorded but its artifact has not been inspected yet."),
            {});
        return;
    }
    setSettingsSectionHelp(label, {}, {});
}

} // namespace pacsmith::gui
