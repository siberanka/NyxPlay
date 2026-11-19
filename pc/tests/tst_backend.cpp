#include <QtTest>

#include "backend/nvhttp.h"
#include "backend/nvcomputer.h"
#include "backend/identitystore.h"
#include "backend/moonlightimport.h"
#include "backend/updateurl.h"
#include "backend/updatelogic.h"
#include "backend/releaseverify.h"

static QString wrap(const QString& body, int status = 200)
{
    return QString("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                   "<root status_code=\"%1\">%2</root>").arg(status).arg(body);
}

static QString app(const QString& title, const QString& id)
{
    return QString("<App><IsHdrSupported>1</IsHdrSupported><AppTitle>%1</AppTitle><ID>%2</ID></App>")
            .arg(title, id);
}

class AppListParseTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesNormalList()
    {
        auto apps = NvHTTP::parseAppList(wrap(app("A", "1") + app("B", "2") + app("C", "3")));
        QCOMPARE(apps.size(), 3);
        QCOMPARE(apps[1].name, QString("B"));
        QCOMPARE(apps[1].id, 2);
        QVERIFY(apps[1].hdrSupported);
    }

    void skipsEntryWithoutId()
    {
        auto apps = NvHTTP::parseAppList(wrap(app("A", "1") + "<App><AppTitle/><ID>0</ID></App>" + app("C", "3")));
        QCOMPARE(apps.size(), 2);
        QCOMPARE(apps[0].id, 1);
        QCOMPARE(apps[1].id, 3);
    }

    void keepsEmptyNameWithValidId()
    {
        auto apps = NvHTTP::parseAppList(wrap("<App><AppTitle/><ID>7</ID></App>"));
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps[0].id, 7);
        QVERIFY(apps[0].isInitialized());
    }

    void skipsEntryMissingTitle()
    {
        auto apps = NvHTTP::parseAppList(wrap("<App><ID>5</ID></App>" + app("Good", "6")));
        QCOMPARE(apps.size(), 2);
        QCOMPARE(apps[1].name, QString("Good"));
    }

    void lastEntryValidated()
    {
        auto apps = NvHTTP::parseAppList(wrap(app("A", "1") + "<App><AppTitle>X</AppTitle><ID>0</ID></App>"));
        QCOMPARE(apps.size(), 1);
    }

    void truncatedXmlThrows()
    {
        QString xml = wrap(app("A", "1") + app("B", "2"));
        xml.truncate(xml.size() - 30);
        QVERIFY_EXCEPTION_THROWN(NvHTTP::parseAppList(xml), std::exception);
    }

    void entryCap()
    {
        QString body;
        for (int i = 1; i <= NvHTTP::MAX_APPLIST_ENTRIES + 1000; i++) {
            body += app("n", QString::number(i));
        }
        QCOMPARE(NvHTTP::parseAppList(wrap(body)).size(), NvHTTP::MAX_APPLIST_ENTRIES);
    }

    void longNameTruncated()
    {
        auto apps = NvHTTP::parseAppList(wrap(app(QString(100000, 'x'), "1")));
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps[0].name.size(), NvHTTP::MAX_APP_NAME_LENGTH);
    }

    void negativeAndHugeIdsSkipped()
    {
        auto apps = NvHTTP::parseAppList(wrap(app("A", "-1") + app("B", "99999999999") + app("C", "4")));
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps[0].id, 4);
    }

    void statusNot200Throws()
    {
        QVERIFY_EXCEPTION_THROWN(NvHTTP::parseAppList(wrap("", 401)), GfeHttpResponseException);
    }
};

class NetworkErrorTest : public QObject
{
    Q_OBJECT

private slots:
    void resetCodes_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("reset");
        QTest::newRow("wsa reset") << 10054 << true;
        QTest::newRow("wsa abort") << 10053 << true;
        QTest::newRow("linux reset") << 104 << true;
        QTest::newRow("wsa timeout") << 10060 << false;
        QTest::newRow("refused") << 10061 << false;
        QTest::newRow("unknown") << 12345 << false;
    }

    void resetCodes()
    {
        QFETCH(int, code);
        QFETCH(bool, reset);
        QCOMPARE(NvHTTP::isConnectionResetError(code), reset);
    }

    void descriptionsDiffer()
    {
        QString reset = NvHTTP::describeNetworkError(10054);
        QString blocked = NvHTTP::describeNetworkError(10060);
        QVERIFY(!reset.contains("firewall", Qt::CaseInsensitive));
        QVERIFY(blocked.contains("firewall", Qt::CaseInsensitive));
        QVERIFY(NvHTTP::describeNetworkError(4242).contains("4242"));
    }
};

class UuidTest : public QObject
{
    Q_OBJECT

private slots:
    void validation_data()
    {
        QTest::addColumn<QString>("uuid");
        QTest::addColumn<bool>("valid");
        QTest::newRow("hex") << "0123456789ABCDEF" << true;
        QTest::newRow("hyphens") << "a-b-c" << true;
        QTest::newRow("64 chars") << QString(64, 'a') << true;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("65 chars") << QString(65, 'a') << false;
        QTest::newRow("traversal") << "../x" << false;
        QTest::newRow("slash") << "a/b" << false;
        QTest::newRow("backslash") << "a\b" << false;
        QTest::newRow("space") << "a b" << false;
        QTest::newRow("braces") << "{id}" << false;
        QTest::newRow("unicode") << QString::fromUtf8("\xc3\xa9") << false;
    }

    void validation()
    {
        QFETCH(QString, uuid);
        QFETCH(bool, valid);
        QCOMPARE(NvComputer::isValidUuid(uuid), valid);
    }

