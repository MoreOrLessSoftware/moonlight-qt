// The overlay menu's input and actions (see OverlayMenu for how it's drawn)

#include "session.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QCoreApplication>

// How long Quit Session waits to be pressed again
#define OVERLAY_MENU_QUIT_CONFIRM_MS 3000

// How long a message (like why a host command failed) shows on its button
#define OVERLAY_MENU_MESSAGE_MS 4000

// A host group's button without an icon of its own, by where the group comes from
static QString defaultGroupIcon(const QString& kind)
{
    if (kind == "app") {
        return "videogame_asset";
    }
    else if (kind == "exe") {
        return "web_asset";
    }
    else if (kind == "plugin") {
        return "extension";
    }
    return "folder";
}

// Network jitter tolerance's choices, as in the settings
static const int k_JitterTolerancePercentiles[] = { 90, 97, 99 };

void Session::openOverlayMenu(int slot)
{
    if (m_OverlayMenu.isOpen()) {
        return;
    }

    // Guide and Mouse need a gamepad to act for
    m_OverlayMenu.setShowGamepadButtons(m_InputHandler->getFirstGamepadSlot() >= 0);
    if (!m_OverlayMenu.open(m_Window)) {
        return;
    }

    m_OverlayMenuSlot = slot;
    m_InputHandler->notifyOverlayMenuOpened();

    // The host's commands can depend on what's running there now
    fetchHostCommands();

    // The pointer is shown for clicking the menu's buttons, and captured again as it closes
    m_OverlayMenuPressedButton = false;
    m_OverlayMenuReleasedMouse = m_InputHandler->isCaptureActive();
    if (m_OverlayMenuReleasedMouse) {
        m_InputHandler->setCaptureActive(false);
    }
}

void Session::handleOverlayMenuMouseButton(int x, int y, int button, bool pressed)
{
    if (!m_OverlayMenu.isOpen()) {
        return;
    }

    if (button == SDL_BUTTON_LEFT) {
        bool onButton = m_OverlayMenu.selectAt(x, y);

        // Acting on the release, as Moonlight captures the mouse on a release, so the host
        // doesn't get a release without a press once the mouse is captured again
        if (pressed) {
            m_OverlayMenuPressedButton = onButton;
        }
        else if (onButton && m_OverlayMenuPressedButton) {
            activateOverlayMenuAction(m_OverlayMenu.selectedAction());
        }
        else if (!onButton && !m_OverlayMenuPressedButton) {
            // A click outside the menu closes it, like a popup
            closeOverlayMenu();
        }
    }
    else if (button == SDL_BUTTON_RIGHT && !pressed) {
        // Back, like B
        handleOverlayMenuInput(B_FLAG, -1);
    }
}

void Session::handleOverlayMenuMouseMotion(int x, int y)
{
    if (m_OverlayMenu.isOpen()) {
        m_OverlayMenu.selectAt(x, y);
    }
}

void Session::closeOverlayMenu()
{
    // The menu's closed handler gives the input back to the host
    m_OverlayMenu.close();
}

