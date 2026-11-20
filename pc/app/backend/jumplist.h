#pragma once

#include <QString>

// The Windows taskbar / Start menu jump list of NyxPlay: the tasks "Check for updates" and, while an
// update is pending, "Install update <version>". Each task starts NyxPlay.exe with --check-update or
// --install-update. Does nothing on other platforms.
namespace JumpList {

// Replaces the task list. pendingVersion is empty when no update is pending. Returns false if the
// shell refused the list (the tasks are cosmetic, so callers only log this).
bool update(const QString& pendingVersion);

}