    void hostName()
    {
        QCOMPARE(NvComputer::sanitizeName("Living Room PC"), QString("Living Room PC"));
        // A right-to-left override would make the following text read backwards
        QCOMPARE(NvComputer::sanitizeName(QString::fromUtf8("PC\xe2\x80\xae" "exe.txt")), QString("PCexe.txt"));
        QCOMPARE(NvComputer::sanitizeName(QString("A\r\n\tB  ")), QString("AB"));
        QCOMPARE(NvComputer::sanitizeName(QString(2 * 1024 * 1024, 'x')).size(), 64);

        // Characters outside the BMP count once and are never split
        QString emoji;
        for (int i = 0; i < 100; i++) {
            emoji += QString::fromUtf8("\xf0\x9f\x8e\xae");
        }
        QString cut = NvComputer::sanitizeName(emoji);
        QCOMPARE(cut.size(), 128);
        QVERIFY(cut.back().isLowSurrogate());
    }
};

class IdentityStoreTest : public QObject
{
    Q_OBJECT

private:
    static QByteArray samplePem()
    {
        return "-----BEGIN PRIVATE KEY-----\nQUJDREVGR0hJSktMTU5PUA==\n-----END PRIVATE KEY-----\n";
    }

private slots:
    void roundTrip()
    {
#ifdef Q_OS_WIN
        QByteArray blob = IdentityStore::protect(samplePem());
        QVERIFY(!blob.isEmpty());
        QVERIFY(!blob.contains("PRIVATE KEY"));
        QCOMPARE(IdentityStore::unprotect(blob), samplePem());
#else
        QVERIFY(IdentityStore::protect(samplePem()).isEmpty());
#endif
    }

    void tamperedBlobIsRejected()
    {
#ifdef Q_OS_WIN
        QByteArray blob = IdentityStore::protect(samplePem());
        QVERIFY(!blob.isEmpty());
        blob[blob.size() / 2] = blob[blob.size() / 2] ^ 0x5A;
        QVERIFY(IdentityStore::unprotect(blob).isEmpty());
        QVERIFY(IdentityStore::unprotect(QByteArray("not a protected value")).isEmpty());
#endif
    }

    void saveThenLoad()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath("a.ini"), QSettings::IniFormat);

        QCOMPARE(IdentityStore::load(s).status, IdentityStore::Status::Missing);

        IdentityStore::save(s, samplePem());
        IdentityStore::Result r = IdentityStore::load(s);
        QCOMPARE(r.status, IdentityStore::Status::Loaded);
        QCOMPARE(r.key, samplePem());
#ifdef Q_OS_WIN
        QVERIFY(!s.contains(IdentityStore::KEY_PLAIN));
        QVERIFY(s.contains(IdentityStore::KEY_PROTECTED));
#endif
    }

    void plainKeyIsMigrated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString path = dir.filePath("b.ini");
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue(IdentityStore::KEY_PLAIN, samplePem());
        }

        QSettings s(path, QSettings::IniFormat);
        IdentityStore::Result r = IdentityStore::load(s);
        QCOMPARE(r.key, samplePem());
#ifdef Q_OS_WIN
        QCOMPARE(r.status, IdentityStore::Status::Migrated);
        s.sync();
        QVERIFY(!s.contains(IdentityStore::KEY_PLAIN));

        // The stored file no longer contains the key material
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("PRIVATE KEY"));
        f.close();

        // And the migrated value loads again
        QCOMPARE(IdentityStore::load(s).key, samplePem());
#else
        QCOMPARE(r.status, IdentityStore::Status::Loaded);
#endif
    }

    void undecryptableValueIsReported()
    {
#ifdef Q_OS_WIN
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath("c.ini"), QSettings::IniFormat);
        s.setValue(IdentityStore::KEY_PROTECTED, QByteArray("value from another machine"));
        IdentityStore::Result r = IdentityStore::load(s);
        QCOMPARE(r.status, IdentityStore::Status::Undecryptable);
        QVERIFY(r.key.isEmpty());
#endif
    }

    void decryptErrorsAreClassified()
    {
        // Damaged data and keys of another account never recover
        QVERIFY(IdentityStore::isPermanentDecryptError(13));
        QVERIFY(IdentityStore::isPermanentDecryptError(0x80090005UL));
        QVERIFY(IdentityStore::isPermanentDecryptError(0x8009000BUL));

        // Access problems, missing resources, an unavailable service or an unknown code may pass
        QVERIFY(!IdentityStore::isPermanentDecryptError(0));
        QVERIFY(!IdentityStore::isPermanentDecryptError(5));      // ERROR_ACCESS_DENIED
        QVERIFY(!IdentityStore::isPermanentDecryptError(8));      // ERROR_NOT_ENOUGH_MEMORY
        QVERIFY(!IdentityStore::isPermanentDecryptError(1722));   // RPC_S_SERVER_UNAVAILABLE
        QVERIFY(!IdentityStore::isPermanentDecryptError(0x80090016UL)); // NTE_BAD_KEYSET
    }

    void unprotectReportsPermanentFailureForGarbage()
    {
#ifdef Q_OS_WIN
        bool permanent = false;
        QVERIFY(IdentityStore::unprotect(QByteArray("not a protected value"), &permanent).isEmpty());
        QVERIFY(permanent);
#endif
    }

    void undecryptableValueIsKept()
    {
#ifdef Q_OS_WIN
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath("keep.ini"), QSettings::IniFormat);
        QByteArray blob("value from another machine");
        s.setValue(IdentityStore::KEY_PROTECTED, blob);
        IdentityStore::load(s);
        QCOMPARE(s.value(IdentityStore::KEY_PROTECTED).toByteArray(), blob);
        QVERIFY(!s.contains(IdentityStore::KEY_PLAIN));
