#include "computerseeker.h"
#include "computermanager.h"
#include "settings/streamingpreferences.h"
#include <QTimer>
#include <QUrl>
#include <QReadLocker>
#include <QWriteLocker>

ComputerSeeker::ComputerSeeker(ComputerManager *manager, QString computerName, QObject *parent)
    : QObject(parent), m_ComputerManager(manager), m_ComputerName(computerName),
      m_TimeoutTimer(new QTimer(this)), m_HasExplicitAddress(false)
{
    m_HasExplicitAddress = parseExplicitAddress(computerName, m_ExplicitAddress);

    // If we know this computer, send a WOL packet to wake it up in case it is asleep.
    NvComputer* matchingComputer = findMatchingComputer();
    if (matchingComputer) {
        matchingComputer->wake();
    }

    m_TimeoutTimer->setSingleShot(true);
    connect(m_TimeoutTimer, &QTimer::timeout,
            this, &ComputerSeeker::onTimeout);
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerSeeker::onComputerUpdated);
}

void ComputerSeeker::start(int timeout)
{
    m_TimeoutTimer->start(timeout);

    // If we don't know this computer by name, address, or UUID, try adding it
    // manually and see if we can find it by address, hostname, or mDNS.
    //
    // NB: We don't do this unconditionally because it will wipe out the user's
    // manual address if they pass another reachable hostname/address.
    if (!findMatchingComputer()) {
        m_ComputerManager->addNewHostManually(m_ComputerName);
    }

    if (m_HasExplicitAddress) {
        // The user named the endpoint to use, so discovery must not pick another one.
        // This only changes the running process; the setting is not saved.
        StreamingPreferences::get()->enableMdns = false;
    }

    m_ComputerManager->startPolling();
}

bool ComputerSeeker::parseExplicitAddress(const QString& argument, NvAddress& address)
{
    QHostAddress ip(argument);
    if (!ip.isNull()) {
        // A bare IPv4 or unescaped IPv6 literal uses the default port
        address = NvAddress(ip, DEFAULT_HTTP_PORT);
        return true;
    }

    QUrl url = QUrl::fromUserInput("moonlight://" + argument);
    if (!url.isValid() || url.host().isEmpty() || url.scheme() != "moonlight") {
        return false;
    }

    bool hasPort = url.port() != -1;
    if (QHostAddress(url.host()).isNull() && !hasPort) {
        return false;
    }

    address = NvAddress(url.host(), url.port(DEFAULT_HTTP_PORT));
    return true;
}

bool ComputerSeeker::useExplicitAddress(NvComputer *computer)
{
    QSslCertificate cert;
    bool trueUid;
    {
        QReadLocker lock(&computer->lock);
        if (computer->activeAddress == m_ExplicitAddress) {
            return true;
        }
        cert = computer->serverCert;
        trueUid = !computer->isNvidiaServerSoftware;
    }

    // The PC answered through some other address. Only switch if the PC also
    // answers at the address the user gave, and check that at a limited rate.
    if (m_LastProbe.isValid() && m_LastProbe.elapsed() < 1500) {
        return false;
    }
    m_LastProbe.start();

    try {
        NvHTTP http(m_ExplicitAddress, 0, cert, trueUid);
        QString serverInfo = http.getServerInfo(NvHTTP::NvLogLevel::NVLL_NONE, true);
        NvComputer probe(http, serverInfo);
        if (probe.uuid != computer->uuid) {
            return false;
        }
    } catch (...) {
        return false;
    }

    QWriteLocker lock(&computer->lock);
    computer->activeAddress = m_ExplicitAddress;
    return true;
}

void ComputerSeeker::onComputerUpdated(NvComputer *computer)
{
    if (!m_TimeoutTimer->isActive()) {
        return;
    }
    if (matchComputer(computer) && isOnline(computer)) {
        if (m_HasExplicitAddress && !useExplicitAddress(computer)) {
            // Keep waiting: the requested address may still come up before the deadline
            return;
        }

        m_ComputerManager->stopPollingAsync();
        m_TimeoutTimer->stop();
        emit computerFound(computer);
    }
}

bool ComputerSeeker::matchComputer(NvComputer *computer) const
{
    QString value = m_ComputerName.toLower();

    if (computer->name.toLower() == value || computer->uuid.toLower() == value) {
        return true;
    }

    const auto uniqueAddresses = computer->uniqueAddresses();
    for (const NvAddress& addr : uniqueAddresses) {
        if (addr.address().toLower() == value || addr.toString().toLower() == value) {
            return true;
        }
    }

    return false;
}

NvComputer* ComputerSeeker::findMatchingComputer() const
{
    const auto computers = m_ComputerManager->getComputers();
    for (NvComputer* computer : computers) {
        if (this->matchComputer(computer)) {
            return computer;
        }
    }

    return nullptr;
}

bool ComputerSeeker::isOnline(NvComputer *computer) const
{
    return computer->state == NvComputer::CS_ONLINE;
}

void ComputerSeeker::onTimeout()
{
    m_TimeoutTimer->stop();
    m_ComputerManager->stopPollingAsync();
    emit errorTimeout();
}