void Session::handleOverlayMenuInput(int buttons, int slot)
{
    // A gamepad's buttons act for that gamepad, and the keyboard's for the one that opened
    // the menu (or the first one)
    if (slot >= 0) {
        m_OverlayMenuSlot = slot;
    }

    // Several at once can come from one GameInput reading. Once one closes the menu, the
    // rest are dropped.
    static const int k_MenuButtons[] = {
        UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
        A_FLAG, B_FLAG, BACK_FLAG, X_FLAG, Y_FLAG, PLAY_FLAG, RB_FLAG,
    };
    for (int button : k_MenuButtons) {
        if (!(buttons & button) || !m_OverlayMenu.isOpen()) {
            continue;
        }

        bool mainButtons = m_OverlayMenu.isOnMainButtons();

        switch (button) {
        case UP_FLAG:
            m_OverlayMenu.navigate(OverlayMenu::DirectionUp);
            break;
        case DOWN_FLAG:
            m_OverlayMenu.navigate(OverlayMenu::DirectionDown);
            break;
        case LEFT_FLAG:
            m_OverlayMenu.navigate(OverlayMenu::DirectionLeft);
            break;
        case RIGHT_FLAG:
            m_OverlayMenu.navigate(OverlayMenu::DirectionRight);
            break;

        case A_FLAG:
            activateOverlayMenuAction(m_OverlayMenu.selectedAction());
            break;

        // B (or Select, like Android's back button) goes back from options, or closes
        case B_FLAG:
        case BACK_FLAG:
            if (mainButtons) {
                closeOverlayMenu();
            }
            else {
                m_OverlayMenu.showMainButtons();
            }
            break;

        // Shortcuts to the main buttons, as on the Android client (Y was its keyboard)
        case X_FLAG:
            if (mainButtons) {
                activateOverlayMenuAction(OverlayMenu::ActionMouseEmulation);
            }
            break;
        case Y_FLAG:
            if (mainButtons) {
                activateOverlayMenuAction(OverlayMenu::ActionFullScreen);
            }
            break;
        case PLAY_FLAG:
            if (mainButtons) {
                activateOverlayMenuAction(OverlayMenu::ActionGuide);
            }
            break;
        case RB_FLAG:
            if (mainButtons) {
                activateOverlayMenuAction(OverlayMenu::ActionStats);
            }
            break;
        }
    }
}

void Session::activateOverlayMenuAction(int action)
{
    switch (action) {
    case OverlayMenu::ActionGuide:
        closeOverlayMenu();
        m_InputHandler->sendGuideButton(m_OverlayMenuSlot);
        break;

    case OverlayMenu::ActionMouseEmulation:
        closeOverlayMenu();
        m_InputHandler->toggleMouseEmulation(m_OverlayMenuSlot);
        break;

    case OverlayMenu::ActionFullScreen:
        closeOverlayMenu();
        toggleFullscreen();

        // As for the shortcut, in case key events get lost across the change
        m_InputHandler->raiseAllKeys();
        break;

    case OverlayMenu::ActionPasteText:
        closeOverlayMenu();
        m_InputHandler->pasteClipboardText();
        break;

    case OverlayMenu::ActionStats: {
        // The stats overlay's three modes, as the shortcut cycles through them
        int mode = !m_OverlayManager.isOverlayEnabled(Overlay::OverlayDebug) ? 0 :
                   !m_OverlayManager.isOverlayLatencyMeasurementEnabled(Overlay::OverlayDebug) ? 1 : 2;
        m_OverlayMenu.showOptions(action,
                                  { QCoreApplication::translate("OverlayMenu", "Off"),
                                    QCoreApplication::translate("OverlayMenu", "Stats"),
                                    QCoreApplication::translate("OverlayMenu", "Latency Measurement") },
                                  mode);
        break;
    }

    case OverlayMenu::ActionJitterTolerance: {
        int percentile = m_PacingArrivalPercentileOverride != 0 ?
                    m_PacingArrivalPercentileOverride : StreamingPreferences::get()->pacingArrivalPercentile;
        int checked = -1;
        for (int i = 0; i < (int)SDL_arraysize(k_JitterTolerancePercentiles); i++) {
            if (k_JitterTolerancePercentiles[i] == percentile) {
                checked = i;
            }
        }
        m_OverlayMenu.showOptions(action,
                                  { QCoreApplication::translate("OverlayMenu", "Lowest Latency"),
                                    QCoreApplication::translate("OverlayMenu", "Balanced"),
                                    QCoreApplication::translate("OverlayMenu", "Smoothest") },
                                  checked);
        break;
    }

    case OverlayMenu::ActionQuit:
        if (!m_OverlayMenu.isQuitConfirmation()) {
            // Quitting closes the game on the host, losing anything unsaved, so it takes a
            // second press within a few seconds. Each timer carries its own number, so only the
            // latest takes the question back.
            m_OverlayMenu.setQuitConfirmation(true);
            SDL_AddTimer(OVERLAY_MENU_QUIT_CONFIRM_MS, [](Uint32, void* param) -> Uint32 {
                SDL_Event event = {};
                event.type = SDL_USEREVENT;
                event.user.code = SDL_CODE_OVERLAY_MENU_QUIT_TIMEOUT;
                event.user.data1 = param;
                SDL_PushEvent(&event);
                return 0;
            }, (void*)(intptr_t)++m_OverlayMenuQuitTimeouts);
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quitting the session from the overlay menu");
            closeOverlayMenu();

            // The host app is quit whatever the settings say
            m_QuitHostApp = true;

            SDL_Event event = {};
            event.type = SDL_QUIT;
            event.quit.timestamp = SDL_GetTicks();
            SDL_PushEvent(&event);
        }
        break;

    case OverlayMenu::ActionDisconnect: {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Disconnecting from the overlay menu");
        closeOverlayMenu();

        // The host app keeps running whatever the settings say
        setLeaveHostAppRunning();

        SDL_Event event = {};
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        break;
    }

    case OverlayMenu::ActionClose:
        closeOverlayMenu();
        break;

    case OverlayMenu::ActionBack:
        m_OverlayMenu.showMainButtons();
        break;

    default:
        if (action >= OverlayMenu::ActionGroupCommandBase) {
            int group = m_OverlayMenu.getGroup();
            int index = action - OverlayMenu::ActionGroupCommandBase;
            if (group >= 0 && group < m_HostCommands.groups.size() &&
                    index < m_HostCommands.groups[group].commands.size()) {
                runHostCommand(m_HostCommands.groups[group].commands[index], action);
            }
        }
        else if (action >= OverlayMenu::ActionHostGroupBase) {
            m_OverlayMenu.showGroup(action - OverlayMenu::ActionHostGroupBase);
        }
        else if (action >= OverlayMenu::ActionHostCommandBase) {
            int index = action - OverlayMenu::ActionHostCommandBase;
            if (index < m_HostCommands.commands.size()) {
                runHostCommand(m_HostCommands.commands[index], action);
            }
        }
        else if (action >= OverlayMenu::ActionOptionBase) {
            pickOverlayMenuOption(action - OverlayMenu::ActionOptionBase);
        }
        else if (action >= OverlayMenu::ActionCustomBase &&
                 action - OverlayMenu::ActionCustomBase < m_OverlayMenuCommands.size()) {
            const OverlayMenuCommand& command = m_OverlayMenuCommands[action - OverlayMenu::ActionCustomBase];

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Sending the overlay menu's custom command: %s",
                        qPrintable(OverlayMenuCommand::describeKeys(command.steps)));

            // Closing first gives the game its focus back before the keys arrive. Otherwise the
            // menu stays open for more, unless the session is ending once they're sent.
            if (command.postAction == OverlayMenuCommand::PostActionCloseMenu) {
                closeOverlayMenu();
            }

            m_InputHandler->sendKeySequence(command.steps, SDL_CODE_OVERLAY_MENU_COMMAND_DONE,
                                            (void*)(intptr_t)command.postAction,
                                            (void*)(intptr_t)m_OverlayMenuCommandToken);
        }
        break;
    }
}