#endif
    }

    void clearsPinnedCertificatesOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath("d.ini"), QSettings::IniFormat);
        s.beginWriteArray("hosts");
        for (int i = 0; i < 2; i++) {
            s.setArrayIndex(i);
            s.setValue("uuid", QString("host-%1").arg(i));
            s.setValue("hostname", QString("PC %1").arg(i));
            s.setValue("srvcert", QByteArray("CERT"));
        }
        s.endArray();

        QCOMPARE(IdentityStore::clearHostPairings(s), 2);

        QCOMPARE(s.beginReadArray("hosts"), 2);
        for (int i = 0; i < 2; i++) {
            s.setArrayIndex(i);
            QVERIFY(s.value("srvcert").toByteArray().isEmpty());
            QCOMPARE(s.value("uuid").toString(), QString("host-%1").arg(i));
            QCOMPARE(s.value("hostname").toString(), QString("PC %1").arg(i));
        }
        s.endArray();

        QCOMPARE(IdentityStore::clearHostPairings(s), 0);
    }

    void noticeIsReturnedOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath("e.ini"), QSettings::IniFormat);
        QVERIFY(IdentityStore::takeStartupNotice(s).isEmpty());
        IdentityStore::setStartupNotice(s, "repair");
        QCOMPARE(IdentityStore::takeStartupNotice(s), QString("repair"));
        QVERIFY(IdentityStore::takeStartupNotice(s).isEmpty());
    }
};

class UpdateUrlTest : public QObject
{
    Q_OBJECT

private slots:
    void accepts_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("expected");

        QTest::newRow("repo") << "https://github.com/siberanka/NyxPlay"
                              << "https://github.com/siberanka/NyxPlay";
        QTest::newRow("release") << "https://github.com/siberanka/NyxPlay/releases/tag/v1"
                                 << "https://github.com/siberanka/NyxPlay/releases/tag/v1";
        QTest::newRow("trailing slash") << "https://github.com/siberanka/NyxPlay/"
                                        << "https://github.com/siberanka/NyxPlay/";
        QTest::newRow("gitlab mirror") << "https://gitlab.com/siberanka/NyxPlay/-/releases/v1"
                                       << "https://gitlab.com/siberanka/NyxPlay/-/releases/v1";
    }

    void accepts()
    {
        QFETCH(QString, url);
        QFETCH(QString, expected);
        QString normalized;
        QVERIFY(normalizeTrustedUpdateUrl(url, &normalized));
        QCOMPARE(normalized, expected);
    }

    void rejects_data()
    {
        QTest::addColumn<QString>("url");

        QTest::newRow("http") << "http://github.com/siberanka/NyxPlay/x";
        QTest::newRow("other host") << "https://example.com/siberanka/NyxPlay/x";
        QTest::newRow("lookalike host") << "https://github.com.evil.example/siberanka/NyxPlay/x";
        QTest::newRow("userinfo") << "https://user@github.com/siberanka/NyxPlay/x";
        QTest::newRow("port") << "https://github.com:8443/siberanka/NyxPlay/x";
        QTest::newRow("other repo") << "https://github.com/siberanka/Other/x";
        QTest::newRow("sibling prefix") << "https://github.com/siberanka/NyxPlayEvil/x";
        QTest::newRow("dot dot") << "https://github.com/siberanka/NyxPlay/../../attacker/repo/x";
        QTest::newRow("dot dot at end") << "https://github.com/siberanka/NyxPlay/..";
        QTest::newRow("single dot") << "https://github.com/siberanka/NyxPlay/./x";
        QTest::newRow("encoded dot dot") << "https://github.com/siberanka/NyxPlay/%2e%2e/%2E%2e/attacker/x";
        QTest::newRow("mixed dot dot") << "https://github.com/siberanka/NyxPlay/.%2e/attacker/x";
        QTest::newRow("encoded slash") << "https://github.com/siberanka/NyxPlay%2f..%2f..%2fattacker";
        QTest::newRow("encoded backslash") << "https://github.com/siberanka/NyxPlay/%5c..%5cattacker";
        QTest::newRow("backslash") << "https://github.com/siberanka/NyxPlay/\\..\\x";
        QTest::newRow("double slash") << "https://github.com/siberanka/NyxPlay//x";
        QTest::newRow("escapes up") << "https://github.com/siberanka/NyxPlay/x/../../../y";
        QTest::newRow("empty") << "";
        QTest::newRow("not a url") << "not a url";
    }

    void rejects()
    {
        QFETCH(QString, url);
        QString normalized = "untouched";
        QVERIFY(!normalizeTrustedUpdateUrl(url, &normalized));
        QCOMPARE(normalized, QString("untouched"));
    }
};

class UpdateLogicTest : public QObject
{
    Q_OBJECT

    static QByteArray manifest(const QString& version, const QString& url = "https://github.com/siberanka/NyxPlay/releases/tag/v1",
                               const QString& arch = "x86_64", const QString& platform = "windows")
    {
        return QString("[{\"platform\":\"%1\",\"arch\":\"%2\",\"version\":\"%3\",\"browser_url\":\"%4\"}]")
                .arg(platform, arch, version, url).toUtf8();
    }

    static UpdateLogic::Result eval(const QByteArray& body, const QString& current = "2026.11.19.1",
                                    const QString& kernel = "10.0.26100")
    {
        return UpdateLogic::evaluateManifest(body, current, "x86_64", "windows", kernel);
    }

private slots:
    void parsesVersions_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("lenient");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QVector<int>>("expected");

