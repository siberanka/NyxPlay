#include <QtTest>

#include <QBuffer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <private/qzipwriter_p.h>

#include <openssl/evp.h>

#include <qt_windows.h>

#include "backend/updatecommand.h"
#include "backend/updateinstaller.h"
#include "backend/updatelogic.h"
#include "backend/updatestage.h"
#include "path.h"
#include "../updater/applyupdate.h"

namespace fs = std::filesystem;

static QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

static QString systemExe()
{
    return QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", "C:/Windows")) + "/System32/notepad.exe";
}

// ---- manifest -------------------------------------------------------------------------------

class UpdateManifestTest : public QObject
{
    Q_OBJECT

    static QByteArray manifest(const QString& extra, const QString& version = "2026.12.1.1")
    {
        return QString("[{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"%1\","
                       "\"browser_url\":\"https://github.com/siberanka/NyxPlay/releases/tag/v%1\"%2}]")
                .arg(version, extra.isEmpty() ? QString() : "," + extra).toUtf8();
    }

    static QString goodFields(const QString& version = "2026.12.1.1")
    {
        return QString("\"tag\":\"v%1\",\"asset_url\":\"https://github.com/siberanka/NyxPlay/releases/download/v%1/"
                       "NyxPlayPC-%1-windows-x64-portable.zip\",\"asset_size\":38016381").arg(version);
    }

    static UpdateLogic::Result eval(const QByteArray& body, const QString& current = "2026.11.19.1")
    {
        return UpdateLogic::evaluateManifest(body, current, "x86_64", "windows", "10.0.26100");
    }

private slots:
    void parsesInstallFields()
    {
        const UpdateLogic::Result r = eval(manifest(goodFields()));
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QVERIFY(r.package.valid());
        QCOMPARE(r.package.tag, QString("v2026.12.1.1"));
        QCOMPARE(r.package.assetName, QString("NyxPlayPC-2026.12.1.1-windows-x64-portable.zip"));
        QCOMPARE(r.package.assetSize, qint64(38016381));
        QCOMPARE(UpdateLogic::releaseFileUrl(r.package, "SHA256SUMS.txt").toString(),
                 QString("https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/SHA256SUMS.txt"));
        QCOMPARE(UpdateLogic::releaseFileUrl(r.package, "SHA256SUMS.txt.sig").toString(),
                 QString("https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/SHA256SUMS.txt.sig"));
    }

    void oldManifestStillWorks()
    {
        // The format of the previous release: no install fields, the notice and release page keep working
        const UpdateLogic::Result r = eval(manifest(QString()));
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QVERIFY(!r.package.valid());
        QCOMPARE(r.url, QString("https://github.com/siberanka/NyxPlay/releases/tag/v2026.12.1.1"));
    }

    void gitlabMirrorAsset()
    {
        const QString fields = "\"tag\":\"v2026.12.1.1\",\"asset_url\":\"https://gitlab.com/siberanka/NyxPlay/-/releases/"
                               "v2026.12.1.1/downloads/NyxPlayPC-2026.12.1.1-windows-x64-portable.zip\",\"asset_size\":1000";
        QVERIFY(eval(manifest(fields)).package.valid());
    }

    void rejectsBadInstallFields_data()
    {
        QTest::addColumn<QString>("fields");
        const QString name = "NyxPlayPC-2026.12.1.1-windows-x64-portable.zip";
        const QString good = "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/" + name;
        auto f = [](const QString& tag, const QString& url, const QString& size) {
            return QString("\"tag\":\"%1\",\"asset_url\":\"%2\",\"asset_size\":%3").arg(tag, url, size);
        };
        QTest::newRow("tag mismatch") << f("v2026.12.1.2", good, "1000");
        QTest::newRow("tag without v") << f("2026.12.1.1", good, "1000");
        QTest::newRow("other repository") << f("v2026.12.1.1", "https://github.com/evil/NyxPlay/releases/download/v2026.12.1.1/" + name, "1000");
        QTest::newRow("other host") << f("v2026.12.1.1", "https://example.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/" + name, "1000");
        QTest::newRow("http") << f("v2026.12.1.1", "http://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/" + name, "1000");
        QTest::newRow("other asset name") << f("v2026.12.1.1", "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/other.zip", "1000");
        QTest::newRow("older version name") << f("v2026.12.1.1", "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/NyxPlayPC-2026.11.19.1-windows-x64-portable.zip", "1000");
        QTest::newRow("other tag path") << f("v2026.12.1.1", "https://github.com/siberanka/NyxPlay/releases/download/v2026.11.19.1/" + name, "1000");
        QTest::newRow("query") << f("v2026.12.1.1", good + "?token=1", "1000");
        QTest::newRow("dot dot") << f("v2026.12.1.1", "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/../" + name, "1000");
        QTest::newRow("userinfo") << f("v2026.12.1.1", "https://a@github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/" + name, "1000");
        QTest::newRow("size zero") << f("v2026.12.1.1", good, "0");
        QTest::newRow("size negative") << f("v2026.12.1.1", good, "-5");
        QTest::newRow("size too large") << f("v2026.12.1.1", good, "400000000");
        QTest::newRow("size fractional") << f("v2026.12.1.1", good, "10.5");
        QTest::newRow("size string") << QString("\"tag\":\"v2026.12.1.1\",\"asset_url\":\"%1\",\"asset_size\":\"1000\"").arg(good);
    }

    void rejectsBadInstallFields()
    {
        QFETCH(QString, fields);
        const UpdateLogic::Result r = eval(manifest(fields));
        // The update itself is still announced; only the install is disabled
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QVERIFY(!r.package.valid());
    }

    void noPackageForOtherPlatforms()
    {
        const QByteArray body = QString("[{\"platform\":\"linux\",\"arch\":\"x86_64\",\"version\":\"2026.12.1.1\","
                                        "\"browser_url\":\"https://github.com/siberanka/NyxPlay/releases\",%1}]")
                .arg(goodFields()).toUtf8();
        const UpdateLogic::Result r = UpdateLogic::evaluateManifest(body, "2026.11.19.1", "x86_64", "linux", "6.5.0");
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QVERIFY(!r.package.valid());
    }

    void noDowngradeOrSameVersion()
    {
        QCOMPARE(eval(manifest(goodFields("2026.11.19.1"), "2026.11.19.1"), "2026.11.19.1").status, UpdateLogic::Status::UpToDate);
        QCOMPARE(eval(manifest(goodFields("2026.11.19.1"), "2026.11.19.1"), "2026.12.1.1").status, UpdateLogic::Status::UpToDate);
        QVERIFY(!eval(manifest(goodFields("2026.11.19.1"), "2026.11.19.1"), "2026.12.1.1").package.valid());
    }

