#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

// The two update commands that the jump list tasks (and a user) can start NyxPlay with, and how they
// reach an already running instance. Nothing else can be sent this way: the command line accepts exactly
// these two options, and the local server accepts exactly two fixed lines.
namespace UpdateCommand {

enum class Command {
    None,
    CheckUpdate,    // --check-update
    InstallUpdate   // --install-update
};

// Looks for an update option in the arguments (the first entry is the program path). The option is
// removed from the list and returned only when it is the single argument; combined with anything else
// it is left in place so that the normal command line parser rejects it.
Command extract(QStringList& arguments);

// The fixed line that is sent to a running instance, and its strict parser.
QByteArray toWire(Command command);
Command fromWire(const QByteArray& line);

// Name of the local socket of the instance that runs from installDir for the user with the given SID.
// Different users and different install folders never share a name.
QString serverName(const QString& userSid, const QString& installDir);

// SID of the current user as text; empty if it cannot be determined.
QString currentUserSid();

// Sends a command to a running instance. True when an instance accepted it.
bool sendToRunningInstance(const QString& name, Command command, int timeoutMs = 1500);

class Server : public QObject
{
    Q_OBJECT
public:
    explicit Server(QObject* parent = nullptr);

    // Starts listening. Fails quietly when another instance of the same folder already does.
    bool listen(const QString& name);

signals:
    void commandReceived(UpdateCommand::Command command);

private slots:
    void handleConnection();

private:
    QLocalServer m_Server;
};

}

Q_DECLARE_METATYPE(UpdateCommand::Command)
