#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

#include <QLockFile>

#include "updatelogic.h"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// Downloads, verifies and stages a new version of the portable client, then hands over to
// NyxPlayUpdater.exe. The steps are strictly ordered: signed checksum list, then the package,
// then the hash, then extraction and the integrity check, and only then the restart question.
class UpdateInstaller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY stateChanged)
    Q_PROPERTY(QString version READ version NOTIFY offerChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY offerChanged)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY offerChanged)
    Q_PROPERTY(bool streamActive READ streamActive NOTIFY stateChanged)
    Q_PROPERTY(QString lastResult READ lastResult NOTIFY lastResultChanged)

public:
    enum State {
        Idle,               // nothing offered
        Available,          // an update is offered
        WaitingForStream,   // install requested, starts when the running stream has ended
        Downloading,
        Verifying,
        Extracting,
        ReadyToRestart,     // staged and checked, waiting for the user's decision
        Restarting,
        Failed
    };
    Q_ENUM(State)

    enum Error {
        NoError,
        NotWritable,        // the install folder cannot be written
        NoConnection,
        DownloadFailed,
        VerifyFailed,       // signature, hash or size did not check out; the download was deleted
        NoSpace,
        PackageInvalid,     // the package did not pass the integrity check
        LaunchFailed,       // the updater could not be started
        NotInstallable,     // no install package in the manifest
        Busy                // another NyxPlay window of this folder is already updating
    };
    Q_ENUM(Error)

    // installDir: folder of NyxPlay.exe, updateDir: private working folder, currentVersion: running version
    UpdateInstaller(const QString& installDir, const QString& updateDir, const QString& currentVersion,
                    QObject* checker, QNetworkAccessManager* nam = nullptr, QObject* parent = nullptr);

    // The application-wide installer (created on first use from the application paths); defined in updateinstaller_app.cpp.
    static UpdateInstaller* instance();

    int state() const { return m_State; }
    int progress() const { return m_Progress; }
    int error() const { return m_Error; }
    QString errorDetail() const { return m_ErrorDetail; }
    QString version() const { return m_Offer.version; }
    QString releaseUrl() const { return m_Offer.url; }
    bool canInstall() const;
    bool streamActive() const;
    QString lastResult() const { return m_LastResult; }

    // True while the user could still be asked to act on the update (shown in the notice).
    bool hasOffer() const { return m_State != Idle; }

    // Whether a stream is running; an install waits for it to end. Defaults to "never".
    void setStreamProbe(std::function<bool()> probe) { m_StreamProbe = std::move(probe); }

    // The same folder-level check that install() does, for the UI to explain beforehand.
    Q_INVOKABLE bool isFolderWritable() const;

    // Looks at the previous update's result and removes leftovers. Call once at start-up.
    void startupMaintenance();

    // Update commands from the jump list or the command line.
    Q_INVOKABLE void checkNow(bool thenInstall = false);
    Q_INVOKABLE void install();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void restartNow();
    Q_INVOKABLE void dismissError();
    Q_INVOKABLE void clearLastResult();

#ifdef NYX_UNIT_TESTS
    // Unit tests sign their fixtures with a throw-away key
    void setTrustedKeys(const QList<QByteArray>& keys) { m_Keys = keys; }
    void setExpectedMachine(quint16 machine) { m_Machine = machine; }
    void setLaunchHook(std::function<bool(const QString& updaterPath, const QStringList& args)> hook) { m_LaunchHook = std::move(hook); }
#endif



public slots:
    // An update the checker found (also used directly by the unit tests)
    void offer(const UpdateLogic::Result& result);

signals:
    void stateChanged();
    void progressChanged();
    void offerChanged();
    void lastResultChanged();

    // An explicit check (jump list, menu) finished; available is false when up to date or failed
    void checkReported(bool available, bool failed);

private slots:
    void pollStream();
    void handleCheckFinished(bool available, bool failed);

private:
    void setState(State state, Error error = NoError, const QString& detail = QString());
    void fail(Error error, const QString& detail = QString());
    void resetWork();

    void begin();
    void fetch(const QList<QUrl>& urls, qint64 limit, const QString& filePath,
               std::function<void(bool ok, const QByteArray& data)> done);
    void fetchNext();
    void finishFetch(bool ok, QNetworkReply* reply);
    void abortFetch();
    QList<QUrl> candidates(const QString& fileName) const;

    void signatureStep();
    void downloadPackage();
    void prepareStep();
    void finishPrepare(bool ok, int error, const QString& detail);
    void maybeAskRestart();
    bool launchUpdater();

    QString m_InstallDir;
    QString m_UpdateDir;
    QString m_CurrentVersion;
    QObject* m_Checker;
    QNetworkAccessManager* m_Nam;

    State m_State = Idle;
    Error m_Error = NoError;
    QString m_ErrorDetail;
    int m_Progress = 0;
    QString m_LastResult;
    UpdateLogic::Result m_Offer;

    std::function<bool()> m_StreamProbe;
    QTimer m_StreamTimer;
    bool m_StagedPending = false;
    bool m_InstallAfterCheck = false;
    bool m_ExplicitCheck = false;

    // The fetch in progress
    QNetworkReply* m_Reply = nullptr;
    QList<QUrl> m_Urls;
    int m_UrlIndex = 0;
    qint64 m_Limit = 0;
    QString m_FilePath;
    QFile* m_File = nullptr;
    QByteArray m_Memory;
    qint64 m_Received = 0;
    bool m_Cancelled = false;
    bool m_NetworkFault = false;
    std::function<void(bool, const QByteArray&)> m_Done;

    std::unique_ptr<QLockFile> m_Lock;
    QByteArray m_Sums;
    QString m_ZipPath;
    QString m_StageDir;
    int m_Generation = 0;

    QList<QByteArray> m_Keys;
    quint16 m_Machine = 0x8664;
#ifdef NYX_UNIT_TESTS
    std::function<bool(const QString&, const QStringList&)> m_LaunchHook;
#endif
};
