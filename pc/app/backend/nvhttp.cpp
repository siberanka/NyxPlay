#include "nvcomputer.h"
#include <Limelight.h>

#include <QDebug>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QtEndian>
#include <QNetworkProxy>
#include <QElapsedTimer>
#include <QReadLocker>
#include <QCoreApplication>

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000
#define QUIT_TIMEOUT_MS 30000
#define IDEMPOTENT_RETRY_DELAY_MS 150
#define MAX_IDEMPOTENT_RETRIES 2
#define MAX_BOXART_DIMENSION 4096
#define BOXART_ALLOCATION_LIMIT_MB 64

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid, QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this)),
    m_ServerCert(serverCert),
    m_UseTrueUid(useTrueUid)
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

// Reads the connection parameters of a computer under its lock, because
// polling threads may update them while a request is being prepared.
struct NvHTTPTarget
{
    explicit NvHTTPTarget(NvComputer* computer)
    {
        QReadLocker lock(&computer->lock);
        address = computer->activeAddress;
        httpsPort = computer->activeHttpsPort;
        serverCert = computer->serverCert;
        useTrueUid = !computer->isNvidiaServerSoftware;
    }

    NvAddress address;
    uint16_t httpsPort;
    QSslCertificate serverCert;
    bool useTrueUid;
};

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(NvHTTPTarget(computer), nam)
{
}

NvHTTP::NvHTTP(const NvHTTPTarget& target, QNetworkAccessManager* nam) :
    NvHTTP(target.address, target.httpsPort, target.serverCert, target.useTrueUid, nam)
{
}

void NvHTTP::setServerCert(QSslCertificate serverCert)
{
    m_ServerCert = serverCert;
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

void NvHTTP::setTrueUid(bool useTrueUid)
{
    m_UseTrueUid = useTrueUid;
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

QSslCertificate NvHTTP::serverCert()
{
    return m_ServerCert;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    QString serverInfo;

    // Check if we have a pinned cert and HTTPS port for this host yet
    if (!m_ServerCert.isNull() && httpsPort() != 0)
    {
        try
        {
            // Always try HTTPS first, since it properly reports
            // pairing status (and a few other attributes).
            serverInfo = openConnectionToString(m_BaseUrlHttps,
                                                "serverinfo",
                                                nullptr,
                                                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                logLevel,
                                                fastFail ? 1 : MAX_IDEMPOTENT_RETRIES);
            // Throws if the request failed
            verifyResponseStatus(serverInfo);
        }
        catch (const GfeHttpResponseException& e)
        {
            if (e.getStatusCode() == 401)
            {
                // Certificate validation error, fallback to HTTP
                serverInfo = openConnectionToString(m_BaseUrlHttp,
                                                    "serverinfo",
                                                    nullptr,
                                                    fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                    logLevel,
                                                    fastFail ? 1 : MAX_IDEMPOTENT_RETRIES);
                verifyResponseStatus(serverInfo);
            }
            else
            {
                // Rethrow real errors
                throw e;
            }
        }
    }
    else
    {
        // Only use HTTP prior to pairing or fetching HTTPS port
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel,
                                            fastFail ? 1 : MAX_IDEMPOTENT_RETRIES);
        verifyResponseStatus(serverInfo);

        // Populate the HTTPS port
        uint16_t httpsPort = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (httpsPort == 0) {
            httpsPort = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(httpsPort);

        // If we just needed to determine the HTTPS port, we'll try again over
        // HTTPS now that we have the port number
        if (!m_ServerCert.isNull()) {
            return getServerInfo(logLevel, fastFail);
        }
    }

    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 bool isGfe,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool sops,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString& rtspSessionUrl)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   // Using an FPS value over 60 causes SOPS to default to 720p60,
                                   // so force it to 0 to ensure the correct resolution is set. We
                                   // used to use 60 here but that locked the frame rate to 60 FPS
                                   // on GFE 3.20.3. We don't need this hack for Sunshine.
                                   QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps)+
                                   "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                                   "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                                   "&rikeyid="+QString::number(riKeyId)+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   // NyxPlay: opt in to host text input focus notifications (diagnostics)
                                   (qEnvironmentVariableIntValue("NYXPLAY_TEXT_INPUT_EVENTS") ? "&nyxTextInput=1" : "")+
                                   LiGetLaunchUrlQueryParameters(),
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

void
NvHTTP::quitApp()
{
    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   "cancel",
                                   nullptr,
                                   QUIT_TIMEOUT_MS);

    qInfo() << "Quit response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    // Newer GFE versions will just return success even if quitting fails
    // if we're not the original requester.
    if (getCurrentGame(getServerInfo(NvHTTP::NVLL_ERROR)) != 0) {
        // Generate a synthetic GfeResponseException letting the caller know
        // that they can't kill someone else's stream.
        throw GfeHttpResponseException(599, "");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (!modes.isEmpty()) {
                if (XML_NAME_EQUALS(name, "Width")) {
                    modes.last().width = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "Height")) {
                    modes.last().height = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "RefreshRate")) {
                    modes.last().refreshRate = xmlReader.readElementText().toInt();
                }
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR,
                                            MAX_IDEMPOTENT_RETRIES,
                                            MAX_APPLIST_BYTES);
    return parseAppList(appxml);
}

