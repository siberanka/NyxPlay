#include "updateinstaller.h"

#include "releaseverify.h"
#include "updateurl.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStorageInfo>
#include <QThread>
#include <QtDebug>

#ifdef Q_OS_WIN
#include "updatestage.h"
#endif

namespace {

constexpr qint64 SpareBytes = 128LL * 1024 * 1024;

const char* const UpdaterName = "NyxPlayUpdater.exe";

bool isAllowedHost(const QUrl& url)
{
    if (url.scheme() != QLatin1String("https") || url.port() != -1 || !url.userInfo().isEmpty()) {
        return false;
    }
    const QString host = url.host();
    return host == QLatin1String("github.com") || host == QLatin1String("gitlab.com") ||
           host == QLatin1String("objects.githubusercontent.com") ||
           host == QLatin1String("release-assets.githubusercontent.com");
}

// User data and the work folder are never searched for leftovers
bool isUserDataDir(const QString& name)
{
    const QString lower = name.toLower();
    return lower == QLatin1String("nyxplay") || lower == QLatin1String("cache") || lower == QLatin1String("boxart") ||
           lower == QLatin1String("qmlcache") || lower == QLatin1String(".nyxupdate");
}

void removeBackups(const QDir& dir, bool root)
{
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo& info : entries) {
        if (info.isDir()) {
            if (!(root && isUserDataDir(info.fileName()))) {
                removeBackups(QDir(info.absoluteFilePath()), false);
            }
        }
        else if (info.fileName().endsWith(QLatin1String(".nyxold"), Qt::CaseInsensitive)) {
            if (!QFile::remove(info.absoluteFilePath())) {
                qWarning() << "Could not remove" << info.fileName();
            }
        }
    }
}

}

UpdateInstaller::UpdateInstaller(const QString& installDir, const QString& updateDir, const QString& currentVersion,
                                 QObject* checker, QNetworkAccessManager* nam, QObject* parent) :
    QObject(parent),
    m_InstallDir(QDir::cleanPath(installDir)),
    m_UpdateDir(QDir::cleanPath(updateDir)),
    m_CurrentVersion(currentVersion),
    m_Checker(checker),
    m_Nam(nam)
{
    m_StreamTimer.setInterval(2000);
    connect(&m_StreamTimer, &QTimer::timeout, this, &UpdateInstaller::pollStream);

    if (m_Checker) {
        // The checker is an AutoUpdateChecker; it is only known by its signals here
        connect(m_Checker, SIGNAL(updateFound(UpdateLogic::Result)), this, SLOT(offer(UpdateLogic::Result)));
        connect(m_Checker, SIGNAL(checkFinished(bool,bool)), this, SLOT(handleCheckFinished(bool,bool)));
    }
}

void UpdateInstaller::handleCheckFinished(bool available, bool failed)
{
    if (m_ExplicitCheck) {
        m_ExplicitCheck = false;
        if (!available) {
            m_InstallAfterCheck = false;
        }
        emit checkReported(available, failed);
    }
}

bool UpdateInstaller::canInstall() const
{
#ifdef Q_OS_WIN
    return m_Offer.status == UpdateLogic::Status::UpdateAvailable && m_Offer.package.valid();
#else
    return false;
#endif
}

bool UpdateInstaller::streamActive() const
{
    return m_StreamProbe && m_StreamProbe();
}

bool UpdateInstaller::isFolderWritable() const
{
#ifdef Q_OS_WIN
    return UpdateStage::isWritable(m_InstallDir);
#else
    return false;
#endif
}

void UpdateInstaller::setState(State state, Error error, const QString& detail)
{
    m_State = state;
    m_Error = error;
    m_ErrorDetail = detail;
    if (state == WaitingForStream) {
        m_StreamTimer.start();
    }
    else {
        m_StreamTimer.stop();
    }
    emit stateChanged();
}

void UpdateInstaller::fail(Error error, const QString& detail)
{
    qWarning() << "Update failed:" << error << detail;
    m_StagedPending = false;
    m_InstallAfterCheck = false;
    resetWork();
    m_Lock.reset();
    setState(Failed, error, detail);
}

void UpdateInstaller::resetWork()
{
    abortFetch();
    m_Sums.clear();
    if (!m_ZipPath.isEmpty()) {
        QFile::remove(m_ZipPath);
    }
    m_ZipPath.clear();
#ifdef Q_OS_WIN
    QDir(UpdateStage::stageDir(m_InstallDir)).removeRecursively();
#endif
}