    void repositoryManifestIsValid()
    {
        const QByteArray body = readFile(QStringLiteral(UPDATE_MANIFEST_FILE));
        QVERIFY(!body.isEmpty());
        // Seen from the previous release, the checked-in manifest offers the install package
        const UpdateLogic::Result r = UpdateLogic::evaluateManifest(body, "2026.11.18.1", "x86_64", "windows", "10.0.26100");
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QVERIFY(r.package.valid());
        QCOMPARE(r.package.assetName, UpdateLogic::packageAssetName(r.version, "x86_64"));
        QVERIFY(r.package.assetSize > 1000000);
    }
};

// ---- updater: replace and roll back -----------------------------------------------------------

class UpdaterApplyTest : public QObject
{
    Q_OBJECT

    QTemporaryDir root;
    QString install;
    QString stage;

    void put(const QString& dir, const QString& rel, const QByteArray& data)
    {
        QVERIFY(writeFile(dir + "/" + rel, data));
    }

    void list(const QString& dir, const QStringList& rels)
    {
        put(dir, "package-files.txt", (rels.join("\n") + "\n").toUtf8());
    }

    NyxUpdater::Options options()
    {
        NyxUpdater::Options o;
        o.installDir = install.toStdWString();
        o.stageDir = stage.toStdWString();
        o.renameRetries = 1;
        o.retryDelayMs = 1;
        return o;
    }

    // Version "old" installed, version "new" staged
    void makeTrees()
    {
        install = root.filePath("install");
        stage = root.filePath("install/.nyxupdate/stage");
        QDir(install).removeRecursively();
        put(install, "NyxPlay.exe", "old-exe");
        put(install, "Qt6Core.dll", "old-core");
        put(install, "obsolete.dll", "old-obsolete");
        put(install, "qml/a/plugin.dll", "old-plugin");
        put(install, "qml/a/gone.qml", "old-gone");
        list(install, { "NyxPlay.exe", "Qt6Core.dll", "obsolete.dll", "qml/a/plugin.dll", "qml/a/gone.qml" });
        // User data and a file the user added
        put(install, "portable.dat", "");
        put(install, "NyxPlay/NyxPlay.ini", "settings");
        put(install, "cache/gamecontrollerdb.txt", "cached");
        put(install, "boxart/1.png", "art");
        put(install, "NyxPlay-123.log", "log");
        put(install, "notes.txt", "mine");

        put(stage, "NyxPlay.exe", "new-exe");
        put(stage, "Qt6Core.dll", "new-core");
        put(stage, "qml/a/plugin.dll", "new-plugin");
        put(stage, "qml/b/added.dll", "new-added");
        put(stage, "NyxPlayUpdater.exe", "new-updater");
        put(stage, "portable.dat", "");
        list(stage, { "NyxPlay.exe", "NyxPlayUpdater.exe", "Qt6Core.dll", "qml/a/plugin.dll", "qml/b/added.dll" });
    }

    QByteArray at(const QString& rel) { return readFile(install + "/" + rel); }

    QStringList snapshot()
    {
        QStringList files;
        QDirIterator it(install, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            if (p.contains("/.nyxupdate/")) {
                continue;
            }
            files << p.mid(install.size() + 1) + "=" + QString::fromLatin1(readFile(p));
        }
        files.sort();
        return files;
    }

private slots:
    void replacesFilesAndKeepsUserData()
    {
        makeTrees();
        const NyxUpdater::Outcome o = NyxUpdater::apply(options());
        QVERIFY2(o.ok, o.code.c_str());

        QCOMPARE(at("NyxPlay.exe"), QByteArray("new-exe"));
        QCOMPARE(at("Qt6Core.dll"), QByteArray("new-core"));
        QCOMPARE(at("qml/a/plugin.dll"), QByteArray("new-plugin"));
        QCOMPARE(at("qml/b/added.dll"), QByteArray("new-added"));
        QCOMPARE(at("NyxPlayUpdater.exe"), QByteArray("new-updater"));
        QVERIFY(at("package-files.txt").contains("qml/b/added.dll"));

        // Known program files of the old version that are not in the new one are gone
        QVERIFY(!QFile::exists(install + "/obsolete.dll"));
        QVERIFY(!QFile::exists(install + "/qml/a/gone.qml"));

        // User data and unknown files are untouched
        QCOMPARE(at("NyxPlay/NyxPlay.ini"), QByteArray("settings"));
        QCOMPARE(at("cache/gamecontrollerdb.txt"), QByteArray("cached"));
        QCOMPARE(at("boxart/1.png"), QByteArray("art"));
        QCOMPARE(at("NyxPlay-123.log"), QByteArray("log"));
        QCOMPARE(at("notes.txt"), QByteArray("mine"));
        QVERIFY(QFile::exists(install + "/portable.dat"));

        // Replaced files wait as .nyxold for the client's next start
        QCOMPARE(at("NyxPlay.exe.nyxold"), QByteArray("old-exe"));
        QCOMPARE(at("obsolete.dll.nyxold"), QByteArray("old-obsolete"));
    }

    void stageNeverTouchesUserData()
    {
        makeTrees();
        // A malicious or careless stage with user data paths is ignored, and the marker is not created
        QFile::remove(install + "/portable.dat");
        put(stage, "NyxPlay/NyxPlay.ini", "evil");
        put(stage, "cache/x.txt", "evil");
        put(stage, "NyxPlay-9.log", "evil");
        QVERIFY(NyxUpdater::apply(options()).ok);
        QCOMPARE(at("NyxPlay/NyxPlay.ini"), QByteArray("settings"));
        QVERIFY(!QFile::exists(install + "/cache/x.txt"));
        QVERIFY(!QFile::exists(install + "/NyxPlay-9.log"));
        QVERIFY(!QFile::exists(install + "/portable.dat"));
    }

