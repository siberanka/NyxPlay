#include <QtTest>

#include "backend/nvhttp.h"
#include "backend/nvcomputer.h"
#include "backend/identitystore.h"
#include "backend/moonlightimport.h"
#include "backend/updateurl.h"

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
    status |= runSuite<MoonlightImportTest>(argc, argv);
    return status;
}

#include "tst_backend.moc"
