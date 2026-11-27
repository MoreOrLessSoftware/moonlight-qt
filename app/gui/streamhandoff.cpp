#include "streamhandoff.h"

#include "settings/streamingpreferences.h"
#include "streaming/session.h"
#include "streaming/streamprocess.h"

#include <QScreen>

StreamHandoff::StreamHandoff(bool allowed, QObject* parent)
    : QObject(parent),
      m_Enabled(allowed && StreamProcess::isHandoffEnabled())
{
}

bool StreamHandoff::isEnabled() const
{
    return m_Enabled;
}

bool StreamHandoff::launch(Session* session, QWindow* window)
{
    if (!m_Enabled || session == nullptr) {
        return false;
    }

    QPoint displayOrigin;
    if (window != nullptr && window->screen() != nullptr) {
        displayOrigin = window->screen()->geometry().topLeft();
    }

    // The stream process reads the settings from disk
    StreamingPreferences::get()->save();

    return StreamProcess::launchStream(session->getComputer(), session->getApp(), displayOrigin);
}
