#pragma once

#include <QList>
#include <QSettings>
#include <QString>
#include <QVariantMap>

// Read-only view of another installation's saved data
class ImportSource
{
public:
    virtual ~ImportSource() {}

    // Raw host records as stored by the other installation
    virtual QList<QVariantMap> hosts() const = 0;

    // Root-level preference values
    virtual QVariantMap preferences() const = 0;
};

// Adapter over a QSettings location
class QSettingsImportSource : public ImportSource
{
public:
    explicit QSettingsImportSource(QSettings& settings) : m_Settings(settings) {}

    QList<QVariantMap> hosts() const override;
    QVariantMap preferences() const override;

private:
    QSettings& m_Settings;
};

class MoonlightImport
{
public:
    struct Result {
        int hostsImported = 0;
        int hostsSkipped = 0;
        int preferencesImported = 0;
    };

    static constexpr const char* KEY_DONE = "moonlightimportdone";

    // True if no import was done yet and no hosts exist
    static bool isNeeded(QSettings& dest);

    // Copies the validated host list and preferences. The client identity,
    // pinned server certificates and cached app lists are never copied, so
    // imported hosts are unpaired. The done flag is always set.
    static Result run(QSettings& dest, const ImportSource& source);

    // Startup entry point: imports from an existing Moonlight installation
    // when one is present and records a one-time notice for the UI.
    static void runAtStartup();

    // Exposed for testing
    static QVariantMap sanitizeHost(const QVariantMap& raw);
    static QVariantMap sanitizePreferences(const QVariantMap& raw);
};
