#pragma once

#include "identitymanager.h"
#include "nvapp.h"
#include "nvaddress.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class NvComputer;
struct NvHTTPTarget;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class GfeHttpResponseException : public std::exception
{
public:
    GfeHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.constData();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.constData();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_StatusMessage) + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QByteArray m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText) :
        m_Error(error),
        m_ErrorText(errorText.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.constData();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.constData();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_ErrorText) + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QByteArray m_ErrorText;
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 DEFAULT_MAX_RESPONSE_BYTES = 1024 * 1024;
    static constexpr qint64 MAX_APPLIST_BYTES = 8 * 1024 * 1024;
    static constexpr qint64 MAX_BOXART_BYTES = 8 * 1024 * 1024;
    static constexpr int MAX_APPLIST_ENTRIES = 5000;
    static constexpr int MAX_APP_NAME_LENGTH = 256;

    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(NvComputer* computer, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(const NvHTTPTarget& target, QNetworkAccessManager* nam);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel = NvLogLevel::NVLL_VERBOSE,
                           int retries = 0,
                           qint64 maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES);

    void setServerCert(QSslCertificate serverCert);
    void setAddress(NvAddress address);
    void setHttpsPort(uint16_t port);
    void setTrueUid(bool useTrueUid);

    NvAddress address();

    QSslCertificate serverCert();

    uint16_t httpPort();

    uint16_t httpsPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    quitApp();

    void
    startApp(QString verb,
             bool isGfe,
             int appId,
             PSTREAM_CONFIGURATION streamConfig,
             bool sops,
             bool localAudio,
             int gamepadMask,
             bool persistGameControllersOnDisconnect,
             QString& rtspSessionUrl);

    QVector<NvApp>
    getAppList();

    // Parses an applist response. Entries without a usable ID are skipped.
    // Throws if the response is not a successful, well-formed document.
    static
    QVector<NvApp>
    parseAppList(const QString& xml);

    // Returns a user-facing explanation for a socket error code
    static
    QString
    describeNetworkError(int errorCode);

    // Returns true for socket errors that mean the peer dropped an established connection
    static
    bool
    isConnectionResetError(int errorCode);

    QImage
    getBoxArt(int appId);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    QUrl m_BaseUrlHttp;
    QUrl m_BaseUrlHttps;
private:
    void
    handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

    QNetworkReply*
    openConnection(QUrl baseUrl,
                   QString command,
                   QString arguments,
                   int timeoutMs,
                   NvLogLevel logLevel,
                   int retries = 0,
                   qint64 maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES);

    QNetworkReply*
    openConnectionOnce(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel,
                       qint64 maxResponseBytes,
                       bool* receivedData);

    NvAddress m_Address;
    QNetworkAccessManager* m_Nam;
    QSslCertificate m_ServerCert;
    bool m_UseTrueUid;
};