QVector<NvApp>
NvHTTP::parseAppList(const QString& appxml)
{
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    NvApp current;
    bool inApp = false;
    int skipped = 0;

    // Completes the entry being parsed. Entries without a usable ID cannot be
    // launched, so they are dropped without affecting the rest of the list.
    auto finishApp = [&]() {
        if (!inApp) {
            return;
        }
        inApp = false;

        if (current.id <= 0) {
            skipped++;
            return;
        }
        if (apps.size() >= MAX_APPLIST_ENTRIES) {
            skipped++;
            return;
        }
        if (current.name.isEmpty()) {
            current.name = tr("App %1").arg(current.id);
        }
        apps.append(current);
    };

    while (!xmlReader.atEnd()) {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }

        auto name = xmlReader.name();
        if (XML_NAME_EQUALS(name, "App")) {
            finishApp();
            current = NvApp();
            inApp = true;
        }
        else if (inApp) {
            if (XML_NAME_EQUALS(name, "AppTitle")) {
                // An app without a name may be sent as <AppTitle/>
                QString title = xmlReader.readElementText();
                if (title.size() > MAX_APP_NAME_LENGTH) {
                    title.truncate(MAX_APP_NAME_LENGTH);
                }
                current.name = title.isNull() ? QString("") : title;
            }
            else if (XML_NAME_EQUALS(name, "ID")) {
                bool ok = false;
                int id = xmlReader.readElementText().toInt(&ok);
                current.id = ok ? id : 0;
            }
            else if (XML_NAME_EQUALS(name, "IsHdrSupported")) {
                current.hdrSupported = xmlReader.readElementText() == "1";
            }
            else if (XML_NAME_EQUALS(name, "IsAppCollectorGame")) {
                current.isAppCollectorGame = xmlReader.readElementText() == "1";
            }
        }
    }
    finishApp();

    // A damaged document must not replace a good cached list with a partial one
    if (xmlReader.hasError()) {
        qWarning() << "Invalid applist XML:" << xmlReader.errorString();
        throw std::runtime_error("Invalid applist XML");
    }

    if (skipped > 0) {
        qWarning() << "Ignored" << skipped << "invalid entries in the app list";
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (XML_NAME_EQUALS(xmlReader.name(), "root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected for unpaired PCs when we fetch serverinfo over HTTPS
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE,
                                          MAX_IDEMPOTENT_RETRIES,
                                          MAX_BOXART_BYTES);

    QImage image;
    {
        QImageReader reader(reply);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Bound the memory a decoded image may use
        QImageReader::setAllocationLimit(BOXART_ALLOCATION_LIMIT_MB);
#endif
        QSize size = reader.size();
        if (!size.isValid() || (size.width() <= MAX_BOXART_DIMENSION && size.height() <= MAX_BOXART_DIMENSION)) {
            image = reader.read();
            if (image.width() > MAX_BOXART_DIMENSION || image.height() > MAX_BOXART_DIMENSION) {
                image = QImage();
            }
        }
        else {
            qWarning() << "Ignoring oversized box art for app" << appId;
        }
    }
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    return QByteArray::fromHex(getXmlString(xml, tagName).toUtf8());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return QString();
}

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    bool ignoreErrors = true;

    if (m_ServerCert.isNull()) {
        // We should never make an HTTPS request without a cert
        Q_ASSERT(!m_ServerCert.isNull());
        return;
    }

    for (const QSslError& error : errors) {
        if (m_ServerCert != error.certificate()) {
            ignoreErrors = false;
            break;
        }
    }

    if (ignoreErrors) {
        reply->ignoreSslErrors(errors);
    }
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel,
                               int retries,
                               qint64 maxResponseBytes)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel, retries, maxResponseBytes);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

bool
NvHTTP::isConnectionResetError(int errorCode)
{
    switch (errorCode) {
    case 10054: // WSAECONNRESET
    case 10053: // WSAECONNABORTED
    case 104:   // ECONNRESET (Linux)
    case 103:   // ECONNABORTED (Linux)
    case 54:    // ECONNRESET (BSD)
    case 53:    // ECONNABORTED (BSD)
        return true;
    default:
        return false;
    }
}