    void rollsBackWhenAFileIsLocked()
    {
        makeTrees();
        const QStringList before = snapshot();

        // "qml/b/added.dll" is handled after NyxPlay.exe and Qt6Core.dll have been replaced; hold the old plugin open
        const QString locked = install + "/qml/a/plugin.dll";
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(locked).utf16()), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);

        const NyxUpdater::Outcome o = NyxUpdater::apply(options());
        CloseHandle(h);

        QVERIFY(!o.ok);
        QCOMPARE(QString::fromStdString(o.code), QString("locked"));
        QCOMPARE(QString::fromStdString(o.detail), QString("qml/a/plugin.dll"));
        // Everything is as before: contents, no new files, no backups, no new folders
        QCOMPARE(snapshot(), before);
        QVERIFY(!QFile::exists(install + "/qml/b"));
        QCOMPARE(at("NyxPlay.exe"), QByteArray("old-exe"));
    }

    void rollsBackWhenStageFileVanishes()
    {
        makeTrees();
        const QStringList before = snapshot();
        // Listed but not present: caught before anything is touched
        QFile::remove(stage + "/qml/b/added.dll");
        const NyxUpdater::Outcome o = NyxUpdater::apply(options());
        QVERIFY(!o.ok);
        QCOMPARE(QString::fromStdString(o.code), QString("stage-invalid"));
        QCOMPARE(snapshot(), before);
    }

    void rejectsInconsistentStages_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("unlisted file") << "unlisted";
        QTest::newRow("traversal in list") << "traversal";
        QTest::newRow("absolute path in list") << "absolute";
        QTest::newRow("drive in list") << "drive";
        QTest::newRow("protected path in list") << "protected";
        QTest::newRow("no list") << "nolist";
        QTest::newRow("no exe") << "noexe";
        QTest::newRow("duplicate") << "duplicate";
    }

    void rejectsInconsistentStages()
    {
        QFETCH(QString, what);
        makeTrees();
        const QStringList before = snapshot();
        if (what == "unlisted") {
            put(stage, "extra.dll", "x");
        }
        else if (what == "traversal") {
            list(stage, { "NyxPlay.exe", "../evil.dll" });
        }
        else if (what == "absolute") {
            list(stage, { "NyxPlay.exe", "/evil.dll" });
        }
        else if (what == "drive") {
            list(stage, { "NyxPlay.exe", "C:/evil.dll" });
        }
        else if (what == "protected") {
            list(stage, { "NyxPlay.exe", "NyxPlay/NyxPlay.ini" });
        }
        else if (what == "nolist") {
            QFile::remove(stage + "/package-files.txt");
        }
        else if (what == "noexe") {
            list(stage, { "Qt6Core.dll" });
            QFile::remove(stage + "/NyxPlay.exe");
            QFile::remove(stage + "/NyxPlayUpdater.exe");
            QFile::remove(stage + "/qml/a/plugin.dll");
            QFile::remove(stage + "/qml/b/added.dll");
        }
        else if (what == "duplicate") {
            list(stage, { "NyxPlay.exe", "nyxplay.exe" });
        }
        const NyxUpdater::Outcome o = NyxUpdater::apply(options());
        QVERIFY(!o.ok);
        QCOMPARE(QString::fromStdString(o.code), QString("stage-invalid"));
        QCOMPARE(snapshot(), before);
    }

    void refusesAnInstallFolderWithoutTheClient()
    {
        makeTrees();
        QFile::remove(install + "/NyxPlay.exe");
        QVERIFY(!NyxUpdater::apply(options()).ok);
    }

    void relativePathRules_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("valid");
        QTest::newRow("plain") << "NyxPlay.exe" << true;
        QTest::newRow("nested") << "qml/QtQuick/Controls/Material/plugin.dll" << true;
        QTest::newRow("unicode") << QString::fromUtf8("dil/t\xC3\xBCrk\xC3\xA7" "e.qm") << true;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("leading slash") << "/a" << false;
        QTest::newRow("trailing slash") << "a/" << false;
        QTest::newRow("dot") << "a/./b" << false;
        QTest::newRow("dotdot") << "a/../b" << false;
        QTest::newRow("dotdot start") << "../a" << false;
        QTest::newRow("empty segment") << "a//b" << false;
        QTest::newRow("backslash") << "a\\b" << false;
        QTest::newRow("drive") << "C:/a" << false;
        QTest::newRow("stream") << "a.txt:evil" << false;
        QTest::newRow("trailing dot") << "a." << false;
        QTest::newRow("trailing space") << "a " << false;
        QTest::newRow("wildcard") << "a*.dll" << false;
        QTest::newRow("control") << QString("a\x01") << false;
        QTest::newRow("too long") << QString(300, 'a') << false;
    }

    void relativePathRules()
    {
        QFETCH(QString, path);
        QFETCH(bool, valid);
        QCOMPARE(NyxUpdater::isValidRelativePath(path.toStdString()), valid);
    }

    void protectedPathRules_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("isProtected");
        QTest::newRow("marker") << "portable.dat" << true;
        QTest::newRow("marker upper") << "Portable.dat" << true;
        QTest::newRow("settings") << "NyxPlay/NyxPlay.ini" << true;
        QTest::newRow("cache") << "cache/x" << true;
        QTest::newRow("boxart") << "boxart/1.png" << true;
        QTest::newRow("qmlcache") << "qmlcache/a" << true;
        QTest::newRow("work") << ".nyxupdate/stage/a" << true;
        QTest::newRow("log") << "NyxPlay-1700000000.log" << true;
        QTest::newRow("dump") << "NyxPlay-1700000000.dmp" << true;
        QTest::newRow("exe") << "NyxPlay.exe" << false;
        QTest::newRow("updater") << "NyxPlayUpdater.exe" << false;
        QTest::newRow("nested log") << "qml/NyxPlay-1.log" << false;
        QTest::newRow("dll") << "Qt6Core.dll" << false;
    }

    void protectedPathRules()
    {
        QFETCH(QString, path);
        QFETCH(bool, isProtected);
        QCOMPARE(NyxUpdater::isProtectedPath(path.toStdString()), isProtected);
    }

    void waitsForProcesses()
    {
        // A process id that does not exist counts as exited
        QVERIFY(NyxUpdater::waitForProcessExit(0x7FFFFFF0u, 50));
        // This process is running: the wait times out
        QVERIFY(!NyxUpdater::waitForProcessExit(GetCurrentProcessId(), 50));
    }
};

// ---- staging --------------------------------------------------------------------------------

class UpdateStageTest : public QObject
{
    Q_OBJECT

    QTemporaryDir root;

    static QByteArray makeZip(const QList<QPair<QString, QByteArray>>& files, const QStringList& dirs = {},
                              const QList<QPair<QString, QString>>& links = {})
    {
        QByteArray out;
        QBuffer buffer(&out);
        buffer.open(QIODevice::WriteOnly);
        QZipWriter zip(&buffer);
        for (const QString& d : dirs) {
            zip.addDirectory(d);
        }
        for (const auto& f : files) {
            zip.addFile(f.first, f.second);
        }
        for (const auto& l : links) {
            zip.addSymLink(l.first, l.second);
        }
        zip.close();
        return out;
    }

    static bool extract(const QByteArray& zip, const QString& dest, QString* error)
    {
        return UpdateStage::extractPackage(zip, dest, error);
    }

    // A complete stage whose NyxPlay.exe carries the version of a system executable
    QString completeStage(const QString& name, QString* version)
    {
        const QString dir = root.filePath(name);
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        *version = UpdateStage::executableVersion(systemExe());
        QStringList files = { "NyxPlay.exe", "NyxPlayUpdater.exe", "Qt6Core.dll", "platforms/qwindows.dll" };
        for (const QString& f : files) {
            QDir().mkpath(QFileInfo(dir + "/" + f).path());
            if (f.endsWith(".exe")) {
                QFile::copy(systemExe(), dir + "/" + f);
            }
            else {
                writeFile(dir + "/" + f, "dll");
            }
        }
        writeFile(dir + "/package-files.txt", (files.join("\n") + "\n").toUtf8());
        return dir;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(root.isValid());
    }

