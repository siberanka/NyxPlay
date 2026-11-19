#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QByteArray>
#include <QStringList>

class QNetworkReply;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

signals:
    void onUpdateAvailable(QString newVersion, QString url);

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
};
