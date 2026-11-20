#include "updatecommand.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
#include <QTimer>
#include <QtDebug>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <sddl.h>
#endif

namespace UpdateCommand {

static const char CheckLine[] = "check-update\n";
static const char InstallLine[] = "install-update\n";

Command extract(QStringList& arguments)
{
    if (arguments.size() != 2) {
        return Command::None;
    }
    // Exact, case-sensitive match: nothing else is ever treated as an update command
    Command command = Command::None;
    if (arguments.at(1) == QLatin1String("--check-update")) {
        command = Command::CheckUpdate;
    }
    else if (arguments.at(1) == QLatin1String("--install-update")) {
        command = Command::InstallUpdate;
    }
    if (command != Command::None) {
        arguments.removeAt(1);
    }
    return command;
}

QByteArray toWire(Command command)
{
    switch (command) {
    case Command::CheckUpdate:
        return QByteArray(CheckLine);
    case Command::InstallUpdate:
        return QByteArray(InstallLine);
    default:
        return QByteArray();
    }
}

Command fromWire(const QByteArray& line)
{
    if (line == CheckLine) {
        return Command::CheckUpdate;
    }
    if (line == InstallLine) {
        return Command::InstallUpdate;
    }
    return Command::None;
}

QString serverName(const QString& userSid, const QString& installDir)
{
    const QString folder = QDir::cleanPath(installDir).toLower();
    const QByteArray digest = QCryptographicHash::hash((userSid + QLatin1Char('|') + folder).toUtf8(),
                                                       QCryptographicHash::Sha256).toHex();
    return QStringLiteral("NyxPlay-cmd-") + QString::fromLatin1(digest.left(40));
}

QString currentUserSid()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return QString();
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(int(size), '\0');
    QString result;
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            result = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return result;
#else
    return QString::number(qulonglong(0));
#endif
}

bool sendToRunningInstance(const QString& name, Command command, int timeoutMs)
{
    // The receiving instance may be busy drawing; once the command is in its pipe it is theirs to handle,
    // so a late acknowledgement must not make the sender run the command a second time.
    const int ackTimeoutMs = 10000;
    const QByteArray wire = toWire(command);
    if (wire.isEmpty()) {
        return false;
    }
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    socket.write(wire);
    if (!socket.waitForBytesWritten(timeoutMs)) {
        return false;
    }
    // The receiving instance answers once it has accepted the command
    if (!socket.waitForReadyRead(ackTimeoutMs)) {
        qWarning() << "The running instance did not acknowledge the command in time";
        return true;
    }
    return socket.readLine(16) == "ok\n";
}

Server::Server(QObject* parent) :
    QObject(parent)
{
    // Only the same user may connect
    m_Server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_Server, &QLocalServer::newConnection, this, &Server::handleConnection);
}

bool Server::listen(const QString& name)
{
    if (!m_Server.listen(name)) {
        qWarning() << "Command server not started:" << m_Server.errorString();
        return false;
    }
    return true;
}

void Server::handleConnection()
{
    while (QLocalSocket* socket = m_Server.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);

        // A client that sends nothing, or anything long, is dropped
        QTimer::singleShot(2000, socket, [socket]() { socket->abort(); });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            if (socket->bytesAvailable() > 32) {
                socket->abort();
                return;
            }
            if (!socket->canReadLine()) {
                return;
            }
            const Command command = fromWire(socket->readLine(32));
            if (command == Command::None) {
                socket->abort();
                return;
            }
            socket->write("ok\n");
            socket->flush();
            socket->disconnectFromServer();
            emit commandReceived(command);
        });
    }
}

}
