#include "MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QIcon makeAppIconForApplication() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#DCE3EA"));
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);

    p.setBrush(QColor("#30C56C"));
    p.drawRoundedRect(QRectF(10, 36, 18, 10), 3, 3);
    p.drawRoundedRect(QRectF(28, 20, 24, 10), 3, 3);

    p.setPen(QPen(QColor("#20354E"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(10, 41, 18, 41);
    p.drawLine(28, 25, 52, 25);
    p.drawLine(28, 25, 28, 41);
    p.drawLine(28, 41, 42, 41);
    p.drawLine(42, 41, 42, 14);
    p.drawLine(42, 14, 52, 14);

    return QIcon(pm);
}

static bool isUsableIcon(const QIcon& icon) {
    if (icon.isNull()) {
        return false;
    }
    if (!icon.availableSizes().isEmpty()) {
        return true;
    }
    return !icon.pixmap(32, 32).isNull();
}

#ifdef _WIN32
static std::wstring getModulePathNative() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0) {
        return L"";
    }
    buf.resize(n);
    return buf;
}

static std::wstring dirnameNative(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, p);
}

static std::wstring joinNative(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) {
        return name;
    }
    const wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') {
        return dir + name;
    }
    return dir + L"\\" + name;
}

static std::wstring getCurrentDirNative() {
    const DWORD need = GetCurrentDirectoryW(0, nullptr);
    if (need == 0) {
        return L"";
    }

    std::wstring buf(static_cast<size_t>(need) + 2, L'\0');
    const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), &buf[0]);
    if (n == 0) {
        return L"";
    }
    buf.resize(n);
    return buf;
}
#endif

static QIcon loadApplicationIcon() {
    QIcon icon(QStringLiteral(":/app.ico"));
    if (isUsableIcon(icon)) {
        return icon;
    }

#ifdef _WIN32
    const std::wstring exeDir = dirnameNative(getModulePathNative());

    const std::wstring exeIcon = joinNative(exeDir, L"app.ico");
    icon = QIcon(QString::fromWCharArray(exeIcon.c_str()));
    if (isUsableIcon(icon)) {
        return icon;
    }

    const std::wstring cwdIcon = joinNative(getCurrentDirNative(), L"app.ico");
    icon = QIcon(QString::fromWCharArray(cwdIcon.c_str()));
    if (isUsableIcon(icon)) {
        return icon;
    }
#else
    icon = QIcon(QStringLiteral("app.ico"));
    if (isUsableIcon(icon)) {
        return icon;
    }
#endif

    return makeAppIconForApplication();
}

int main(int argc, char *argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

    QApplication a(argc, argv);

    const QIcon appIcon = loadApplicationIcon();
    a.setWindowIcon(appIcon);

    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();

    const QStringList args = a.arguments();
    if (args.size() >= 2) {
        w.openWaveFilePath(args.at(1));
    }

    return a.exec();
}