QString
NvHTTP::describeNetworkError(int errorCode)
{
    if (isConnectionResetError(errorCode)) {
        return tr("The host closed the connection unexpectedly. Try again; if it keeps happening, restart the host.");
    }

    switch (errorCode) {
    case 10060: // WSAETIMEDOUT
    case 10061: // WSAECONNREFUSED
    case 10065: // WSAEHOSTUNREACH
    case 110:   // ETIMEDOUT (Linux)
    case 111:   // ECONNREFUSED (Linux)
    case 113:   // EHOSTUNREACH (Linux)
    case 60:    // ETIMEDOUT (BSD)
    case 61:    // ECONNREFUSED (BSD)
        return tr("The host could not be reached. Check your firewall and port forwarding rules.");
    default:
        return tr("Network error %1").arg(errorCode);
    }
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel,
                       int retries,
                       qint64 maxResponseBytes)
{
    QElapsedTimer elapsed;
    elapsed.start();

    for (int attempt = 0;; attempt++) {
        bool receivedData = false;

        try {
            return openConnectionOnce(baseUrl, command, arguments, timeoutMs, logLevel, maxResponseBytes, &receivedData);
        }
        catch (const QtNetworkReplyException& e) {
            // Only repeat requests that failed because the connection was dropped
            // before any part of the response arrived.
            bool retryableError =
                    e.getError() == QNetworkReply::RemoteHostClosedError ||
                    e.getError() == QNetworkReply::ConnectionRefusedError ||
                    e.getError() == QNetworkReply::UnknownNetworkError ||
                    e.getError() == QNetworkReply::TemporaryNetworkFailureError;
            bool withinBudget = timeoutMs <= 0 || elapsed.elapsed() + IDEMPOTENT_RETRY_DELAY_MS < timeoutMs;
            if (attempt >= retries || receivedData || !retryableError || !withinBudget) {
                throw;
            }

            if (logLevel >= NvLogLevel::NVLL_ERROR) {
                qWarning() << command << "request retry" << (attempt + 1) << "after error" << e.getError();
            }

            // Wait briefly, but give up immediately if the application is quitting
            bool quitting = false;
            QEventLoop delayLoop;
            auto quitConnection = connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &delayLoop, [&]() {
                quitting = true;
                delayLoop.quit();
            });
            QTimer::singleShot(IDEMPOTENT_RETRY_DELAY_MS, &delayLoop, &QEventLoop::quit);
            delayLoop.exec(QEventLoop::ExcludeUserInputEvents);
            disconnect(quitConnection);
            if (quitting) {
                throw;
            }
        }
    }
}

QNetworkReply*
NvHTTP::openConnectionOnce(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel,
                           qint64 maxResponseBytes,
                           bool* receivedData)
{
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a placeholder UID for GFE allow them to quit games for each other.
    url.setQuery("uniqueid=" + (m_UseTrueUid ? IdentityManager::get()->getUniqueId() : "0123456789ABCDEF") +
                 "&uuid=" + QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request(url);

    // Add our client certificate
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif


    // Use fine-grained idle timeouts to avoid calling QNetworkAccessManager::clearAccessCache(),
    // which tears down the NAM's global thread each time. We must not keep persistent connections
    // or GFE will puke.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, -1);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    auto sslErrorsConnection = connect(m_Nam, &QNetworkAccessManager::sslErrors, this, &NvHTTP::handleSslErrors);
    QNetworkReply* reply = m_Nam->get(request);
    QEventLoop loop;

    // Refuse responses larger than the allowed size
    bool tooLarge = false;
    auto readyReadConnection = connect(reply, &QNetworkReply::readyRead, reply, [&]() {
        *receivedData = true;
        if (reply->bytesAvailable() > maxResponseBytes) {
            tooLarge = true;
            reply->abort();
        }
    });

    // Connect the quit signals
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);

    // Enforce a transfer timeout if requested
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Time the request to distinguish a slow response from an unreachable host
    auto* connectTimer = new QTimer(reply);
    connectTimer->setSingleShot(true);
    connect(connectTimer, &QTimer::timeout, reply, [reply, logLevel, url]() {
        if (!reply->isFinished()) {
            if (logLevel >= NvLogLevel::NVLL_ERROR) {
                qWarning() << "Aborting timed out connection for" << url.toString();
            }

            reply->setProperty("timeout_reason", "connection_timeout");
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::requestSent, connectTimer, &QTimer::stop);
    connectTimer->start(REQUEST_TIMEOUT_MS);
#endif

    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    // If we couldn't use fine-grained connection idle timeouts, kill them all now
    m_Nam->clearAccessCache();
#endif
    disconnect(sslErrorsConnection);
    disconnect(readyReadConnection);
    if (reply->bytesAvailable() > 0) {
        *receivedData = true;
    }

    if (tooLarge) {
        qWarning() << command << "response exceeded the size limit";
        delete reply;
        throw QtNetworkReplyException(QNetworkReply::UnknownContentError, tr("Response too large"));
    }

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            // This will trigger falling back to HTTP for the serverinfo query
            // then pairing again to get the updated certificate.
            GfeHttpResponseException exception(401, "Server certificate mismatch");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QString reason = reply->property("timeout_reason").toString();
            QtNetworkReplyException exception(QNetworkReply::TimeoutError,
                                              reason == "connection_timeout" ?
                                                  "Connection timed out" :
                                                  "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(reply->error(),
                                              reply->error() == QNetworkReply::RemoteHostClosedError ?
                                                  tr("The host closed the connection unexpectedly. Try again; if it keeps happening, restart the host.") :
                                                  reply->errorString());
            delete reply;
            throw exception;
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // The requestSent signal must have stopped the timer for any successful request,
    // otherwise we could falsely time out a slow response when we connected fine.
    Q_ASSERT(!connectTimer->isActive());
#endif

    return reply;
}
