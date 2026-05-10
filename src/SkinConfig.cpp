#include "SkinConfig.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>
#include <memory>

std::unique_ptr<SkinConfig> SkinConfig::s_instance;

SkinConfig::SkinConfig(QObject* parent)
    : QObject(parent)
{
}

SkinConfig::~SkinConfig() = default;

bool SkinConfig::loadFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[SkinConfig] Failed to open skin file:" << path
                   << file.errorString();
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[SkinConfig] JSON parse error at line" << parseError.offset
                   << ":" << parseError.errorString();
        return false;
    }

    s_instance = std::make_unique<SkinConfig>();
    if (!s_instance->parse(doc.object())) {
        qWarning() << "[SkinConfig] Failed to parse skin manifest:" << path;
        s_instance.reset();
        return false;
    }

    qDebug() << "[SkinConfig] Loaded skin '" << s_instance->m_skinName
             << "' from" << path;
    return true;
}

SkinConfig* SkinConfig::instance()
{
    return s_instance.get();
}

bool SkinConfig::parse(const QJsonObject& root)
{
    // --- Meta (optional) ---
    if (root.contains("name")) {
        m_skinName = root["name"].toString();
    }
    if (root.contains("version")) {
        m_version = root["version"].toString();
    }

    // --- Window ---
    if (root.contains("window")) {
        QJsonObject window = root["window"].toObject();
        if (window.contains("frameless")) {
            m_frameless = window["frameless"].toBool(true);
        }
    }

    // --- Sidebar ---
    if (root.contains("sidebar")) {
        QJsonObject sidebar = root["sidebar"].toObject();
        if (sidebar.contains("detached")) {
            m_sidebarDetached = sidebar["detached"].toBool(false);
        }
        if (sidebar.contains("defaultPosition")) {
            QJsonObject pos = sidebar["defaultPosition"].toObject();
            if (pos.contains("x")) {
                m_sidebarDefaultX = pos["x"].toInt(-76);
            }
            if (pos.contains("y")) {
                m_sidebarDefaultY = pos["y"].toInt(200);
            }
        }
    }

    // --- Theme ---
    if (root.contains("theme")) {
        QJsonObject theme = root["theme"].toObject();
        if (theme.contains("background")) {
            m_themeBackground = theme["background"].toString(m_themeBackground);
        }
        if (theme.contains("surface")) {
            m_themeSurface = theme["surface"].toString(m_themeSurface);
        }
        if (theme.contains("border")) {
            m_themeBorder = theme["border"].toString(m_themeBorder);
        }
    }

    return true;  // always succeeds — missing keys use defaults
}

// --- Property getters ---

bool SkinConfig::isFrameless() const { return m_frameless; }

bool SkinConfig::isSidebarDetached() const { return m_sidebarDetached; }
int SkinConfig::sidebarDefaultX() const { return m_sidebarDefaultX; }
int SkinConfig::sidebarDefaultY() const { return m_sidebarDefaultY; }

QString SkinConfig::themeBackground() const { return m_themeBackground; }
QString SkinConfig::themeSurface() const { return m_themeSurface; }
QString SkinConfig::themeBorder() const { return m_themeBorder; }

QString SkinConfig::skinName() const { return m_skinName; }