void Session::finishOverlayMenuCommand(const SDL_UserEvent& event)
{
    // One from an earlier session has nothing to do with this one
    if ((int)(intptr_t)event.data2 != m_OverlayMenuCommandToken) {
        return;
    }

    switch ((int)(intptr_t)event.data1) {
    case OverlayMenuCommand::PostActionDisconnect: {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Disconnecting after the overlay menu's custom command");
        closeOverlayMenu();
        setLeaveHostAppRunning();

        SDL_Event quitEvent = {};
        quitEvent.type = SDL_QUIT;
        quitEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&quitEvent);
        break;
    }

    case OverlayMenuCommand::PostActionQuit: {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Quitting the session after the overlay menu's custom command");
        closeOverlayMenu();
        m_QuitHostApp = true;

        SDL_Event quitEvent = {};
        quitEvent.type = SDL_QUIT;
        quitEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&quitEvent);
        break;
    }

    default:
        break;
    }
}

void Session::fetchHostCommands()
{
    if (!m_HostCommandsSupported || m_HostCommandsFetching) {
        return;
    }

    m_HostCommandsFetching = true;
    HostCommandsClient::fetch(m_HostCommandsTarget, m_OverlayMenuCommandToken);
}

void Session::handleHostCommandsFetched(const SDL_UserEvent& event)
{
    QScopedPointer<HostCommandsFetchResult> result((HostCommandsFetchResult*)event.data1);
    if ((int)(intptr_t)event.data2 != m_OverlayMenuCommandToken) {
        return;
    }

    m_HostCommandsFetching = false;
    if (result->unsupported) {
        m_HostCommandsSupported = false;
        m_HostCommands = {};
    }
    else if (result->ok) {
        m_HostCommands = result->commands;
    }
    else {
        // A failed request keeps what the menu has
        return;
    }

    QVector<QPair<QString, QString>> commands;
    for (const HostCommand& command : std::as_const(m_HostCommands.commands)) {
        commands.append({ command.icon, command.label });
    }

    QVector<OverlayMenu::HostGroup> groups;
    for (const HostCommandGroup& group : std::as_const(m_HostCommands.groups)) {
        OverlayMenu::HostGroup item;
        item.icon = group.icon.isEmpty() ? defaultGroupIcon(group.kind) : group.icon;
        item.label = group.label;
        for (const HostCommand& command : group.commands) {
            item.commands.append({ command.icon, command.label });
        }
        groups.append(item);
    }

    m_OverlayMenu.setHostCommands(commands, groups);
}

