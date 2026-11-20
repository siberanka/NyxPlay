#include "updatestage.h"

#include "updatelogic.h"
#include "../../updater/applyupdate.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>
#include <private/qzipreader_p.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace UpdateStage {

QString workDir(const QString& installDir)
{
    return QDir(installDir).filePath(QStringLiteral(".nyxupdate"));
}

QString stageDir(const QString& installDir)
{
    return QDir(workDir(installDir)).filePath(QStringLiteral("stage"));
}

static bool entryAllowed(const QZipReader::FileInfo& info, QString* name)
{
    QString path = info.filePath;
    if (info.isDir && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (info.isSymLink || path.isEmpty() ||
            !NyxUpdater::isValidRelativePath(path.toUtf8().toStdString())) {
        return false;
    }
    *name = path;
    return true;
}

qint64 unpackedSize(const QByteArray& data, QString* error)
{
    QBuffer device;
    device.setData(data);
    device.open(QIODevice::ReadOnly);
    QZipReader zip(&device);
    if (!zip.isReadable()) {
        *error = QStringLiteral("unreadable");
        return -1;
    }
    const QList<QZipReader::FileInfo> entries = zip.fileInfoList();
    if (entries.isEmpty() || entries.size() > MaxEntries) {
        *error = QStringLiteral("entry count");
        return -1;
    }
    qint64 total = 0;
    QSet<QString> seen;
    for (const QZipReader::FileInfo& info : entries) {
        QString name;
        if (!entryAllowed(info, &name)) {
            *error = QStringLiteral("entry name");
            return -1;
        }
        if (seen.contains(name.toLower())) {
            *error = QStringLiteral("duplicate entry");
            return -1;
        }
        seen.insert(name.toLower());
        if (info.isFile) {
            if (info.size < 0 || info.size > MaxUnpackedBytes) {
                *error = QStringLiteral("entry size");
                return -1;
            }
            total += info.size;
            if (total > MaxUnpackedBytes) {
                *error = QStringLiteral("archive size");
                return -1;
            }
        }
    }
    return total;
}

bool extractPackage(const QByteArray& data, const QString& destDir, QString* error)
{
    if (unpackedSize(data, error) < 0) {
        return false;
    }

    QDir dest(destDir);
    if (!dest.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("cannot create stage");
        return false;
    }
    if (!dest.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        *error = QStringLiteral("stage not empty");
        return false;
    }

    QBuffer device;
    device.setData(data);
    device.open(QIODevice::ReadOnly);
    QZipReader zip(&device);
    const QList<QZipReader::FileInfo> entries = zip.fileInfoList();
    for (const QZipReader::FileInfo& info : entries) {
        QString name;
        if (!entryAllowed(info, &name)) {
            *error = QStringLiteral("entry name");
            return false;
        }
        if (NyxUpdater::isProtectedPath(name.toUtf8().toStdString()) ||
                NyxUpdater::isProtectedPath((name + QLatin1Char('/')).toUtf8().toStdString())) {
            continue;
        }
        const QString target = dest.filePath(name);
        if (info.isDir) {
            if (!dest.mkpath(name)) {
                *error = QStringLiteral("cannot create folder");
                return false;
            }
            continue;
        }
        if (!info.isFile) {
            *error = QStringLiteral("entry type");
            return false;
        }

        const QByteArray data = zip.fileData(info.filePath);
        if (data.size() != info.size) {
            *error = QStringLiteral("corrupt entry");
            return false;
        }
        if (!dest.mkpath(QFileInfo(name).path())) {
            *error = QStringLiteral("cannot create folder");
            return false;
        }
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                out.write(data) != data.size() || !out.flush()) {
            *error = QStringLiteral("cannot write");
            return false;
        }
        out.close();
    }
    return true;
}

QString executableVersion(const QString& exePath)
{
#ifdef Q_OS_WIN
    const std::wstring path = QDir::toNativeSeparators(exePath).toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0 || size > 1024 * 1024) {
        return QString();
    }
    QByteArray block(int(size), '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) {
        return QString();
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.constData(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
            !info || length < sizeof(VS_FIXEDFILEINFO)) {
        return QString();
    }
    return QStringLiteral("%1.%2.%3.%4")
            .arg(HIWORD(info->dwProductVersionMS)).arg(LOWORD(info->dwProductVersionMS))
            .arg(HIWORD(info->dwProductVersionLS)).arg(LOWORD(info->dwProductVersionLS));
#else
    Q_UNUSED(exePath);
    return QString();
#endif
}

quint16 executableMachine(const QString& exePath)
{
    QFile file(exePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray head = file.read(4096);
    if (head.size() < 0x40 || head[0] != 'M' || head[1] != 'Z') {
        return 0;
    }
    const quint32 pe = quint8(head[0x3C]) | quint8(head[0x3D]) << 8 | quint8(head[0x3E]) << 16 | quint32(quint8(head[0x3F])) << 24;
    if (pe + 6 > quint32(head.size()) || head.mid(int(pe), 4) != QByteArray("PE\0\0", 4)) {
        return 0;
    }
    return quint16(quint8(head[int(pe) + 4]) | quint8(head[int(pe) + 5]) << 8);
}

bool checkStage(const QString& stage, const QString& expectedVersion, quint16 expectedMachine, QString* error)
{
    std::vector<std::string> files;
    std::string why;
    if (!NyxUpdater::validateStage(stage.toStdWString(), &files, &why)) {
        *error = QString::fromStdString(why);
        return false;
    }

    // What the client needs to start and to apply the next update
    QSet<QString> listed;
    for (const std::string& f : files) {
        listed.insert(QString::fromStdString(f).toLower());
    }
    for (const char* required : { "nyxplay.exe", "nyxplayupdater.exe", "qt6core.dll", "platforms/qwindows.dll" }) {
        if (!listed.contains(QLatin1String(required))) {
            *error = QStringLiteral("missing %1").arg(QLatin1String(required));
            return false;
        }
    }

    const QString exe = QDir(stage).filePath(QStringLiteral("NyxPlay.exe"));
    QVector<int> expected;
    QVector<int> actual;
    const QString version = executableVersion(exe);
    if (!UpdateLogic::parseVersion(expectedVersion, expected) || !UpdateLogic::parseVersion(version, actual) ||
            actual.size() != 4 || UpdateLogic::compareVersions(expected, actual) != 0) {
        *error = QStringLiteral("version %1").arg(version.isEmpty() ? QStringLiteral("unknown") : version);
        return false;
    }
    if (expectedMachine != 0 && executableMachine(exe) != expectedMachine) {
        *error = QStringLiteral("architecture");
        return false;
    }
    if (expectedMachine != 0 && executableMachine(QDir(stage).filePath(QStringLiteral("NyxPlayUpdater.exe"))) != expectedMachine) {
        *error = QStringLiteral("architecture");
        return false;
    }
    return true;
}

bool isWritable(const QString& dir)
{
    const QString name = QStringLiteral(".nyxwrite-") + QUuid::createUuid().toString(QUuid::Id128);
    const QString probe = QDir(dir).filePath(name);
    const QString renamed = probe + QStringLiteral(".r");
    {
        QFile f(probe);
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write("x", 1) != 1) {
            QFile::remove(probe);
            return false;
        }
    }
    const bool ok = QFile::rename(probe, renamed) && QFile::remove(renamed);
    QFile::remove(probe);
    QFile::remove(renamed);
    return ok;
}

}
