#include "updateinstaller.h"

#include "autoupdatechecker.h"
#include "../path.h"

#include <QCoreApplication>

UpdateInstaller* UpdateInstaller::instance()
{
    static UpdateInstaller* installer = new UpdateInstaller(QCoreApplication::applicationDirPath(),
                                                            Path::getUpdateDir(),
                                                            QStringLiteral(VERSION_STR),
                                                            AutoUpdateChecker::instance(),
                                                            nullptr, QCoreApplication::instance());
    return installer;
}