void Session::runHostCommand(const HostCommand& command, int action)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Running the host's command: %s",
                qPrintable(command.label));

    // The menu stays open until the host answers, so a failure can show on the button
    m_HostCommandRunningId = command.id;
    m_HostCommandRunningAction = action;
    m_HostCommandRunningCloses = command.closeMenu;
    HostCommandsClient::run(m_HostCommandsTarget, command.id, m_OverlayMenuCommandToken);
}

void Session::handleHostCommandRan(const SDL_UserEvent& event)
{
    QScopedPointer<HostCommandRunResult> result((HostCommandRunResult*)event.data1);
    if ((int)(intptr_t)event.data2 != m_OverlayMenuCommandToken || result->id != m_HostCommandRunningId) {
        return;
    }

    m_HostCommandRunningId.clear();
    if (result->ok) {
        if (m_HostCommandRunningCloses) {
            closeOverlayMenu();
        }
    }
    else {
        showOverlayMenuMessage(m_HostCommandRunningAction, result->message);
    }
}

void Session::showOverlayMenuMessage(int action, const QString& message)
{
    if (!m_OverlayMenu.isOpen()) {
        return;
    }

    m_OverlayMenu.setMessage(action, message);

    struct Timeout {
        int message;
        int action;
    };
    SDL_AddTimer(OVERLAY_MENU_MESSAGE_MS, [](Uint32, void* param) -> Uint32 {
        auto timeout = (Timeout*)param;
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_OVERLAY_MENU_MESSAGE_TIMEOUT;
        event.user.data1 = (void*)(intptr_t)timeout->message;
        event.user.data2 = (void*)(intptr_t)timeout->action;
        SDL_PushEvent(&event);
        delete timeout;
        return 0;
    }, new Timeout { ++m_OverlayMenuMessages, action });
}

void Session::pickOverlayMenuOption(int option)
{
    int optionsAction = m_OverlayMenu.getOptionsAction();
    closeOverlayMenu();

    switch (optionsAction) {
    case OverlayMenu::ActionStats:
        m_OverlayManager.setOverlayState(Overlay::OverlayDebug, option != 0);
        if (option != 0) {
            m_OverlayManager.setOverlayLatencyMeasurement(Overlay::OverlayDebug, option == 2);
        }
        break;

    case OverlayMenu::ActionJitterTolerance:
        if (option >= 0 && option < (int)SDL_arraysize(k_JitterTolerancePercentiles)) {
            // For the rest of this session only, as on the Android client. Kept here for a
            // decoder made later in the session too (see chooseDecoder()).
            m_PacingArrivalPercentileOverride = k_JitterTolerancePercentiles[option];

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Network jitter tolerance set to the %dth percentile from the overlay menu",
                        m_PacingArrivalPercentileOverride);

            SDL_LockMutex(m_DecoderLock);
            if (m_VideoDecoder != nullptr) {
                m_VideoDecoder->setPacingArrivalPercentile(m_PacingArrivalPercentileOverride);
            }
            SDL_UnlockMutex(m_DecoderLock);
        }
        break;
    }
}