    void extractsAPackage()
    {
        const QByteArray zip = makeZip({ { "NyxPlay.exe", "exe" }, { "qml/a/b.dll", "dll" }, { "portable.dat", "" },
                                         { "package-files.txt", "NyxPlay.exe\nqml/a/b.dll\n" } }, { "boxart", "qml" });
        const QString dest = root.filePath("ok");
        QString error;
        QVERIFY2(extract(zip, dest, &error), qPrintable(error));
        QCOMPARE(readFile(dest + "/NyxPlay.exe"), QByteArray("exe"));
        QCOMPARE(readFile(dest + "/qml/a/b.dll"), QByteArray("dll"));
        // The portable marker and box art folder are user data and are not part of a stage
        QVERIFY(!QFile::exists(dest + "/portable.dat"));
        QVERIFY(!QFile::exists(dest + "/boxart"));
    }

    void rejectsUnsafeArchives_data()
    {
        QTest::addColumn<QByteArray>("zip");
        QTest::newRow("parent folder") << makeZip({ { "../evil.dll", "x" } });
        QTest::newRow("nested parent") << makeZip({ { "a/../../evil.dll", "x" } });
        QTest::newRow("absolute") << makeZip({ { "/evil.dll", "x" } });
        QTest::newRow("drive") << makeZip({ { "C:/evil.dll", "x" } });
        QTest::newRow("stream") << makeZip({ { "a.dll:stream", "x" } });
        QTest::newRow("duplicate") << makeZip({ { "A.dll", "x" }, { "a.dll", "y" } });
        QTest::newRow("symlink") << makeZip({ { "ok.dll", "x" } }, {}, { { "link.dll", "C:/Windows/notepad.exe" } });
        QTest::newRow("not a zip") << QByteArray("this is not a zip file at all");
        QTest::newRow("empty") << QByteArray();
    }

    void rejectsUnsafeArchives()
    {
        QFETCH(QByteArray, zip);
        const QString dest = root.filePath("unsafe");
        QDir(dest).removeRecursively();
        QString error;
        QVERIFY(!extract(zip, dest, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(root.filePath("evil.dll")));
        QVERIFY(!QFile::exists(QDir::tempPath() + "/evil.dll"));
    }

    void refusesNonEmptyDestination()
    {
        const QString dest = root.filePath("busy");
        writeFile(dest + "/old.txt", "old");
        QString error;
        QVERIFY(!extract(makeZip({ { "NyxPlay.exe", "x" } }), dest, &error));
        QCOMPARE(readFile(dest + "/old.txt"), QByteArray("old"));
    }

    void readsExecutableMetadata()
    {
        const QString version = UpdateStage::executableVersion(systemExe());
        QCOMPARE(version.split('.').size(), 4);
        QCOMPARE(UpdateStage::executableMachine(systemExe()), quint16(0x8664));
        QVERIFY(UpdateStage::executableVersion(root.filePath("missing.exe")).isEmpty());
        writeFile(root.filePath("text.exe"), "plain text");
        QVERIFY(UpdateStage::executableVersion(root.filePath("text.exe")).isEmpty());
        QCOMPARE(UpdateStage::executableMachine(root.filePath("text.exe")), quint16(0));
    }

    void stageIntegrity()
    {
        QString version;
        const QString dir = completeStage("integrity", &version);
        QString error;
        QVERIFY2(UpdateStage::checkStage(dir, version, 0x8664, &error), qPrintable(error));

        // Another version than the one that was offered
        QVERIFY(!UpdateStage::checkStage(dir, "2026.12.1.1", 0x8664, &error));
        QVERIFY(error.startsWith("version"));
        // Another architecture
        QVERIFY(!UpdateStage::checkStage(dir, version, 0xAA64, &error));
        QCOMPARE(error, QString("architecture"));
    }

    void stageIntegrityDetectsDamage()
    {
        QString version;
        QString dir = completeStage("damage1", &version);
        QString error;
        QFile::remove(dir + "/Qt6Core.dll");
        QVERIFY(!UpdateStage::checkStage(dir, version, 0x8664, &error));

        dir = completeStage("damage2", &version);
        writeFile(dir + "/unlisted.dll", "x");
        QVERIFY(!UpdateStage::checkStage(dir, version, 0x8664, &error));

        dir = completeStage("damage3", &version);
        writeFile(dir + "/package-files.txt", "NyxPlay.exe\nNyxPlayUpdater.exe\n");
        QVERIFY(!UpdateStage::checkStage(dir, version, 0x8664, &error));   // required files missing from the list

        dir = completeStage("damage4", &version);
        writeFile(dir + "/NyxPlay.exe", "not an executable");
        QVERIFY(!UpdateStage::checkStage(dir, version, 0x8664, &error));
    }

    void detectsWritableFolders()
    {
        QVERIFY(UpdateStage::isWritable(root.path()));
        QVERIFY(!UpdateStage::isWritable(root.filePath("does/not/exist")));
        // No probe files are left behind
        QVERIFY(QDir(root.path()).entryList({ ".nyxwrite-*" }, QDir::Files | QDir::Hidden).isEmpty());
    }

    void detectsReadOnlyFolders()
    {
        const QString dir = root.filePath("readonly");
        QVERIFY(QDir().mkpath(dir));
        const QString native = QDir::toNativeSeparators(dir);
        QProcess deny;
        deny.start("icacls.exe", { native, "/deny", "*S-1-1-0:(WD,AD)" });
        QVERIFY(deny.waitForFinished(10000));
        QCOMPARE(deny.exitCode(), 0);

        const bool writable = UpdateStage::isWritable(dir);

        QProcess allow;
        allow.start("icacls.exe", { native, "/remove:d", "*S-1-1-0" });
        QVERIFY(allow.waitForFinished(10000));
        QVERIFY(!writable);
        QVERIFY(UpdateStage::isWritable(dir));
    }
};

// ---- commands -------------------------------------------------------------------------------

class UpdateCommandTest : public QObject
{
    Q_OBJECT

    using C = UpdateCommand::Command;

private slots:
    void extractsExactOptions()
    {
        QStringList args = { "NyxPlay.exe", "--check-update" };
        QCOMPARE(UpdateCommand::extract(args), C::CheckUpdate);
        QCOMPARE(args, QStringList({ "NyxPlay.exe" }));

        args = { "NyxPlay.exe", "--install-update" };
        QCOMPARE(UpdateCommand::extract(args), C::InstallUpdate);
        QCOMPARE(args, QStringList({ "NyxPlay.exe" }));

        args = { "NyxPlay.exe" };
        QCOMPARE(UpdateCommand::extract(args), C::None);
    }

