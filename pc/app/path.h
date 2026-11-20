#pragma once

#include <QString>
#include <QFileInfo>

class Path
{
public:
    static QString getLogDir();
    static QString getBoxArtCacheDir();
    static QString getQmlCacheDir();

    static QByteArray readDataFile(QString fileName);
    static void writeCacheFile(QString fileName, QByteArray data);
    static void deleteCacheFile(QString fileName);
    static QFileInfo getCacheFileInfo(QString fileName);

    // Only safe to use directly for Qt classes
    static QString getDataFilePath(QString fileName);

    // Private working folder of the updater (downloads and the result of the last update)
    static QString getUpdateDir();

    // The folder of a portable installation, or an empty string for a standard installation.
    // On Windows the marker is looked up next to the executable, not in the current directory,
    // because shortcuts, jump list tasks and the updater start NyxPlay with another working directory.
    static QString findPortableDir(const QString& exeDir, const QString& currentDir);

    // portableDir is only used when portable is true; it defaults to the current directory.
    static void initialize(bool portable, const QString& portableDir = QString());

private:
    static QString s_CacheDir;
    static QString s_LogDir;
    static QString s_BoxArtCacheDir;
    static QString s_QmlCacheDir;
};