        QTest::newRow("four parts") << "2026.11.19.1" << false << true << QVector<int>{2026, 11, 19, 1};
        QTest::newRow("no leading zero") << "2026.12.1.1" << false << true << QVector<int>{2026, 12, 1, 1};
        QTest::newRow("leading zeros") << "2026.01.02.03" << false << true << QVector<int>{2026, 1, 2, 3};
        QTest::newRow("short") << "1.0" << false << true << QVector<int>{1, 0};
        QTest::newRow("whitespace") << " 3.2.1 " << false << true << QVector<int>{3, 2, 1};
        QTest::newRow("empty") << "" << false << false << QVector<int>{};
        QTest::newRow("suffix") << "2026.11.19.1-beta" << false << false << QVector<int>{};
        QTest::newRow("letters") << "a.b.c" << false << false << QVector<int>{};
        QTest::newRow("empty component") << "1..2" << false << false << QVector<int>{};
        QTest::newRow("negative") << "1.-2" << false << false << QVector<int>{};
        QTest::newRow("huge component") << "1.99999999999" << false << false << QVector<int>{};
        QTest::newRow("too many parts") << "1.2.3.4.5.6.7.8.9" << false << false << QVector<int>{};
        QTest::newRow("kernel lenient") << "6.5.0-generic" << true << true << QVector<int>{6, 5, 0};
        QTest::newRow("kernel build") << "10.0.26100" << true << true << QVector<int>{10, 0, 26100};
    }

    void parsesVersions()
    {
        QFETCH(QString, text);
        QFETCH(bool, lenient);
        QFETCH(bool, ok);
        QFETCH(QVector<int>, expected);
        QVector<int> version;
        QCOMPARE(UpdateLogic::parseVersion(text, version, lenient), ok);
        QCOMPARE(version, expected);
    }

    void comparesCalendarVersions()
    {
        QVector<int> a, b;
        QVERIFY(UpdateLogic::parseVersion("2026.12.1.1", a));
        QVERIFY(UpdateLogic::parseVersion("2026.11.30.9", b));
        QCOMPARE(UpdateLogic::compareVersions(a, b), 1);
        QCOMPARE(UpdateLogic::compareVersions(b, a), -1);
        QCOMPARE(UpdateLogic::compareVersions(a, a), 0);

        QVERIFY(UpdateLogic::parseVersion("2026.11.19.2", a));
        QVERIFY(UpdateLogic::parseVersion("2026.11.19.10", b));
        QCOMPARE(UpdateLogic::compareVersions(a, b), -1);
    }

    void missingComponentsCountAsZero()
    {
        QVector<int> a, b;
        QVERIFY(UpdateLogic::parseVersion("2026.11.19", a));
        QVERIFY(UpdateLogic::parseVersion("2026.11.19.0", b));
        QCOMPARE(UpdateLogic::compareVersions(a, b), 0);
        QVERIFY(UpdateLogic::parseVersion("2026.11.19.1", b));
        QCOMPARE(UpdateLogic::compareVersions(a, b), -1);
    }

    void detectsNewerVersion()
    {
        UpdateLogic::Result r = eval(manifest("2026.12.1.1"));
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QCOMPARE(r.version, QString("2026.12.1.1"));
        QCOMPARE(r.url, QString("https://github.com/siberanka/NyxPlay/releases/tag/v1"));
    }

    void newerWithoutLeadingZeroBeatsOlder()
    {
        QCOMPARE(eval(manifest("2026.12.1.1"), "2026.11.30.9").status, UpdateLogic::Status::UpdateAvailable);
        QCOMPARE(eval(manifest("2026.11.30.9"), "2026.12.1.1").status, UpdateLogic::Status::UpToDate);
    }

    void equalOrOlderIsUpToDate()
    {
        QCOMPARE(eval(manifest("2026.11.19.1")).status, UpdateLogic::Status::UpToDate);
        QCOMPARE(eval(manifest("2026.11.18.5")).status, UpdateLogic::Status::UpToDate);
        QVERIFY(!UpdateLogic::shouldTryFallback(UpdateLogic::Status::UpToDate));
        QVERIFY(!UpdateLogic::shouldTryFallback(UpdateLogic::Status::UpdateAvailable));
    }

    void otherArchOrPlatformIsIgnored()
    {
        QCOMPARE(eval(manifest("2099.1.1.1", "https://github.com/siberanka/NyxPlay", "arm64")).status,
                 UpdateLogic::Status::NoMatchingEntry);
        QCOMPARE(eval(manifest("2099.1.1.1", "https://github.com/siberanka/NyxPlay", "x86_64", "osx")).status,
                 UpdateLogic::Status::NoMatchingEntry);
    }

    void untrustedUrlFallsBackToReleasesPage_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("other host") << "https://example.com/siberanka/NyxPlay/releases";
        QTest::newRow("http") << "http://github.com/siberanka/NyxPlay/releases";
        QTest::newRow("other repo") << "https://github.com/someone/else/releases";
        QTest::newRow("dot dot") << "https://github.com/siberanka/NyxPlay/../../evil/x";
        QTest::newRow("file") << "file:///C:/Windows/System32/calc.exe";
        QTest::newRow("empty") << "";
    }

    void untrustedUrlFallsBackToReleasesPage()
    {
        QFETCH(QString, url);
        UpdateLogic::Result r = eval(manifest("2099.1.1.1", url));
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QCOMPARE(r.url, UpdateLogic::fallbackReleasesUrl());
    }

    void gitlabMirrorUrlIsTrusted()
    {
        UpdateLogic::Result r = eval(manifest("2099.1.1.1", "https://gitlab.com/siberanka/NyxPlay/-/releases/v2099.1.1.1"));
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QCOMPARE(r.url, QString("https://gitlab.com/siberanka/NyxPlay/-/releases/v2099.1.1.1"));
        QVERIFY(!normalizeTrustedUpdateUrl("https://gitlab.com/other/NyxPlay/x"));
        QVERIFY(!normalizeTrustedUpdateUrl("https://gitlab.com.evil.example/siberanka/NyxPlay/x"));
    }

    void malformedManifestsAreRejected_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not json") << QByteArray("<html>rate limited</html>");
        QTest::newRow("truncated") << QByteArray("[{\"platform\":\"windows\",");
        QTest::newRow("object") << QByteArray("{\"platform\":\"windows\"}");
        QTest::newRow("string") << QByteArray("\"x\"");
        QTest::newRow("binary") << QByteArray("\x00\x01\x02\xff\xfe", 5);
    }

    void malformedManifestsAreRejected()
    {
        QFETCH(QByteArray, body);
        UpdateLogic::Result r = eval(body);
        QCOMPARE(r.status, UpdateLogic::Status::Malformed);
        QVERIFY(UpdateLogic::shouldTryFallback(r.status));
    }

    void emptyArrayHasNoEntry()
    {
        UpdateLogic::Result r = eval("[]");
        QCOMPARE(r.status, UpdateLogic::Status::NoMatchingEntry);
        QVERIFY(UpdateLogic::shouldTryFallback(r.status));
    }

    void badEntriesAreSkipped_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("missing field") << QByteArray("[{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"2099.1.1.1\"}]");
        QTest::newRow("wrong type") << QByteArray("[{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":20991,\"browser_url\":\"https://github.com/siberanka/NyxPlay\"}]");
        QTest::newRow("non object") << QByteArray("[1,\"a\",null,[]]");
        QTest::newRow("bad version") << QByteArray("[{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"latest\",\"browser_url\":\"https://github.com/siberanka/NyxPlay\"}]");
    }

    void badEntriesAreSkipped()
    {
        QFETCH(QByteArray, body);
        QCOMPARE(eval(body).status, UpdateLogic::Status::NoMatchingEntry);
    }

    void badEntryDoesNotHideGoodOne()
    {
        QByteArray body = "[1,{\"platform\":\"windows\"},{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"x\",\"browser_url\":\"https://github.com/siberanka/NyxPlay\"},"
                          "{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"2099.1.1.1\",\"browser_url\":\"https://github.com/siberanka/NyxPlay/releases\"}]";
        UpdateLogic::Result r = eval(body);
        QCOMPARE(r.status, UpdateLogic::Status::UpdateAvailable);
        QCOMPARE(r.version, QString("2099.1.1.1"));
    }

    void invalidCurrentVersionReportsNothing()
    {
        QCOMPARE(eval(manifest("2099.1.1.1"), "dev-build").status, UpdateLogic::Status::Malformed);
    }

    void oversizedBodyIsRejected()
    {
        QByteArray body = "[";
        while (body.size() <= UpdateLogic::MaxManifestBytes) {
            body += "{\"platform\":\"linux\",\"arch\":\"x86_64\",\"version\":\"1.0\",\"browser_url\":\"https://github.com/siberanka/NyxPlay\"},";
        }
        body += manifest("2099.1.1.1").mid(1);
        QCOMPARE(eval(body).status, UpdateLogic::Status::Malformed);
    }

    void entryCountIsBounded()
    {
        QByteArray body = "[";
        for (int i = 0; i < UpdateLogic::MaxManifestEntries; i++) {
            body += "{},";
        }
        body += manifest("2099.1.1.1").mid(1);
        QCOMPARE(eval(body).status, UpdateLogic::Status::NoMatchingEntry);
    }

    void kernelMinimumIsHonoured()
    {
        QByteArray body = "[{\"platform\":\"windows\",\"arch\":\"x86_64\",\"version\":\"2099.1.1.1\","
                          "\"browser_url\":\"https://github.com/siberanka/NyxPlay\",\"kernel_version_at_least\":\"10.0.19045\"}]";
        QCOMPARE(eval(body, "2026.11.19.1", "10.0.17763").status, UpdateLogic::Status::NoMatchingEntry);
        QCOMPARE(eval(body, "2026.11.19.1", "10.0.26100").status, UpdateLogic::Status::UpdateAvailable);
    }

    void fallbackDecision()
    {
        // GitHub unusable, so the mirror is consulted; a usable answer stops the chain
        QVERIFY(UpdateLogic::shouldTryFallback(eval("<html>").status));
        QVERIFY(UpdateLogic::shouldTryFallback(eval("[]").status));
        QVERIFY(UpdateLogic::shouldTryFallback(eval("not json").status));
        QVERIFY(!UpdateLogic::shouldTryFallback(eval(manifest("2026.11.19.1")).status));
    }
};