    void leavesEverythingElseAlone_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("both") << QStringList({ "NyxPlay.exe", "--check-update", "--install-update" });
        QTest::newRow("twice") << QStringList({ "NyxPlay.exe", "--check-update", "--check-update" });
        QTest::newRow("with stream") << QStringList({ "NyxPlay.exe", "stream", "host", "app", "--check-update" });
        QTest::newRow("before stream") << QStringList({ "NyxPlay.exe", "--install-update", "stream", "host", "app" });
        QTest::newRow("upper case") << QStringList({ "NyxPlay.exe", "--CHECK-UPDATE" });
        QTest::newRow("single dash") << QStringList({ "NyxPlay.exe", "-check-update" });
        QTest::newRow("value") << QStringList({ "NyxPlay.exe", "--check-update=1" });
        QTest::newRow("prefix") << QStringList({ "NyxPlay.exe", "--check-updates" });
        QTest::newRow("padded") << QStringList({ "NyxPlay.exe", " --check-update" });
        QTest::newRow("shell") << QStringList({ "NyxPlay.exe", "--check-update & calc.exe" });
        QTest::newRow("path value") << QStringList({ "NyxPlay.exe", "--install-update", "C:/evil.exe" });
        QTest::newRow("empty") << QStringList();
    }

    void leavesEverythingElseAlone()
    {
        QFETCH(QStringList, args);
        const QStringList before = args;
        QCOMPARE(UpdateCommand::extract(args), C::None);
        QCOMPARE(args, before);
    }

    void wireFormat()
    {
        QCOMPARE(UpdateCommand::fromWire(UpdateCommand::toWire(C::CheckUpdate)), C::CheckUpdate);
        QCOMPARE(UpdateCommand::fromWire(UpdateCommand::toWire(C::InstallUpdate)), C::InstallUpdate);
        QVERIFY(UpdateCommand::toWire(C::None).isEmpty());
        for (const QByteArray& bad : { QByteArray(), QByteArray("\n"), QByteArray("check-update"), QByteArray("check-update\r\n"),
                                       QByteArray("Check-Update\n"), QByteArray("install-update\nextra"), QByteArray("install-update \n"),
                                       QByteArray("calc.exe\n"), QByteArray("check-update\0\n", 14), QByteArray(1000, 'x') }) {
            QCOMPARE(UpdateCommand::fromWire(bad), C::None);
        }
    }

    void serverNamesAreScoped()
    {
        const QString sid1 = "S-1-5-21-1-2-3-1001";
        const QString sid2 = "S-1-5-21-1-2-3-1002";
        const QString a = UpdateCommand::serverName(sid1, "C:/Users/a/NyxPlay");
        QCOMPARE(a, UpdateCommand::serverName(sid1, "C:/Users/a/NyxPlay"));
        QCOMPARE(a, UpdateCommand::serverName(sid1, "c:\\users\\a\\nyxplay\\"));   // same folder, spelled differently
        QVERIFY(a != UpdateCommand::serverName(sid2, "C:/Users/a/NyxPlay"));
        QVERIFY(a != UpdateCommand::serverName(sid1, "C:/Users/a/NyxPlay2"));
        QVERIFY(!a.contains("1001"));
        QVERIFY(!a.contains("Users"));
        QVERIFY(UpdateCommand::currentUserSid().startsWith("S-1-"));
    }

    void serverAcceptsOnlyTheTwoCommands()
    {
        const QString name = UpdateCommand::serverName("S-1-5-test", "C:/test/" + QUuid::createUuid().toString());
        UpdateCommand::Server server;
        QVERIFY(server.listen(name));
        qRegisterMetaType<UpdateCommand::Command>();
        QSignalSpy spy(&server, &UpdateCommand::Server::commandReceived);

        // The sender blocks, so it runs on its own thread while this thread serves the socket
        bool sent = false;
        QThread* thread = QThread::create([&]() { sent = UpdateCommand::sendToRunningInstance(name, C::InstallUpdate); });
        thread->start();
        QTRY_VERIFY_WITH_TIMEOUT(thread->isFinished(), 10000);
        delete thread;
        QVERIFY(sent);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<UpdateCommand::Command>(), C::InstallUpdate);

        // Garbage is dropped without a signal and without an answer
        QThread* junk = QThread::create([&]() {
            QLocalSocket s;
            s.connectToServer(name);
            if (s.waitForConnected(2000)) {
                s.write("calc.exe\n");
                s.waitForBytesWritten(2000);
                s.waitForReadyRead(500);
            }
        });
        junk->start();
        QTRY_VERIFY_WITH_TIMEOUT(junk->isFinished(), 10000);
        delete junk;
        QCOMPARE(spy.count(), 1);
    }

    void noServerMeansNoDelivery()
    {
        QVERIFY(!UpdateCommand::sendToRunningInstance("NyxPlay-test-nobody-" + QUuid::createUuid().toString(), C::CheckUpdate, 200));
    }
};

// ---- portable detection ---------------------------------------------------------------------

class PortableDetectTest : public QObject
{
    Q_OBJECT

private slots:
    void usesTheExecutableDirectory()
    {
        QTemporaryDir exeDir;
        QTemporaryDir cwd;
        QVERIFY(exeDir.isValid() && cwd.isValid());

        // Double-click: working directory is the executable directory
        QVERIFY(writeFile(exeDir.filePath("portable.dat"), ""));
        QCOMPARE(Path::findPortableDir(exeDir.path(), exeDir.path()), QDir::cleanPath(exeDir.path()));

        // Jump list task, shortcut or updater: another working directory
        QCOMPARE(Path::findPortableDir(exeDir.path(), cwd.path()), QDir::cleanPath(exeDir.path()));

        // A marker in some unrelated working directory does not make a standard installation portable
        QVERIFY(QFile::remove(exeDir.filePath("portable.dat")));
        QVERIFY(writeFile(cwd.filePath("portable.dat"), ""));
        QVERIFY(Path::findPortableDir(exeDir.path(), cwd.path()).isEmpty());

        QVERIFY(Path::findPortableDir(QString(), cwd.path()).isEmpty());
        QVERIFY(Path::findPortableDir(exeDir.filePath("nope"), cwd.path()).isEmpty());
    }
};

// ---- installer state machine ----------------------------------------------------------------

class FakeReply : public QNetworkReply
{
    Q_OBJECT
public:
    FakeReply(const QUrl& url, int status, const QByteArray& body, bool hold, QObject* parent) :
        QNetworkReply(parent), m_Body(body)
    {
        setUrl(url);
        setOperation(QNetworkAccessManager::GetOperation);
        setOpenMode(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (status != 200) {
            setError(status == 404 ? QNetworkReply::ContentNotFoundError : QNetworkReply::UnknownServerError, "fake");
        }
        if (!hold) {
            QTimer::singleShot(0, this, &FakeReply::deliver);
        }
    }

    void abort() override
    {
        if (isFinished()) {
            return;
        }
        setError(QNetworkReply::OperationCanceledError, "aborted");
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override { return (m_Body.size() - m_Pos) + QIODevice::bytesAvailable(); }
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_Body.size() - m_Pos);
        memcpy(data, m_Body.constData() + m_Pos, size_t(n));
        m_Pos += n;
        return n;
    }

private slots:
    void deliver()
    {
        if (isFinished()) {
            return;
        }
        emit downloadProgress(m_Body.size(), m_Body.size());
        if (!m_Body.isEmpty()) {
            emit readyRead();
        }
        setFinished(true);
        emit finished();
    }

private:
    QByteArray m_Body;
    qint64 m_Pos = 0;
};

