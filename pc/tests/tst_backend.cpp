#include <QtTest>

#include "backend/nvhttp.h"
#include "backend/nvcomputer.h"

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
    return status;
}

#include "tst_backend.moc"