class FakeImportSource : public ImportSource
{
public:
    QList<QVariantMap> hostList;
    QVariantMap prefs;

    QList<QVariantMap> hosts() const override { return hostList; }
    QVariantMap preferences() const override { return prefs; }
};

class MoonlightImportTest : public QObject
{
    Q_OBJECT

private:
    static QVariantMap goodHost(const QString& uuid)
    {
        QVariantMap h;
        h["uuid"] = uuid;
        h["hostname"] = "Desktop";
        h["mac"] = QByteArray("\x01\x02\x03\x04\x05\x06", 6);
        h["localaddress"] = "192.168.1.20";
        h["localport"] = 47989;
        h["manualaddress"] = "host.example.com";
        h["srvcert"] = QByteArray("CERT");
        h["apps"] = "ignored";
        return h;
    }

private slots:
    void importsHostsWithoutPairingState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("f.ini"), QSettings::IniFormat);
        QVERIFY(MoonlightImport::isNeeded(dest));

        FakeImportSource src;
        src.hostList << goodHost("ABC-123") << goodHost("DEF-456");
        MoonlightImport::Result r = MoonlightImport::run(dest, src);
        QCOMPARE(r.hostsImported, 2);
        QCOMPARE(r.hostsSkipped, 0);

        QCOMPARE(dest.beginReadArray("hosts"), 2);
        dest.setArrayIndex(0);
        QCOMPARE(dest.value("uuid").toString(), QString("ABC-123"));
        QCOMPARE(dest.value("hostname").toString(), QString("Desktop"));
        QCOMPARE(dest.value("mac").toByteArray().size(), 6);
        QCOMPARE(dest.value("localaddress").toString(), QString("192.168.1.20"));
        QCOMPARE(dest.value("localport").toInt(), 47989);
        QVERIFY(!dest.contains("srvcert"));
        QVERIFY(!dest.contains("apps"));
        dest.endArray();

        QVERIFY(!MoonlightImport::isNeeded(dest));
    }

    void neverCopiesIdentity()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("g.ini"), QSettings::IniFormat);

        FakeImportSource src;
        src.hostList << goodHost("ABC-123");
        src.prefs["key"] = "-----BEGIN PRIVATE KEY-----";
        src.prefs["certificate"] = "-----BEGIN CERTIFICATE-----";
        src.prefs["uniqueid"] = "deadbeef";
        src.prefs["fps"] = 90;
        MoonlightImport::Result r = MoonlightImport::run(dest, src);
        QCOMPARE(r.preferencesImported, 1);

        QVERIFY(!dest.contains("key"));
        QVERIFY(!dest.contains("certificate"));
        QVERIFY(!dest.contains("uniqueid"));
        QCOMPARE(dest.value("fps").toInt(), 90);
    }

    void skipsMalformedEntries()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("h.ini"), QSettings::IniFormat);

        FakeImportSource src;
        QVariantMap badUuid = goodHost("../evil");
        QVariantMap noAddress = goodHost("NOADDR");
        noAddress.remove("localaddress");
        noAddress.remove("manualaddress");
        QVariantMap badPort = goodHost("BADPORT");
        badPort["localport"] = 99999;
        QVariantMap badAddr = goodHost("BADADDR");
        badAddr["localaddress"] = "a b\\c";
        badAddr.remove("manualaddress");
        src.hostList << badUuid << noAddress << goodHost("DUP") << goodHost("DUP")
                     << badPort << badAddr << goodHost("OK-1");

        MoonlightImport::Result r = MoonlightImport::run(dest, src);
        QCOMPARE(r.hostsImported, 3); // DUP, BADPORT (port dropped), OK-1
        QCOMPARE(r.hostsSkipped, 4);

        QCOMPARE(dest.beginReadArray("hosts"), 3);
        dest.setArrayIndex(1);
        QCOMPARE(dest.value("uuid").toString(), QString("BADPORT"));
        QVERIFY(!dest.contains("localport"));
        dest.endArray();
    }

    void rejectsOutOfRangePreferences()
    {
        QVariantMap raw;
        raw["width"] = 99999999;
        raw["height"] = "abc";
        raw["fps"] = 120;
        raw["bitrate"] = -5;
        raw["audiocfg"] = 7;
        raw["hdr"] = "true";
        raw["vsync"] = "maybe";
        raw["unknownkey"] = 1;

        QVariantMap out = MoonlightImport::sanitizePreferences(raw);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.value("fps").toInt(), 120);
        QCOMPARE(out.value("hdr").toBool(), true);
    }

    void doneFlagSetEvenWhenNothingImported()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("i.ini"), QSettings::IniFormat);

        FakeImportSource src;
        src.hostList << goodHost("../evil");
        MoonlightImport::Result r = MoonlightImport::run(dest, src);
        QCOMPARE(r.hostsImported, 0);
        QVERIFY(!MoonlightImport::isNeeded(dest));
        QCOMPARE(dest.beginReadArray("hosts"), 0);
        dest.endArray();
    }

    void notNeededWhenHostsExist()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("j.ini"), QSettings::IniFormat);
        dest.beginWriteArray("hosts");
        dest.setArrayIndex(0);
        dest.setValue("uuid", "X");
        dest.endArray();
        QVERIFY(!MoonlightImport::isNeeded(dest));
    }

    void doesNotOverwriteExistingPreferences()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings dest(dir.filePath("k.ini"), QSettings::IniFormat);
        dest.setValue("fps", 30);

        FakeImportSource src;
        src.prefs["fps"] = 144;
        MoonlightImport::run(dest, src);
        QCOMPARE(dest.value("fps").toInt(), 30);
    }
};

