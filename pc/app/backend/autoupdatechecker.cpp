#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QSysInfo>
#include <QUrl>

#include "settings/streamingpreferences.h"
#include "updatelogic.h"

// NyxPlay publishes its own update manifest (same format as moonlight-stream.org/updates/qt.json).
// The mirror is only consulted when the primary location is unreachable or unusable.
#define NYX_UPDATE_MANIFEST_PRIMARY "https://raw.githubusercontent.com/siberanka/NyxPlay/main/pc/deploy/updates/qt.json"
#define NYX_UPDATE_MANIFEST_MIRROR "https://gitlab.com/siberanka/NyxPlay/-/raw/main/pc/deploy/updates/qt.json"

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent),
    m_Nam(nullptr),
    m_Reply(nullptr),
    m_NextUrl(0)
{
    QString currentVersion(VERSION_STR);
    qDebug() << "Current NyxPlay version:" << currentVersion;
    m_CurrentVersion = currentVersion;
}

void AutoUpdateChecker::start()
{
    if (m_Nam || m_Reply) {
        // Already running (or done)
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
    if (!StreamingPreferences::get()->checkForUpdates) {
        qDebug() << "Update checking is disabled in the settings";
        return;
    }

    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTPS redirects only
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

#ifdef NYX_UPDATE_TEST_PORT
    // Test builds only: read the manifest from a local test server instead
    m_Urls << QStringLiteral("http://127.0.0.1:%1/qt.json").arg(NYX_UPDATE_TEST_PORT);
#else
    m_Urls << QStringLiteral(NYX_UPDATE_MANIFEST_PRIMARY)
           << QStringLiteral(NYX_UPDATE_MANIFEST_MIRROR);
#endif
    m_NextUrl = 0;

    fetchNext();
#endif
}

QString AutoUpdateChecker::getPlatform()
{
#if defined(STEAM_LINK)
    return QStringLiteral("steamlink");
#elif defined(APP_IMAGE)
    return QStringLiteral("appimage");
#elif defined(Q_OS_DARWIN) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 changed this from 'osx' to 'macos'. Use the old one
    // to be consistent (and not require another entry in the manifest).
    return QStringLiteral("osx");
#else
    return QSysInfo::productType();
#endif
}

void AutoUpdateChecker::fetchNext()
{
    if (m_NextUrl >= m_Urls.count()) {
        finish();
        return;
    }

    QUrl url(m_Urls[m_NextUrl++]);
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    request.setTransferTimeout(15000);
    request.setMaximumRedirectsAllowed(3);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
    request.setMaximumRedirectsAllowed(3);
#endif

    m_Body.clear();
    m_Reply = m_Nam->get(request);
    connect(m_Reply, &QNetworkReply::readyRead, this, &AutoUpdateChecker::handleReadyRead);
    connect(m_Reply, &QNetworkReply::finished, this, &AutoUpdateChecker::handleFinished);
}

void AutoUpdateChecker::handleReadyRead()
{
    if (!m_Reply) {
        return;
    }

    // Never buffer more than the cap, whatever the server sends
    m_Body.append(m_Reply->read(UpdateLogic::MaxManifestBytes + 1 - m_Body.size()));
    if (m_Body.size() > UpdateLogic::MaxManifestBytes) {
        m_Reply->abort();
    }
}

void AutoUpdateChecker::handleFinished()
{
    QNetworkReply* reply = m_Reply;
    m_Reply = nullptr;
    if (!reply) {
        return;
    }

    const bool ok = reply->error() == QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    if (ok) {
        m_Body.append(reply->read(UpdateLogic::MaxManifestBytes + 1 - m_Body.size()));
    }
    else {
        qWarning() << "Update checking failed with error:" << reply->error();
    }
    reply->deleteLater();

    UpdateLogic::Result result;
    if (ok) {
        result = UpdateLogic::evaluateManifest(m_Body, m_CurrentVersion,
                                               QSysInfo::buildCpuArchitecture(),
                                               getPlatform(),
                                               QSysInfo::kernelVersion());
    }
    m_Body.clear();

    if (!ok || UpdateLogic::shouldTryFallback(result.status)) {
        if (ok) {
            qWarning() << "Update manifest unusable for" << QSysInfo::buildCpuArchitecture() << getPlatform();
        }
        fetchNext();
        return;
    }

    if (result.status == UpdateLogic::Status::UpdateAvailable) {
        qDebug() << "Update available:" << result.version;
        finish();
        emit onUpdateAvailable(result.version, result.url);
    }
    else {
        qDebug() << "Already running the latest version";
        finish();
    }
}

void AutoUpdateChecker::finish()
{
    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    if (m_Nam) {
        m_Nam->deleteLater();
        m_Nam = nullptr;
    }
}
