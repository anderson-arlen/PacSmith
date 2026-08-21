#include "gui/main_window/help_widgets.hpp"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

namespace pacsmith::gui {

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

void showHelpDetails(QWidget *host, QLabel *source, const QString &details) {
    auto *dialog = new QDialog(host);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("More information"));
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QLabel(dialog);
    text->setWordWrap(true);
    text->setTextFormat(Qt::PlainText);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    text->setMinimumWidth(480);
    text->setMaximumWidth(560);
    text->setText(details);
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog->setMinimumWidth(520);
    dialog->adjustSize();
    if (dialog->width() < 520) dialog->resize(520, dialog->height());

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
    dialog->open();
}

void setSettingsSectionHelp(QLabel *label, const QString &summary, const QString &details) {
    if (label == nullptr) return;
    label->setProperty("helpDetails", details);
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

QLabel *settingsSectionHelp(QWidget *parent, const QString &summary, const QString &details) {
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    auto font = label->font();
    if (font.pointSize() >= 10) font.setPointSize(font.pointSize() - 1);
    label->setFont(font);
    auto palette = label->palette();
    const auto link = palette.color(QPalette::Link);
    const auto muted = palette.color(QPalette::PlaceholderText);
    palette.setColor(QPalette::WindowText, muted.isValid() ? muted : palette.color(QPalette::Mid));
    palette.setColor(QPalette::Link, link);
    palette.setColor(QPalette::LinkVisited, link);
    label->setPalette(palette);
    setSettingsSectionHelp(label, summary, details);
    QObject::connect(label, &QLabel::linkActivated, label, [label](const QString &) {
        const auto more = label->property("helpDetails").toString();
        if (more.isEmpty()) return;
        QPointer<QLabel> source(label);
        QTimer::singleShot(0, label, [source, more] {
            if (source.isNull()) return;
            auto *host = source->window() != nullptr ? source->window() : static_cast<QWidget *>(source.data());
            showHelpDetails(host, source, more);
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