void UpdateInstaller::offer(const UpdateLogic::Result& result)
{
    if (result.status != UpdateLogic::Status::UpdateAvailable) {
        return;
    }
    // A running download or a staged update keeps the offer it was started with
    if (m_State >= WaitingForStream && m_State != Failed) {
        return;
    }
    m_Offer = result;
    emit offerChanged();
    setState(Available);

    if (m_InstallAfterCheck) {
        m_InstallAfterCheck = false;
        install();
    }
}

void UpdateInstaller::checkNow(bool thenInstall)
{
    if (m_State >= WaitingForStream && m_State != Failed) {
        return;
    }
    if (thenInstall && m_State == Available) {
        install();
        return;
    }
    m_ExplicitCheck = true;
    m_InstallAfterCheck = thenInstall;
    if (m_Checker) {
        QMetaObject::invokeMethod(m_Checker, "start", Q_ARG(bool, true));
    }
}

void UpdateInstaller::install()
{
    if (m_State != Available && m_State != Failed) {
        return;
    }
    if (!canInstall()) {
        fail(NotInstallable);
        return;
    }
    qInfo() << "Installing update" << m_Offer.version;
    m_StagedPending = false;
    if (streamActive()) {
        qInfo() << "A stream is running; the update waits for it to end";
        setState(WaitingForStream);
        return;
    }
    begin();
}

void UpdateInstaller::pollStream()
{
    if (m_State != WaitingForStream || streamActive()) {
        return;
    }
    if (m_StagedPending) {
        setState(ReadyToRestart);
    }
    else {
        begin();
    }
}

void UpdateInstaller::cancel()
{
    if (m_State != Downloading && m_State != WaitingForStream) {
        return;
    }
    qInfo() << "Update cancelled";
    m_Generation++;
    m_StagedPending = false;
    resetWork();
    m_Lock.reset();
    m_Progress = 0;
    emit progressChanged();
    setState(Available);
}

void UpdateInstaller::dismissError()
{
    if (m_State == Failed) {
        setState(m_Offer.status == UpdateLogic::Status::UpdateAvailable ? Available : Idle);
    }
}

void UpdateInstaller::clearLastResult()
{
    if (!m_LastResult.isEmpty()) {
        m_LastResult.clear();
        emit lastResultChanged();
    }
}

// ---- the download steps ---------------------------------------------------------------------

QList<QUrl> UpdateInstaller::candidates(const QString& fileName) const
{
    QList<QUrl> urls;
    const UpdateLogic::Package& package = m_Offer.package;
    urls << UpdateLogic::releaseFileUrl(package, fileName);
    if (package.assetUrl.host() == QLatin1String("github.com")) {
        // The mirror serves the same release; the signature covers every byte either way
        urls << QUrl(QStringLiteral("https://gitlab.com/siberanka/NyxPlay/-/releases/%1/downloads/%2")
                     .arg(package.tag, fileName));
    }
    return urls;
}

void UpdateInstaller::begin()
{
    m_Generation++;
    resetWork();
    m_Progress = 0;
    emit progressChanged();

    if (!isFolderWritable()) {
        fail(NotWritable, m_InstallDir);
        return;
    }
#ifdef Q_OS_WIN
    // Only one window per folder may stage an update, otherwise two extractions would overwrite each other
    if (!m_Lock) {
        QDir().mkpath(UpdateStage::workDir(m_InstallDir));
        auto lock = std::make_unique<QLockFile>(QDir(UpdateStage::workDir(m_InstallDir)).filePath(QStringLiteral("update.lock")));
        lock->setStaleLockTime(0);   // a lock is stale when its process is gone, however long ago it was taken
        if (!lock->tryLock(0)) {
            fail(Busy);
            return;
        }
        m_Lock = std::move(lock);
    }
#endif
    if (!QDir().mkpath(m_UpdateDir)) {
        fail(DownloadFailed, QStringLiteral("work folder"));
        return;
    }
    if (QStorageInfo(m_UpdateDir).bytesAvailable() < m_Offer.package.assetSize + SpareBytes) {
        fail(NoSpace);
        return;
    }

    setState(Downloading);
    const int generation = m_Generation;
    fetch(candidates(QStringLiteral("SHA256SUMS.txt")), ReleaseVerify::MaxSumsSize, QString(),
          [this, generation](bool ok, const QByteArray& data) {
        if (generation != m_Generation) {
            return;
        }
        if (!ok) {
            fail(m_NetworkFault ? NoConnection : DownloadFailed);
            return;
        }
        m_Sums = data;
        signatureStep();
    });
}

