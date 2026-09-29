#include "gui/system_icons.hpp"

#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <QSet>
#include <QFile>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace pacsmith::gui;

class SystemIconTest final : public QObject {
    Q_OBJECT
private:
    QString theme_;
    QString fallback_;
    QStringList paths_;
    QByteArray configHome_;
    QByteArray configDirs_;
    QByteArray dataDirs_;

    void isolate(const QString &root) {
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_CONFIG_DIRS", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_DIRS", root.toUtf8());
        QIcon::setThemeSearchPaths({root});
        QIcon::setThemeName(QStringLiteral("hicolor"));
        QIcon::setFallbackThemeName(QStringLiteral("missing-test-theme"));
    }

    void createTheme(const QString &root, const QString &name) {
        const auto directory = root + QLatin1Char('/') + name;
        QVERIFY(QDir().mkpath(directory + QStringLiteral("/32x32/apps")));
        QFile index(directory + QStringLiteral("/index.theme"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=Test icons\nDirectories=32x32/apps\n\n[32x32/apps]\nSize=32\nContext=Applications\nType=Fixed\n");
        index.close();
        QImage executable(32, 32, QImage::Format_ARGB32);
        executable.fill(Qt::red);
        QVERIFY(executable.save(directory + QStringLiteral("/32x32/apps/application-x-executable.png")));
        executable.fill(Qt::green);
        QVERIFY(executable.save(directory + QStringLiteral("/32x32/apps/utilities-terminal.png")));
    }

private slots:
    void init() {
        theme_ = QIcon::themeName();
        fallback_ = QIcon::fallbackThemeName();
        paths_ = QIcon::themeSearchPaths();
        configHome_ = qgetenv("XDG_CONFIG_HOME");
        configDirs_ = qgetenv("XDG_CONFIG_DIRS");
        dataDirs_ = qgetenv("XDG_DATA_DIRS");
    }

    void cleanup() {
        QIcon::setThemeName(theme_);
        QIcon::setFallbackThemeName(fallback_);
        QIcon::setThemeSearchPaths(paths_);
        for (const auto &[key, value] : QList<QPair<QByteArray, QByteArray>>{
                 {"XDG_CONFIG_HOME", configHome_}, {"XDG_CONFIG_DIRS", configDirs_}, {"XDG_DATA_DIRS", dataDirs_}}) {
            if (value.isNull()) qunsetenv(key.constData());
            else qputenv(key.constData(), value);
        }
    }

    void discoversGtkThemeAndRendersDistinctChoices() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        isolate(directory.path());
        createTheme(directory.path(), QStringLiteral("desktop-icons"));
        QSettings settings(directory.filePath(QStringLiteral("config/gtk-3.0/settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("Settings/gtk-icon-theme-name"), QStringLiteral("desktop-icons"));
        settings.sync();
        const auto choices = standardSystemIcons();
        QCOMPARE(QIcon::themeName(), QStringLiteral("desktop-icons"));
        QCOMPARE(choices.size(), 2);
        QCOMPARE(choices[0].name, QStringLiteral("application-x-executable"));
        QCOMPARE(choices[1].name, QStringLiteral("utilities-terminal"));
        const auto executable = choices[0].icon.pixmap(32, 32).toImage();
        const auto terminal = choices[1].icon.pixmap(32, 32).toImage();
        QVERIFY(!executable.isNull());
        QVERIFY(!terminal.isNull());
        QVERIFY(executable != terminal);
        QCOMPARE(systemIcon(choices[0].name).pixmap(32, 32).toImage(), executable);
    }

    void preservesThemeProvidedByQt() {
        QTemporaryDir directory;
        isolate(directory.path());
        createTheme(directory.path(), QStringLiteral("native-theme"));
        QIcon::setThemeName(QStringLiteral("native-theme"));
        initializeSystemIconTheme();
        QCOMPARE(QIcon::themeName(), QStringLiteral("native-theme"));
    }

    void findsInstalledThemeWithoutDesktopSettings() {
        QTemporaryDir directory;
        isolate(directory.path());
        createTheme(directory.path(), QStringLiteral("breeze"));
        initializeSystemIconTheme();
        QCOMPARE(QIcon::themeName(), QStringLiteral("breeze"));
        QVERIFY(QIcon::hasThemeIcon(QStringLiteral("application-x-executable")));
    }

    void browsesAndSearchesInheritedFontIcons() {
        QTemporaryDir directory;
        isolate(directory.path());
        createTheme(directory.path(), QStringLiteral("desktop-icons"));
        createTheme(directory.path(), QStringLiteral("parent-icons"));
        QSettings child(directory.filePath(QStringLiteral("desktop-icons/index.theme")), QSettings::IniFormat);
        child.setValue(QStringLiteral("Icon Theme/Inherits"), QStringLiteral("parent-icons"));
        child.sync();
        QSettings parent(directory.filePath(QStringLiteral("parent-icons/index.theme")), QSettings::IniFormat);
        parent.setValue(QStringLiteral("Icon Theme/Inherits"), QStringLiteral("desktop-icons"));
        parent.sync();
        QImage font(32, 32, QImage::Format_ARGB32);
        font.fill(Qt::blue);
        QVERIFY(font.save(directory.filePath(QStringLiteral("parent-icons/32x32/apps/preferences-desktop-font.png"))));
        QVERIFY(font.save(directory.filePath(QStringLiteral("parent-icons/32x32/apps/custom-font-tool.png"))));
        QIcon::setThemeName(QStringLiteral("desktop-icons"));
        const auto common = standardSystemIcons();
        bool hasFont = false;
        for (const auto &choice : common) {
            if (choice.label == QStringLiteral("Font")) {
                hasFont = true;
                QVERIFY(!choice.icon.pixmap(32, 32).isNull());
            }
        }
        QVERIFY(hasFont);
        const auto all = availableSystemIcons();
        QSet<QString> names;
        for (const auto &choice : all) names.insert(choice.name);
        QCOMPARE(names.size(), all.size());
        QVERIFY(names.contains(QStringLiteral("custom-font-tool")));
        QVERIFY(names.contains(QStringLiteral("utilities-terminal")));
        bool inspected = false;
        QTimer::singleShot(0, qApp, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *search = dialog->findChild<QLineEdit *>(QStringLiteral("systemIconSearch"));
            auto *list = dialog->findChild<QListWidget *>(QStringLiteral("systemIconList"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(search && list && buttons);
            search->setText(QStringLiteral("FONT"));
            QListWidgetItem *selected = nullptr;
            for (int row = 0; row < list->count(); ++row) {
                auto *item = list->item(row);
                QCOMPARE(!item->isHidden(), item->text().contains(QStringLiteral("font")));
                if (item->text() == QStringLiteral("custom-font-tool")) selected = item;
            }
            QVERIFY(selected != nullptr);
            list->setCurrentItem(selected);
            QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
            search->setText(QStringLiteral("nothing-matches"));
            QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
            search->setText(QStringLiteral("font"));
            list->setCurrentItem(selected);
            inspected = true;
            buttons->button(QDialogButtonBox::Ok)->click();
            close.dismiss();
        });
        QCOMPARE(chooseSystemIcon(nullptr), QStringLiteral("custom-font-tool"));
        QVERIFY(inspected);
    }

    void rendersInstalledIcons() {
        QIcon::setThemeName(QStringLiteral("hicolor"));
        const auto choices = standardSystemIcons();
        if (choices.isEmpty()) QSKIP("No desktop icon theme installed on this test host");
        const auto available = availableSystemIcons();
        QSet<QString> availableNames;
        for (const auto &choice : available) availableNames.insert(choice.name);
        for (const auto &choice : choices) QVERIFY2(availableNames.contains(choice.name), qPrintable(choice.name));
        qInfo() << available.size() << "available system icons";
        if (qEnvironmentVariableIsSet("PACSMITH_TEST_SYSTEM_ICON_BROWSER_SCREENSHOT")) {
            bool captured = false;
            QTimer::singleShot(0, qApp, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog != nullptr);
                auto close = qScopeGuard([dialog] { dialog->reject(); });
                auto *search = dialog->findChild<QLineEdit *>(QStringLiteral("systemIconSearch"));
                auto *list = dialog->findChild<QListWidget *>(QStringLiteral("systemIconList"));
                QVERIFY(search && list);
                search->setText(QStringLiteral("font"));
                list->doItemsLayout();
                QListWidgetItem *firstVisible = nullptr;
                for (int row = 0; row < list->count(); ++row) {
                    if (!list->item(row)->isHidden()) { firstVisible = list->item(row); break; }
                }
                QVERIFY(firstVisible != nullptr);
                QTRY_VERIFY(list->visualItemRect(firstVisible).intersects(list->viewport()->rect()));
                QApplication::processEvents();
                captured = dialog->grab().save(qEnvironmentVariable("PACSMITH_TEST_SYSTEM_ICON_BROWSER_SCREENSHOT"));
            });
            QVERIFY(chooseSystemIcon(nullptr).isEmpty());
            QVERIFY(captured);
        }
        QImage executable, terminal;
        QWidget preview;
        auto *layout = new QGridLayout(&preview);
        for (qsizetype index = 0; index < choices.size(); ++index) {
            const auto &choice = choices[index];
            const auto pixmap = choice.icon.pixmap(64, 64);
            QVERIFY2(!pixmap.isNull(), qPrintable(choice.name));
            if (choice.name == QStringLiteral("application-x-executable")) executable = pixmap.toImage();
            if (choice.name == QStringLiteral("utilities-terminal")) terminal = pixmap.toImage();
            auto *image = new QLabel(&preview);
            image->setPixmap(pixmap);
            image->setAlignment(Qt::AlignCenter);
            const int row = static_cast<int>(index / 4) * 2;
            const int column = static_cast<int>(index % 4);
            layout->addWidget(image, row, column);
            auto *label = new QLabel(choice.label, &preview);
            label->setAlignment(Qt::AlignCenter);
            layout->addWidget(label, row + 1, column);
        }
        if (!executable.isNull() && !terminal.isNull()) QVERIFY(executable != terminal);
        if (qEnvironmentVariableIsSet("PACSMITH_TEST_SYSTEM_ICON_SCREENSHOT")) {
            preview.resize(620, 650);
            preview.show();
            QApplication::processEvents();
            QVERIFY(preview.grab().save(qEnvironmentVariable("PACSMITH_TEST_SYSTEM_ICON_SCREENSHOT")));
        }
    }
};

QTEST_MAIN(SystemIconTest)
#include "system_icon_test.moc"
