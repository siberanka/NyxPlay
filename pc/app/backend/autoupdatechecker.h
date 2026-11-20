#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QByteArray>
#include <QStringList>

#include "updatelogic.h"

class QNetworkReply;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    // The one checker of the application: the QML singleton and the update installer share it.
    static AutoUpdateChecker* instance();

    // userInitiated: also check when automatic checks are switched off in the settings
    Q_INVOKABLE void start(bool userInitiated = false);

    bool isRunning() const { return m_Nam != nullptr; }

signals:
    void onUpdateAvailable(QString newVersion, QString url);

    // Every finished check, whatever its outcome (failed: the manifest could not be fetched or used)
    void checkFinished(bool available, bool failed);

    // Details of an available update, including the install package if the manifest has one
    void updateFound(const UpdateLogic::Result& result);

private slots:
    void handleReadyRead();
    void handleFinished();

private:
    void fetchNext();

    void finish();

    QString getPlatform();

    QString m_CurrentVersion;
    QNetworkAccessManager* m_Nam;
    QNetworkReply* m_Reply;
    QStringList m_Urls;
    int m_NextUrl;
    QByteArray m_Body;
    bool m_UserInitiated = false;
};