void UpdateInstaller::signatureStep()
{
    const int generation = m_Generation;
    fetch(candidates(QStringLiteral("SHA256SUMS.txt.sig")), 1024, QString(),
          [this, generation](bool ok, const QByteArray& signature) {
        if (generation != m_Generation) {
            return;
        }
        if (!ok) {
            fail(m_NetworkFault ? NoConnection : DownloadFailed);
            return;
        }

        const ReleaseVerify::Result verified = m_Keys.isEmpty()
                ? ReleaseVerify::verifySignature(m_Sums, signature)
                : ReleaseVerify::verifySignature(m_Sums, signature, m_Keys);
        QString hash;
        if (verified != ReleaseVerify::Result::Ok ||
                ReleaseVerify::findHash(m_Sums, m_Offer.package.assetName, &hash) != ReleaseVerify::Result::Ok) {
            qWarning() << "Release signature check failed:" << ReleaseVerify::resultName(verified);
            fail(VerifyFailed, QStringLiteral("signature"));
            return;
        }
        qInfo() << "Release signature verified";
        downloadPackage();
    });
}

void UpdateInstaller::downloadPackage()
{
    const int generation = m_Generation;
    m_ZipPath = QDir(m_UpdateDir).filePath(m_Offer.package.assetName + QStringLiteral(".part"));
    fetch(candidates(m_Offer.package.assetName), m_Offer.package.assetSize, m_ZipPath,
          [this, generation](bool ok, const QByteArray&) {
        if (generation != m_Generation) {
            return;
        }
        if (!ok) {
            fail(m_NetworkFault ? NoConnection : DownloadFailed);
            return;
        }
        prepareStep();
    });
}

void UpdateInstaller::fetch(const QList<QUrl>& urls, qint64 limit, const QString& filePath,
                            std::function<void(bool, const QByteArray&)> done)
{
    m_Urls = urls;
    m_UrlIndex = 0;
    m_Limit = limit;
    m_FilePath = filePath;
    m_Done = std::move(done);
    m_NetworkFault = false;
    fetchNext();
}

void UpdateInstaller::fetchNext()
{
    if (m_UrlIndex >= m_Urls.size()) {
        finishFetch(false, nullptr);
        return;
    }
    const QUrl url = m_Urls.at(m_UrlIndex++);
    if (!isAllowedHost(url)) {
        fetchNext();
        return;
    }
    if (!m_Nam) {
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setStrictTransportSecurityEnabled(true);
    }

    delete m_File;
    m_File = nullptr;
    if (!m_FilePath.isEmpty()) {
        m_File = new QFile(m_FilePath, this);
        if (!m_File->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete m_File;
            m_File = nullptr;
            finishFetch(false, nullptr);
            return;
        }
    }
    m_Memory.clear();
    m_Received = 0;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("NyxPlay/%1").arg(m_CurrentVersion));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QNetworkReply* reply = m_Nam->get(request);
    m_Reply = reply;
    connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl& target) {
        if (isAllowedHost(target)) {
            emit reply->redirectAllowed();
        }
        else {
            qWarning() << "Refusing a redirect to" << target.host();
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (reply != m_Reply) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        m_Received += chunk.size();
        if (m_Received > m_Limit) {
            reply->abort();
            return;
        }
        if (m_File) {
            if (m_File->write(chunk) != chunk.size()) {
                reply->abort();
            }
        }
        else {
            m_Memory.append(chunk);
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64) {
        if (reply != m_Reply || !m_File || m_Offer.package.assetSize <= 0) {
            return;
        }
        const int percent = int(qMin<qint64>(100, received * 100 / m_Offer.package.assetSize));
        if (percent != m_Progress) {
            m_Progress = percent;
            emit progressChanged();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply != m_Reply) {
            reply->deleteLater();
            return;
        }
        // The rest of the body that has not been read yet
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            m_Received += tail.size();
            if (m_File) {
                m_File->write(tail);
            }
            else {
                m_Memory.append(tail);
            }
        }
        const bool exact = !m_File || m_Received == m_Limit;
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                        m_Received <= m_Limit && exact;
        if (!ok) {
            switch (reply->error()) {
            case QNetworkReply::ConnectionRefusedError:
            case QNetworkReply::RemoteHostClosedError:
            case QNetworkReply::HostNotFoundError:
            case QNetworkReply::TimeoutError:
            case QNetworkReply::NetworkSessionFailedError:
            case QNetworkReply::TemporaryNetworkFailureError:
            case QNetworkReply::UnknownNetworkError:
                m_NetworkFault = true;
                break;
            default:
                break;
            }
            qWarning() << "Update download failed:" << reply->error()
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        }
        finishFetch(ok, reply);
    });
}

