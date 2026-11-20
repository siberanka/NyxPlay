#pragma once

#include <QByteArray>
#include <QString>

// Local file handling of a client update: extracting the verified package into a staging folder,
// checking that the staged files are a complete NyxPlay of the offered version, and detecting
// whether the install folder can be written. No network access.
namespace UpdateStage {

// Staging folder inside the install folder, so that the final moves stay on one volume.
QString workDir(const QString& installDir);
QString stageDir(const QString& installDir);

constexpr int MaxEntries = 8000;
constexpr qint64 MaxUnpackedBytes = 1024LL * 1024 * 1024;

// Total uncompressed size declared by the zip directory, or -1 when the archive is unusable
// or exceeds the limits.
qint64 unpackedSize(const QByteArray& zip, QString* error);

// Extracts the zip into destDir (created, must not contain files). Entry names are checked
// against the file list rules; links, absolute or escaping paths, and oversized archives fail.
// Entries that would overwrite user data (portable marker, settings, caches) are not written.
bool extractPackage(const QByteArray& zip, const QString& destDir, QString* error);

// Product version stored in the version resource of an executable, "a.b.c.d"; empty when unreadable.
QString executableVersion(const QString& exePath);

// Machine type of a PE image (0x8664 = x64, 0xAA64 = arm64), 0 when unreadable.
quint16 executableMachine(const QString& exePath);

// Integrity check of an extracted package: complete and consistent file list, the files the client
// needs to run, and an NyxPlay.exe of the expected version and architecture.
bool checkStage(const QString& stageDir, const QString& expectedVersion, quint16 expectedMachine, QString* error);

// True when files can be created, renamed and deleted in the folder. This is what the update needs;
// a folder that merely looks writable (ACLs, read-only media) is caught by trying.
bool isWritable(const QString& dir);

}