class ReleaseVerifyTest : public QObject
{
    Q_OBJECT

    QByteArray sums;
    QByteArray sig;
    QByteArray bare;   // the signature without its trailing line feed
    QTemporaryDir work;

    static QByteArray readAll(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    QString write(const QString& name, const QByteArray& content)
    {
        QString path = work.filePath(name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(content);
        }
        return path;
    }

    static QByteArray largeContent()
    {
        QByteArray data(5 * 1024 * 1024 + 17, Qt::Uninitialized);
        for (qsizetype i = 0; i < data.size(); i++) {
            data[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        }
        return data;
    }

    static QList<QByteArray> otherKey()
    {
        // An unrelated public key (RFC 8032 test vector 1)
        return { QByteArray::fromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") };
    }

    const QString installerName = "NyxPlayHost-2026.11.19.2-windows-x64.exe";
    const QString largeName = "NyxPlay-2026.11.19.2-arm64-v8a-release.apk";
    const QString emptyName = "NyxPlay-empty.bin";

private slots:
    void initTestCase()
    {
        QVERIFY(work.isValid());
        const QString dir = QStringLiteral(RELEASE_FIXTURE_DIR);
        sums = readAll(dir + "/SHA256SUMS.txt");
        sig = readAll(dir + "/SHA256SUMS.txt.sig");
        QVERIFY(!sums.isEmpty());
        QVERIFY(sig.size() == 89);
        bare = sig.left(88);
    }

    void embeddedKeyFingerprint()
    {
        QCOMPARE(ReleaseVerify::trustedKeys().size(), 1);
        QCOMPARE(ReleaseVerify::keyFingerprint(ReleaseVerify::trustedKeys().first()),
                 QString("db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1"));
    }

    void realSignatureVerifies()
    {
        QVERIFY(ReleaseVerify::verifySignature(sums, sig) == ReleaseVerify::Result::Ok);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare) == ReleaseVerify::Result::Ok);
    }

    void tamperedSumsAreRejected()
    {
        QByteArray changed = sums;
        changed[0] = changed[0] == 'a' ? 'b' : 'a';
        QVERIFY(ReleaseVerify::verifySignature(changed, sig) == ReleaseVerify::Result::BadSignature);
        QVERIFY(ReleaseVerify::verifySignature(sums + "\n", sig) == ReleaseVerify::Result::BadSignature);
        QVERIFY(ReleaseVerify::verifySignature(sums.mid(1), sig) == ReleaseVerify::Result::BadSignature);
    }

    void tamperedSignatureIsRejected()
    {
        QByteArray changed = sig;
        changed[0] = changed[0] == 'A' ? 'B' : 'A';
        QVERIFY(ReleaseVerify::verifySignature(sums, changed) == ReleaseVerify::Result::BadSignature);
        changed = sig;
        changed[40] = changed[40] == 'A' ? 'B' : 'A';
        QVERIFY(ReleaseVerify::verifySignature(sums, changed) == ReleaseVerify::Result::BadSignature);
    }

    void wrongKeyIsRejected()
    {
        QVERIFY(ReleaseVerify::verifySignature(sums, sig, otherKey()) == ReleaseVerify::Result::BadSignature);
        QVERIFY(ReleaseVerify::verifySignature(sums, sig, { QByteArray(32, '\0') }) != ReleaseVerify::Result::Ok);
        QVERIFY(ReleaseVerify::verifySignature(sums, sig, { QByteArray(31, 'x') }) == ReleaseVerify::Result::BadSignature);
        QVERIFY(ReleaseVerify::verifySignature(sums, sig, QList<QByteArray>()) == ReleaseVerify::Result::BadSignature);
        // A key list accepts the signature when any key matches (key rotation)
        QList<QByteArray> rotated = otherKey();
        rotated.append(ReleaseVerify::trustedKeys());
        QVERIFY(ReleaseVerify::verifySignature(sums, sig, rotated) == ReleaseVerify::Result::Ok);
    }

    void malformedSignatureText()
    {
        using ReleaseVerify::Result;
        QVERIFY(ReleaseVerify::verifySignature(sums, "") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, "\n") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare.left(87)) == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare.left(86)) == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare + "A") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare + "\n\n") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, bare + "\r\n") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, " " + bare) == Result::Malformed);

        QByteArray badChar = bare;
        badChar[10] = '*';
        QVERIFY(ReleaseVerify::verifySignature(sums, badChar) == Result::Malformed);
        badChar[10] = '-';   // URL-safe alphabet is not accepted
        QVERIFY(ReleaseVerify::verifySignature(sums, badChar) == Result::Malformed);

        // Non-zero padding bits in the last character
        QByteArray bits = bare;
        bits[85] = 'B';
        QVERIFY(ReleaseVerify::verifySignature(sums, bits) == Result::Malformed);

        // 63 and 65 bytes of valid Base64, and no padding
        QVERIFY(ReleaseVerify::verifySignature(sums, QByteArray(84, 'A') + "A=") == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, QByteArray(86, 'A')) == Result::Malformed);
        QVERIFY(ReleaseVerify::verifySignature(sums, QByteArray(88, 'A')) == Result::Malformed);
    }

    void rfc8032VectorThroughTheKeyList()
    {
        // Empty message, RFC 8032 section 7.1 test 1
        QByteArray signature = "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw==";
        QVERIFY(ReleaseVerify::verifySignature(QByteArray(), signature, otherKey()) == ReleaseVerify::Result::Ok);
    }

    void parsesTheFixture()
    {
        QVector<ReleaseVerify::SumsEntry> entries;
        QVERIFY(ReleaseVerify::parseSums(sums, &entries) == ReleaseVerify::Result::Ok);
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries[1].name, emptyName);
        QCOMPARE(entries[1].sha256Hex, QString("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    void strictSumsFormat()
    {
        using ReleaseVerify::Result;
        const QByteArray hash(64, 'a');
        QVector<ReleaseVerify::SumsEntry> entries;
        QVERIFY(ReleaseVerify::parseSums(hash + "  file.bin\n", &entries) == Result::Ok);
        QVERIFY(ReleaseVerify::parseSums(hash + "  file.bin", &entries) == Result::Ok);   // missing final LF is tolerated
        QVERIFY(ReleaseVerify::parseSums("", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums("\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "  file.bin\n\n", &entries) == Result::Malformed);   // empty line
        QVERIFY(ReleaseVerify::parseSums(hash + " *file.bin\n", &entries) == Result::Malformed);   // binary mode marker
        QVERIFY(ReleaseVerify::parseSums(hash + " file.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "   file.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(QByteArray(64, 'A') + "  file.bin\n", &entries) == Result::Malformed);   // upper case hex
        QVERIFY(ReleaseVerify::parseSums(QByteArray(63, 'a') + "  file.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(QByteArray(65, 'a') + "  file.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(QByteArray(63, 'a') + "g  file.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "  file.bin\r\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "  \n", &entries) == Result::Malformed);   // no name
        QVERIFY(ReleaseVerify::parseSums(hash + "  file name.bin\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "  " + QByteArray(256, 'x') + "\n", &entries) == Result::Malformed);
        QVERIFY(ReleaseVerify::parseSums(hash + "  " + QByteArray(255, 'x') + "\n", &entries) == Result::Ok);
        QVERIFY(ReleaseVerify::parseSums(QByteArray(ReleaseVerify::MaxSumsSize + 1, 'a'), &entries) == Result::Malformed);
        QVERIFY(entries.isEmpty());
    }

    void pathLikeNamesAreMalformed()
    {
        using ReleaseVerify::Result;
        const QByteArray hash(64, 'b');
        QVector<ReleaseVerify::SumsEntry> entries;
        const QList<QByteArray> names = { "../evil.exe", "dir/file.bin", "dir\\file.bin", "/abs.bin", "C:file.bin",
                                          "..", ".", "a%2Fb", "file.bin:stream", "na\xC3\xAFve" };
        for (const QByteArray& name : names) {
            QVERIFY2(ReleaseVerify::parseSums(hash + "  " + name + "\n", &entries) == Result::Malformed, name.constData());
        }
        QVERIFY(ReleaseVerify::parseSums(hash + "  ...\n", &entries) == Result::Ok);
        QString hex;
        QVERIFY(ReleaseVerify::findHash(sums, "../" + installerName, &hex) == Result::NotListed);
        QVERIFY(ReleaseVerify::findHash(sums, "x/" + installerName, &hex) == Result::NotListed);
    }

    void duplicateAndMissingEntries()
    {
        using ReleaseVerify::Result;
        const QByteArray a(64, 'a');
        const QByteArray b(64, 'b');
        QString hex;
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n" + b + "  one.bin\n", "one.bin", &hex) == Result::Malformed);
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n" + a + "  one.bin\n", "one.bin", &hex) == Result::Malformed);
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n" + b + "  two.bin\n", "two.bin", &hex) == Result::Ok);
        QCOMPARE(hex, QString(b));
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n" + b + "  two.bin\n", "three.bin", &hex) == Result::NotListed);
        QVERIFY(hex.isEmpty());
        // Exact match only: no case folding, no prefix or suffix match
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n", "One.bin", &hex) == Result::NotListed);
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n", "one.bi", &hex) == Result::NotListed);
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n", "one.bin.", &hex) == Result::NotListed);
        QVERIFY(ReleaseVerify::findHash(a + "  one.bin\n", "", &hex) == Result::NotListed);
        QVERIFY(ReleaseVerify::findHash(sums, installerName, &hex) == Result::Ok);
        QCOMPARE(hex, QString("fb5c85338c609e0c9284185f942c7683275e2248086e5824611ef30f5ecd6a58"));
    }

    void fileMatchesSignedHash()
    {
        using ReleaseVerify::Result;
        QString path = write(installerName, "nyxplay fixture installer\n");
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, path) == Result::Ok);
        path = write(emptyName, "");
        QVERIFY(ReleaseVerify::verifyFile(sums, emptyName, path) == Result::Ok);
    }

    void hashMismatch()
    {
        using ReleaseVerify::Result;
        QString path = write(installerName, "nyxplay fixture installer\n\n");
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, path) == Result::HashMismatch);
        path = write(installerName, "Nyxplay fixture installer\n");
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, path) == Result::HashMismatch);
        path = write(installerName, "");
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, path) == Result::HashMismatch);
    }

    void notListedAndIoError()
    {
        using ReleaseVerify::Result;
        QString other = write("other.bin", "x");
        QVERIFY(ReleaseVerify::verifyFile(sums, "other.bin", other) == Result::NotListed);
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, work.filePath("does-not-exist.bin")) == Result::IoError);
        QVERIFY(ReleaseVerify::verifyFile(sums, installerName, work.path()) == Result::IoError);   // a directory
        QVERIFY(ReleaseVerify::verifyFile("garbage", installerName, other) == Result::Malformed);
        QString hex;
        QVERIFY(ReleaseVerify::hashFile(work.filePath("does-not-exist.bin"), &hex) == Result::IoError);
        QVERIFY(hex.isEmpty());
    }

    void largeFileIsStreamedInChunks()
    {
        using ReleaseVerify::Result;
        QByteArray data = largeContent();
        QString path = write(largeName, data);
        QVERIFY(ReleaseVerify::verifyFile(sums, largeName, path) == Result::Ok);
        // One changed byte near the end is detected
        data[data.size() - 5] = static_cast<char>(data[data.size() - 5] ^ 0x01);
        path = write(largeName, data);
        QVERIFY(ReleaseVerify::verifyFile(sums, largeName, path) == Result::HashMismatch);
    }

    void constantTimeCompare()
    {
        QVERIFY(ReleaseVerify::constantTimeEqual("abc", "abc"));
        QVERIFY(ReleaseVerify::constantTimeEqual("", ""));
        QVERIFY(!ReleaseVerify::constantTimeEqual("abc", "abd"));
        QVERIFY(!ReleaseVerify::constantTimeEqual("abc", "ab"));
        QVERIFY(!ReleaseVerify::constantTimeEqual("abc", "abcd"));
    }
};

template <typename T>
static int runSuite(int argc, char** argv)
{
    // Results are also written to a file when TEST_REPORT_DIR is set,
    // for environments where the console output is not captured.
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= runSuite<AppListParseTest>(argc, argv);
    status |= runSuite<NetworkErrorTest>(argc, argv);
    status |= runSuite<UuidTest>(argc, argv);
    status |= runSuite<IdentityStoreTest>(argc, argv);
    status |= runSuite<UpdateUrlTest>(argc, argv);
    status |= runSuite<UpdateLogicTest>(argc, argv);
    status |= runSuite<MoonlightImportTest>(argc, argv);
    status |= runSuite<ReleaseVerifyTest>(argc, argv);
    return status;
}

#include "tst_backend.moc"