class FakeNam : public QNetworkAccessManager
{
public:
    struct Route { int status; QByteArray body; bool hold; };
    QHash<QString, Route> routes;   // URL without scheme and host -> response
    QStringList requested;          // full URLs, in order
    QStringList userAgents;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requested << request.url().toString();
        userAgents << request.header(QNetworkRequest::UserAgentHeader).toString();
        const Route route = routes.value(request.url().toString(), Route{ 404, QByteArray(), false });
        return new FakeReply(request.url(), route.status, route.body, route.hold, this);
    }
};

class UpdateInstallerTest : public QObject
{
    Q_OBJECT

    QTemporaryDir root;
    QString install;
    QString updateDir;
    EVP_PKEY* key = nullptr;
    QByteArray rawPublic;

    QString version;             // the version carried by the staged executable
    QString tag;
    QString assetName;
    QByteArray zip;
    QByteArray sums;
    QByteArray sig;

    QString base() const { return "https://github.com/siberanka/NyxPlay/releases/download/" + tag + "/"; }
    QString mirror() const { return "https://gitlab.com/siberanka/NyxPlay/-/releases/" + tag + "/downloads/"; }

    QByteArray sign(const QByteArray& message) const
    {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        QByteArray out(64, '\0');
        size_t length = 64;
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(out.data()), &length,
                       reinterpret_cast<const unsigned char*>(message.constData()), size_t(message.size()));
        EVP_MD_CTX_free(ctx);
        return out.toBase64() + "\n";
    }

    void buildRelease()
    {
        version = UpdateStage::executableVersion(systemExe());
        tag = "v" + version;
        assetName = "NyxPlayPC-" + version + "-windows-x64-portable.zip";

        const QByteArray exe = readFile(systemExe());
        const QStringList files = { "NyxPlay.exe", "NyxPlayUpdater.exe", "Qt6Core.dll", "platforms/qwindows.dll", "qml/x/plugin.dll" };
        QBuffer buffer(&zip);
        buffer.open(QIODevice::WriteOnly);
        QZipWriter writer(&buffer);
        for (const QString& f : files) {
            writer.addFile(f, f.endsWith(".exe") ? exe : QByteArray("dll " + f.toUtf8()));
        }
        writer.addFile("package-files.txt", (files.join("\n") + "\n").toUtf8());
        writer.addFile("portable.dat", QByteArray());
        writer.addDirectory("boxart");
        writer.close();

        sums = QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() + "  " + assetName.toUtf8() + "\n" +
               QCryptographicHash::hash("other", QCryptographicHash::Sha256).toHex() + "  other.bin\n";
        sig = sign(sums);
    }

    UpdateLogic::Result offerResult() const
    {
        UpdateLogic::Result r;
        r.status = UpdateLogic::Status::UpdateAvailable;
        r.version = version;
        r.url = "https://github.com/siberanka/NyxPlay/releases/tag/" + tag;
        r.package.tag = tag;
        r.package.assetName = assetName;
        r.package.assetUrl = QUrl(base() + assetName);
        r.package.assetSize = zip.size();
        return r;
    }

    void serve(FakeNam& nam, const QByteArray& zipBody, const QByteArray& sumsBody, const QByteArray& sigBody)
    {
        nam.routes[base() + "SHA256SUMS.txt"] = { 200, sumsBody, false };
        nam.routes[base() + "SHA256SUMS.txt.sig"] = { 200, sigBody, false };
        nam.routes[base() + assetName] = { 200, zipBody, false };
    }

    struct Fixture {
        FakeNam nam;
        UpdateInstaller* installer = nullptr;
        QStringList launched;
        QList<int> states;
    };

    Fixture* make()
    {
        auto* f = new Fixture;
        QDir(install).removeRecursively();
        QDir(updateDir).removeRecursively();
        QDir().mkpath(install);
        writeFile(install + "/NyxPlay.exe", "old");
        f->installer = new UpdateInstaller(install, updateDir, "2026.11.19.1", nullptr, &f->nam);
        f->installer->setTrustedKeys({ rawPublic });
        f->installer->setLaunchHook([f](const QString& updater, const QStringList& args) {
            f->launched = QStringList(updater) + args;
            return true;
        });
        connect(f->installer, &UpdateInstaller::stateChanged, [f]() { f->states << f->installer->state(); });
        return f;
    }

    static void finishWork(Fixture* f) { delete f->installer; delete f; }

    bool leftovers() const
    {
        return !QDir(updateDir).entryList({ "*.part", "*.zip" }, QDir::Files).isEmpty() ||
               QDir(install + "/.nyxupdate/stage").exists();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(root.isValid());
        install = root.filePath("install");
        updateDir = root.filePath("cache/update");
        if (!QFile::exists(systemExe())) {
            QSKIP("no system executable to use as a test payload");
        }
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        QVERIFY(ctx);
        QVERIFY(EVP_PKEY_keygen_init(ctx) == 1);
        QVERIFY(EVP_PKEY_keygen(ctx, &key) == 1);
        EVP_PKEY_CTX_free(ctx);
        size_t length = 32;
        rawPublic.resize(32);
        QVERIFY(EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(rawPublic.data()), &length) == 1);
        buildRelease();
    }

    void cleanupTestCase()
    {
        EVP_PKEY_free(key);
    }

    void fullFlowReachesReadyToRestart()
    {
        Fixture* f = make();
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));
        QVERIFY(f->installer->canInstall());

        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);

        // The steps happen in this order: signed list, signature, package
        QCOMPARE(f->nam.requested, QStringList({ base() + "SHA256SUMS.txt", base() + "SHA256SUMS.txt.sig", base() + assetName }));
        for (const QString& ua : f->nam.userAgents) {
            QCOMPARE(ua, QString("NyxPlay/2026.11.19.1"));
        }
        QVERIFY(f->states.contains(int(UpdateInstaller::Downloading)));
        QVERIFY(f->states.contains(int(UpdateInstaller::Verifying)));
        QVERIFY(f->states.contains(int(UpdateInstaller::Extracting)));

        // Staged inside the install folder, the download is gone, nothing was installed yet
        QVERIFY(QFile::exists(install + "/.nyxupdate/stage/NyxPlay.exe"));
        QVERIFY(QFile::exists(install + "/.nyxupdate/stage/package-files.txt"));
        QVERIFY(QDir(updateDir).entryList({ "*.part", "*.zip" }, QDir::Files).isEmpty());
        QCOMPARE(readFile(install + "/NyxPlay.exe"), QByteArray("old"));
        QCOMPARE(f->installer->progress(), 100);

        f->installer->restartNow();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Restarting));
        QVERIFY(!f->launched.isEmpty());
        QCOMPARE(f->launched.at(0), QDir::cleanPath(updateDir + "/run/NyxPlayUpdater.exe"));
        QVERIFY(QFile::exists(f->launched.at(0)));
        const QStringList args = f->launched.mid(1);
        QCOMPARE(args.at(args.indexOf("--wait-pid") + 1), QString::number(QCoreApplication::applicationPid()));
        QCOMPARE(args.at(args.indexOf("--install-dir") + 1), QDir::toNativeSeparators(install));
        QCOMPARE(args.at(args.indexOf("--stage-dir") + 1), QDir::toNativeSeparators(install + "/.nyxupdate/stage"));
        QCOMPARE(args.at(args.indexOf("--version") + 1), version);
        finishWork(f);
    }

    void tamperedPackageIsRejectedAndDeleted()
    {
        Fixture* f = make();
        QByteArray bad = zip;
        bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x01);
        serve(f->nam, bad, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::VerifyFailed));
        QVERIFY(!leftovers());
        QCOMPARE(readFile(install + "/NyxPlay.exe"), QByteArray("old"));
        finishWork(f);
    }

    void tamperedSignatureStopsBeforeThePackage()
    {
        Fixture* f = make();
        QByteArray bad = sig;
        bad[10] = bad[10] == 'A' ? 'B' : 'A';
        serve(f->nam, zip, sums, bad);
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::VerifyFailed));
        QVERIFY(!f->nam.requested.contains(base() + assetName));
        QVERIFY(!leftovers());
        finishWork(f);
    }

    void tamperedChecksumListIsRejected()
    {
        Fixture* f = make();
        QByteArray bad = sums;
        bad.replace(0, 1, bad.at(0) == 'a' ? "b" : "a");
        serve(f->nam, zip, bad, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::VerifyFailed));
        QVERIFY(!f->nam.requested.contains(base() + assetName));
        finishWork(f);
    }

    void signatureOfAnotherKeyIsRejected()
    {
        Fixture* f = make();
        f->installer->setTrustedKeys({ QByteArray::fromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") });
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::VerifyFailed));
        finishWork(f);
    }

    void unlistedPackageIsRejected()
    {
        Fixture* f = make();
        const QByteArray other = QCryptographicHash::hash("other", QCryptographicHash::Sha256).toHex() + "  other.bin\n";
        serve(f->nam, zip, other, sign(other));
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::VerifyFailed));
        QVERIFY(!f->nam.requested.contains(base() + assetName));
        finishWork(f);
    }

    void wrongSizeIsRejected()
    {
        Fixture* f = make();
        serve(f->nam, zip.left(zip.size() - 10), sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QVERIFY(!leftovers());
        finishWork(f);
    }

    void packageThatFailsTheIntegrityCheckIsRejected()
    {
        // Correctly signed, but the executable has another version than the one offered
        Fixture* f = make();
        UpdateLogic::Result r = offerResult();
        r.version = "2099.1.1.1";
        r.package.tag = "v2099.1.1.1";
        r.package.assetName = "NyxPlayPC-2099.1.1.1-windows-x64-portable.zip";
        const QString b = "https://github.com/siberanka/NyxPlay/releases/download/v2099.1.1.1/";
        r.package.assetUrl = QUrl(b + r.package.assetName);
        const QByteArray s = QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() + "  " + r.package.assetName.toUtf8() + "\n";
        f->nam.routes[b + "SHA256SUMS.txt"] = { 200, s, false };
        f->nam.routes[b + "SHA256SUMS.txt.sig"] = { 200, sign(s), false };
        f->nam.routes[b + r.package.assetName] = { 200, zip, false };
        f->installer->offer(r);
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QCOMPARE(f->installer->error(), int(UpdateInstaller::PackageInvalid));
        QVERIFY(!leftovers());
        finishWork(f);
    }

    void unreachableServerIsReported()
    {
        Fixture* f = make();
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::Failed), 20000);
        QVERIFY(f->installer->error() == UpdateInstaller::DownloadFailed || f->installer->error() == UpdateInstaller::NoConnection);
        // Both locations were tried
        QVERIFY(f->nam.requested.contains(mirror() + "SHA256SUMS.txt"));
        f->installer->dismissError();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));
        finishWork(f);
    }

    void fallsBackToTheMirror()
    {
        Fixture* f = make();
        f->nam.routes[mirror() + "SHA256SUMS.txt"] = { 200, sums, false };
        f->nam.routes[mirror() + "SHA256SUMS.txt.sig"] = { 200, sig, false };
        f->nam.routes[mirror() + assetName] = { 200, zip, false };
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);
        QVERIFY(f->nam.requested.contains(base() + "SHA256SUMS.txt"));
        QVERIFY(f->nam.requested.contains(mirror() + assetName));
        finishWork(f);
    }

    void notWritableFolderIsReportedWithoutDownloading()
    {
        Fixture* f = make();
        QDir(install).removeRecursively();   // a folder that cannot be written: it does not exist
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Failed));
        QCOMPARE(f->installer->error(), int(UpdateInstaller::NotWritable));
        QVERIFY(f->nam.requested.isEmpty());
        finishWork(f);
    }

    void waitsForTheStreamToEnd()
    {
        Fixture* f = make();
        bool streaming = true;
        f->installer->setStreamProbe([&streaming]() { return streaming; });
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::WaitingForStream));
        QVERIFY(f->nam.requested.isEmpty());

        QMetaObject::invokeMethod(f->installer, "pollStream");
        QCOMPARE(f->installer->state(), int(UpdateInstaller::WaitingForStream));

        streaming = false;
        QMetaObject::invokeMethod(f->installer, "pollStream");
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);
        finishWork(f);
    }

    void doesNotAskToRestartDuringAStream()
    {
        Fixture* f = make();
        bool streaming = false;
        f->installer->setStreamProbe([&streaming]() { return streaming; });
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        streaming = true;   // a stream starts while the update is being prepared
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::WaitingForStream), 20000);
        QVERIFY(QFile::exists(install + "/.nyxupdate/stage/NyxPlay.exe"));
        f->installer->restartNow();
        QVERIFY(f->launched.isEmpty());

        streaming = false;
        QMetaObject::invokeMethod(f->installer, "pollStream");
        QCOMPARE(f->installer->state(), int(UpdateInstaller::ReadyToRestart));
        f->installer->restartNow();
        QVERIFY(!f->launched.isEmpty());
        finishWork(f);
    }

    void cancelStopsAndCleansUp()
    {
        Fixture* f = make();
        f->nam.routes[base() + "SHA256SUMS.txt"] = { 200, sums, false };
        f->nam.routes[base() + "SHA256SUMS.txt.sig"] = { 200, sig, false };
        f->nam.routes[base() + assetName] = { 200, zip, true };   // never completes
        f->installer->offer(offerResult());
        f->installer->install();
        QTRY_VERIFY_WITH_TIMEOUT(f->nam.requested.contains(base() + assetName), 10000);
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Downloading));

        f->installer->cancel();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));
        QVERIFY(!leftovers());
        QTest::qWait(50);
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));

        // And it can be started again
        f->nam.routes[base() + assetName] = { 200, zip, false };
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);
        finishWork(f);
    }

    void illegalTransitionsAreIgnored()
    {
        Fixture* f = make();
        serve(f->nam, zip, sums, sig);
        f->installer->install();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Idle));
        f->installer->restartNow();
        f->installer->cancel();
        f->installer->dismissError();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Idle));
        QVERIFY(f->nam.requested.isEmpty());

        f->installer->offer(offerResult());
        f->installer->restartNow();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));

        // A second install request while one runs does not start another
        f->installer->install();
        f->installer->install();
        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);
        QCOMPARE(f->nam.requested.count(base() + "SHA256SUMS.txt"), 1);

        // A new offer does not replace the staged update
        UpdateLogic::Result other = offerResult();
        other.version = "2099.1.1.1";
        f->installer->offer(other);
        QCOMPARE(f->installer->version(), version);
        finishWork(f);
    }

    void onlyOneWindowStagesAnUpdate()
    {
        Fixture* f = make();
        serve(f->nam, zip, sums, sig);
        f->installer->offer(offerResult());
        f->installer->install();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Downloading));

        // A second window of the same folder (its own installer) is refused while the first one works
        FakeNam nam2;
        serve(nam2, zip, sums, sig);
        UpdateInstaller second(install, updateDir, "2026.11.19.1", nullptr, &nam2);
        second.setTrustedKeys({ rawPublic });
        second.offer(offerResult());
        second.install();
        QCOMPARE(second.state(), int(UpdateInstaller::Failed));
        QCOMPARE(second.error(), int(UpdateInstaller::Busy));
        QVERIFY(nam2.requested.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(f->installer->state(), int(UpdateInstaller::ReadyToRestart), 20000);
        // The staged files of the first window are untouched by the second one's start-up maintenance
        UpdateInstaller third(install, updateDir, "2026.11.19.1", nullptr, &nam2);
        third.startupMaintenance();
        QVERIFY(QFile::exists(install + "/.nyxupdate/stage/NyxPlay.exe"));
        finishWork(f);
    }

    void offerWithoutPackageCannotBeInstalled()
    {
        Fixture* f = make();
        UpdateLogic::Result r = offerResult();
        r.package = UpdateLogic::Package();
        f->installer->offer(r);
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Available));
        QVERIFY(!f->installer->canInstall());
        f->installer->install();
        QCOMPARE(f->installer->state(), int(UpdateInstaller::Failed));
        QCOMPARE(f->installer->error(), int(UpdateInstaller::NotInstallable));
        QVERIFY(f->nam.requested.isEmpty());
        finishWork(f);
    }

    void startupMaintenanceReadsTheResultAndCleansUp()
    {
        Fixture* f = make();
        writeFile(updateDir + "/result.txt", "ok 2026.12.1.1\nlog replaced 5 files\n");
        writeFile(updateDir + "/NyxPlayPC-1.zip.part", "x");
        writeFile(updateDir + "/run/NyxPlayUpdater.exe", "x");
        writeFile(install + "/.nyxupdate/stage/NyxPlay.exe", "x");
        writeFile(install + "/NyxPlay.exe.nyxold", "old");
        writeFile(install + "/qml/a/plugin.dll.nyxold", "old");
        writeFile(install + "/boxart/keep.nyxold", "user data is not searched");
        writeFile(install + "/notes.txt", "mine");

        QSignalSpy spy(f->installer, &UpdateInstaller::lastResultChanged);
        f->installer->startupMaintenance();
        QCOMPARE(f->installer->lastResult(), QString("ok|2026.12.1.1"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(!QFile::exists(updateDir + "/result.txt"));
        QVERIFY(!QFile::exists(updateDir + "/NyxPlayPC-1.zip.part"));
        QVERIFY(!QFile::exists(updateDir + "/run"));
        QVERIFY(!QFile::exists(install + "/.nyxupdate"));
        QVERIFY(!QFile::exists(install + "/NyxPlay.exe.nyxold"));
        QVERIFY(!QFile::exists(install + "/qml/a/plugin.dll.nyxold"));
        QVERIFY(QFile::exists(install + "/boxart/keep.nyxold"));
        QVERIFY(QFile::exists(install + "/notes.txt"));

        writeFile(updateDir + "/result.txt", "failed locked qml/a/plugin.dll\nlog rolled back\n");
        f->installer->startupMaintenance();
        QCOMPARE(f->installer->lastResult(), QString("failed|locked|qml/a/plugin.dll"));
        f->installer->clearLastResult();
        QVERIFY(f->installer->lastResult().isEmpty());
        finishWork(f);
    }

    void startupMaintenanceLeavesARunningUpdaterAlone()
    {
        Fixture* f = make();
        const QString running = updateDir + "/run/NyxPlayUpdater.exe";
        writeFile(running, "x");
        writeFile(install + "/.nyxupdate/stage/NyxPlay.exe", "x");
        writeFile(install + "/NyxPlay.exe.nyxold", "old");
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(running).utf16()), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        f->installer->startupMaintenance();
        CloseHandle(h);
        QVERIFY(QFile::exists(install + "/.nyxupdate/stage/NyxPlay.exe"));
        QVERIFY(QFile::exists(install + "/NyxPlay.exe.nyxold"));
        finishWork(f);
    }
};

template <typename T>
static int runUpdateSuite(int argc, char** argv)
{
    T suite;
    QByteArray dir = qgetenv("TEST_REPORT_DIR");
    if (dir.isEmpty()) {
        return QTest::qExec(&suite, argc, argv);
    }
    QStringList args;
    for (int i = 0; i < argc; i++) {
        args << QString::fromLocal8Bit(argv[i]);
    }
    args << "-o" << QString("%1/%2.txt,txt").arg(QString::fromLocal8Bit(dir), suite.metaObject()->className());
    return QTest::qExec(&suite, args);
}

int runUpdateSuites(int argc, char** argv)
{
    int status = 0;
    status |= runUpdateSuite<UpdateManifestTest>(argc, argv);
    status |= runUpdateSuite<UpdaterApplyTest>(argc, argv);
    status |= runUpdateSuite<UpdateStageTest>(argc, argv);
    status |= runUpdateSuite<UpdateCommandTest>(argc, argv);
    status |= runUpdateSuite<PortableDetectTest>(argc, argv);
    status |= runUpdateSuite<UpdateInstallerTest>(argc, argv);
    return status;
}

#include "tst_update.moc"
