#pragma once

#include <QObject>
#include <QElapsedTimer>

#include "nvaddress.h"

class ComputerManager;
class NvComputer;
class QTimer;

class ComputerSeeker : public QObject
{
    Q_OBJECT
public:
    explicit ComputerSeeker(ComputerManager *manager, QString computerName, QObject *parent = nullptr);

    void start(int timeout);

    // Parses a command line host argument that names a specific endpoint, which is
    // an IP literal or any host with an explicit port. Plain host names return false.
    static bool parseExplicitAddress(const QString& argument, NvAddress& address);

signals:
    void computerFound(NvComputer *computer);
    void errorTimeout();

private slots:
    void onComputerUpdated(NvComputer *computer);
    void onTimeout();

private:
    bool matchComputer(NvComputer *computer) const;
    NvComputer* findMatchingComputer() const;
    bool isOnline(NvComputer *computer) const;
    bool useExplicitAddress(NvComputer *computer);

private:
    ComputerManager *m_ComputerManager;
    QString m_ComputerName;
    QTimer *m_TimeoutTimer;
    bool m_HasExplicitAddress;
    NvAddress m_ExplicitAddress;
    QElapsedTimer m_LastProbe;
};