void UpdateInstaller::finishFetch(bool ok, QNetworkReply* reply)
{
    if (reply) {
        m_Reply = nullptr;
        reply->deleteLater();
    }
    if (m_File) {
        m_File->close();
        delete m_File;
        m_File = nullptr;
    }
    if (!ok && reply && m_UrlIndex < m_Urls.size()) {
        // Try the mirror
        fetchNext();
        return;
    }
    if (!ok && !m_FilePath.isEmpty()) {
        QFile::remove(m_FilePath);
    }
    auto done = std::move(m_Done);
    m_Done = nullptr;
    const QByteArray data = std::move(m_Memory);
    m_Memory.clear();
    if (done) {
        done(ok, data);
    }
}

void UpdateInstaller::abortFetch()
{
    m_Done = nullptr;
    if (m_Reply) {
        QNetworkReply* reply = m_Reply;
        m_Reply = nullptr;
        reply->disconnect(this);
        reply->disconnect();
        reply->abort();
        reply->deleteLater();
    }
    if (m_File) {
        m_File->close();
        delete m_File;
        m_File = nullptr;
    }
    m_Urls.clear();
    m_UrlIndex = 0;
}

// ---- verification and staging ---------------------------------------------------------------

void UpdateInstaller::prepareStep()
{
#ifdef Q_OS_WIN
    setState(Verifying);

    const int generation = m_Generation;
    const QString zipPath = m_ZipPath;
    const QString stage = UpdateStage::stageDir(m_InstallDir);
    const QString installDir = m_InstallDir;
    const QString version = m_Offer.version;
    const qint64 size = m_Offer.package.assetSize;
    const quint16 machine = m_Machine;
    QString signedHash;
    ReleaseVerify::findHash(m_Sums, m_Offer.package.assetName, &signedHash);

    QThread* worker = QThread::create([this, generation, zipPath, stage, installDir, version, size, machine, signedHash]() {
        auto report = [this, generation](bool ok, int error, const QString& detail) {
            QMetaObject::invokeMethod(this, [this, generation, ok, error, detail]() {
                if (generation == m_Generation) {
                    finishPrepare(ok, error, detail);
                }
            }, Qt::QueuedConnection);
        };

        // From here on only the bytes that were hashed are used, so the file on disk cannot be swapped in between
        QByteArray data;
        {
            QFile file(zipPath);
            if (!file.open(QIODevice::ReadOnly) || file.size() != size) {
                report(false, VerifyFailed, QStringLiteral("size"));
                return;
            }
            data = file.readAll();
        }
        QFile::remove(zipPath);

        const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
        if (data.size() != size || !ReleaseVerify::constantTimeEqual(hash, signedHash.toLatin1())) {
            report(false, VerifyFailed, QStringLiteral("hash"));
            return;
        }

        QString why;
        const qint64 unpacked = UpdateStage::unpackedSize(data, &why);
        if (unpacked < 0) {
            report(false, PackageInvalid, why);
            return;
        }
        if (QStorageInfo(installDir).bytesAvailable() < unpacked + SpareBytes) {
            report(false, NoSpace, QString());
            return;
        }

        QMetaObject::invokeMethod(this, [this, generation]() {
            if (generation == m_Generation) {
                setState(Extracting);
            }
        }, Qt::QueuedConnection);

        QDir(stage).removeRecursively();
        if (!UpdateStage::extractPackage(data, stage, &why) ||
                !UpdateStage::checkStage(stage, version, machine, &why)) {
            QDir(stage).removeRecursively();
            report(false, PackageInvalid, why);
            return;
        }
        report(true, NoError, QString());
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
#else
    fail(NotInstallable);
#endif
}

void UpdateInstaller::finishPrepare(bool ok, int error, const QString& detail)
{
    if (!ok) {
        fail(Error(error), detail);
        return;
    }
    qInfo() << "Update" << m_Offer.version << "is verified and staged";
    m_StagedPending = true;
    m_Progress = 100;
    emit progressChanged();
    if (streamActive()) {
        setState(WaitingForStream);
    }
    else {
        setState(ReadyToRestart);
    }
}

// ---- hand-over to the updater ---------------------------------------------------------------

bool UpdateInstaller::launchUpdater()
{
#ifdef Q_OS_WIN
    const QString stage = UpdateStage::stageDir(m_InstallDir);
    const QString runDir = QDir(m_UpdateDir).filePath(QStringLiteral("run"));
    QDir(runDir).removeRecursively();
    if (!QDir().mkpath(runDir)) {
        return false;
    }
    // The updater runs from a private copy, so the files in the install folder (including the updater itself) can be replaced
    const QString copy = QDir(runDir).filePath(QLatin1String(UpdaterName));
    if (!QFile::copy(QDir(stage).filePath(QLatin1String(UpdaterName)), copy)) {
        return false;
    }

    const QStringList args = {
        QStringLiteral("--wait-pid"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--install-dir"), QDir::toNativeSeparators(m_InstallDir),
        QStringLiteral("--stage-dir"), QDir::toNativeSeparators(stage),
        QStringLiteral("--result-file"), QDir::toNativeSeparators(QDir(m_UpdateDir).filePath(QStringLiteral("result.txt"))),
        QStringLiteral("--version"), m_Offer.version
    };
#ifdef NYX_UNIT_TESTS
    if (m_LaunchHook) {
        return m_LaunchHook(copy, args);
    }
#endif
    return QProcess::startDetached(copy, args, runDir);
#else
    return false;
#endif
}

void UpdateInstaller::restartNow()
{
    if (m_State != ReadyToRestart) {
        return;
    }
    if (streamActive()) {
        setState(WaitingForStream);
        return;
    }
    qInfo() << "Starting the updater for" << m_Offer.version;
    if (!launchUpdater()) {
        fail(LaunchFailed);
        return;
    }
    setState(Restarting);
#ifdef NYX_UNIT_TESTS
    if (m_LaunchHook) {
        return;
    }
#endif
    QCoreApplication::quit();
}

// ---- start-up -------------------------------------------------------------------------------

void UpdateInstaller::startupMaintenance()
{
    // Outcome of the previous update, written by the updater
    const QString resultPath = QDir(m_UpdateDir).filePath(QStringLiteral("result.txt"));
    QFile resultFile(resultPath);
    if (resultFile.exists() && resultFile.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = resultFile.read(8192).split('\n');
        resultFile.close();
        const QString status = lines.isEmpty() ? QString() : QString::fromUtf8(lines.first()).trimmed();
        for (int i = 1; i < lines.size() && i < 64; i++) {
            if (!lines.at(i).trimmed().isEmpty()) {
                qInfo() << "Updater:" << QString::fromUtf8(lines.at(i)).trimmed();
            }
        }
        const QStringList parts = status.split(QLatin1Char(' '));
        if (parts.value(0) == QLatin1String("ok")) {
            m_LastResult = QStringLiteral("ok|") + parts.value(1);
            qInfo() << "Updated to" << parts.value(1);
        }
        else if (parts.value(0) == QLatin1String("failed")) {
            m_LastResult = QStringLiteral("failed|") + parts.value(1) + QLatin1Char('|') + parts.mid(2).join(QLatin1Char(' '));
            qWarning() << "The last update failed:" << status;
        }
        QFile::remove(resultPath);
        emit lastResultChanged();
    }

    // Leftovers. A copy of the updater that is still running cannot be deleted, which also means that
    // an update is in progress and nothing may be touched.
    const QString runDir = QDir(m_UpdateDir).filePath(QStringLiteral("run"));
    const QString runningCopy = QDir(runDir).filePath(QLatin1String(UpdaterName));
    if (QFile::exists(runningCopy) && !QFile::remove(runningCopy)) {
        return;
    }
    // Another window of this folder that is staging an update holds the work folder lock
    const QString workFolder = QDir(m_InstallDir).filePath(QStringLiteral(".nyxupdate"));
    QLockFile workLock(QDir(workFolder).filePath(QStringLiteral("update.lock")));
    workLock.setStaleLockTime(0);
    if (QDir(workFolder).exists() && !workLock.tryLock(0)) {
        return;
    }
    QDir(runDir).removeRecursively();
    const QStringList leftovers = QDir(m_UpdateDir).entryList({ QStringLiteral("*.part"), QStringLiteral("*.zip") }, QDir::Files);
    for (const QString& name : leftovers) {
        QFile::remove(QDir(m_UpdateDir).filePath(name));
    }
    const QString work = QDir(m_InstallDir).filePath(QStringLiteral(".nyxupdate"));
    QDir(QDir(work).filePath(QStringLiteral("stage"))).removeRecursively();
    workLock.unlock();
    QDir().rmdir(work);
    removeBackups(QDir(m_InstallDir), true);
}
