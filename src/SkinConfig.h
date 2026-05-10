#pragma once

#include <QObject>
#include <QString>
#include <memory>

/**
 * SkinConfig — parses a skin manifest JSON file and exposes structured
 * settings to the rest of the app.
 *
 * Singleton: call SkinConfig::loadFromFile() at startup, then access via
 * SkinConfig::instance(). Returns nullptr if no skin was loaded.
 *
 * All properties default to current Basecamp behavior (embedded sidebar,
 * hardcoded colors) so an absent or partial config file is safe.
 */
class SkinConfig : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool frameless READ isFrameless CONSTANT)
    Q_PROPERTY(bool sidebarDetached READ isSidebarDetached CONSTANT)
    Q_PROPERTY(int sidebarDefaultX READ sidebarDefaultX CONSTANT)
    Q_PROPERTY(int sidebarDefaultY READ sidebarDefaultY CONSTANT)
    Q_PROPERTY(QString themeBackground READ themeBackground CONSTANT)
    Q_PROPERTY(QString themeSurface READ themeSurface CONSTANT)
    Q_PROPERTY(QString themeBorder READ themeBorder CONSTANT)
    Q_PROPERTY(QString skinName READ skinName CONSTANT)

public:
    /** Load and parse a skin manifest from file. Returns true on success.
     * After a successful call, instance() returns the config. */
    static bool loadFromFile(const QString& path);

    /** Return the loaded singleton instance, or nullptr if no skin was loaded. */
    static SkinConfig* instance();

    // --- Window ---
    bool isFrameless() const;

    // --- Sidebar ---
    bool isSidebarDetached() const;
    int sidebarDefaultX() const;
    int sidebarDefaultY() const;

    // --- Theme colors ---
    QString themeBackground() const;
    QString themeSurface() const;
    QString themeBorder() const;

    // --- Meta ---
    QString skinName() const;

private:
    SkinConfig(QObject* parent = nullptr);
    ~SkinConfig();
    bool parse(const QJsonObject& root);

    // Window
    bool m_frameless = true;  // default: frameless (prototype always frameless)

    // Sidebar
    bool m_sidebarDetached = false;  // default: embedded (current behavior)
    int m_sidebarDefaultX = -76;     // left of main window
    int m_sidebarDefaultY = 200;     // offset down from top

    // Theme colors — match current hardcoded values
    QString m_themeBackground = "#171717";
    QString m_themeSurface = "#262626";
    QString m_themeBorder = "#434343";

    // Meta
    QString m_skinName;
    QString m_version;

    // Singleton
    static std::unique_ptr<SkinConfig> s_instance;
};
