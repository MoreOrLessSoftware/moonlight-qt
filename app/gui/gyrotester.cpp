#include "gyrotester.h"
#include "sdlgamepadkeynavigation.h"

#include <QtMath>

#define RAD_TO_DEG              57.2957795
#define TRACE_LENGTH            400     // about 2 s at 200 readings per second
#define CROSSHAIR_RANGE_DEG     45.0
#define REST_WINDOW_MS          1000
#define RECEIVING_TIMEOUT_MS    500
#define PUBLISH_INTERVAL_MS     33      // ~30 updates per second for the page

GyroTester::GyroTester(QObject* parent)
    : QObject(parent),
      m_Active(false),
      m_Gamepad(nullptr),
      m_JoystickId(-1),
      m_TraceNext(0),
      m_CrosshairX(0),
      m_CrosshairY(0),
      m_LastGyroTimeUs(0),
      m_RestWindowStart(0),
      m_RestRawMax(0),
      m_RestFilteredMax(0),
      m_RestRawDps(0),
      m_RestFilteredDps(0),
      m_LastReadingTime(0),
      m_Receiving(false),
      m_Dirty(false)
{
    m_Filter.reset(GyroFilterSettings::defaults());
    for (int axis = 0; axis < 3; axis++) {
        m_Raw[axis].fill(0, TRACE_LENGTH);
        m_Filtered[axis].fill(0, TRACE_LENGTH);
    }
    connect(&m_PublishTimer, &QTimer::timeout, this, &GyroTester::onPublishTimer);
}

GyroTester::~GyroTester()
{
    stop();
}

void GyroTester::setDeviceKey(const QString& key)
{
    if (key == m_DeviceKey) {
        return;
    }
    m_DeviceKey = key;
    emit deviceKeyChanged();

    if (m_Active) {
        stop();
        start();
    }
}

void GyroTester::setActive(bool active)
{
    if (active == m_Active) {
        return;
    }
    m_Active = active;
    emit activeChanged();

    if (m_Active) {
        start();
    }
    else {
        stop();
    }
}

void GyroTester::start()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SdlGamepadKeyNavigation* navigation = SdlGamepadKeyNavigation::instance();
    if (m_DeviceKey.isEmpty() || navigation == nullptr || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        return;
    }

    // Find the gamepad, keeping it open (SDL counts opens) while testing
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks && m_Gamepad == nullptr; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        SDL_GameController* gamepad = SDL_GameControllerOpen(i);
        if (gamepad == nullptr) {
            continue;
        }
        if (GamepadSettings::deviceKey(gamepad) == m_DeviceKey && SDL_GameControllerHasSensor(gamepad, SDL_SENSOR_GYRO)) {
            m_Gamepad = gamepad;
        }
        else {
            SDL_GameControllerClose(gamepad);
        }
    }
    if (m_Gamepad == nullptr) {
        return;
    }

    m_JoystickId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_Gamepad));
    m_Filter.reset(GamepadSettings::loadGyro(m_DeviceKey));
    for (int axis = 0; axis < 3; axis++) {
        m_Raw[axis].fill(0);
        m_Filtered[axis].fill(0);
    }
    m_TraceNext = 0;
    m_LastGyroTimeUs = 0;
    m_RestWindowStart = 0;
    m_RestRawDps = m_RestFilteredDps = 0;
    m_Receiving = false;
    recenter();

    SDL_GameControllerSetSensorEnabled(m_Gamepad, SDL_SENSOR_GYRO, SDL_TRUE);
    if (SDL_GameControllerHasSensor(m_Gamepad, SDL_SENSOR_ACCEL)) {
        SDL_GameControllerSetSensorEnabled(m_Gamepad, SDL_SENSOR_ACCEL, SDL_TRUE);
    }

    navigation->setSensorListener([this](const SDL_ControllerSensorEvent& event) {
        onSensorEvent(event);
    });
    m_PublishTimer.start(PUBLISH_INTERVAL_MS);
#endif
}

void GyroTester::stop()
{
    m_PublishTimer.stop();

    if (m_Gamepad == nullptr) {
        return;
    }

    SdlGamepadKeyNavigation* navigation = SdlGamepadKeyNavigation::instance();
    if (navigation != nullptr) {
        navigation->setSensorListener(nullptr);
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        SDL_GameControllerSetSensorEnabled(m_Gamepad, SDL_SENSOR_GYRO, SDL_FALSE);
        SDL_GameControllerSetSensorEnabled(m_Gamepad, SDL_SENSOR_ACCEL, SDL_FALSE);
        SDL_GameControllerClose(m_Gamepad);
    }
#endif
    m_Gamepad = nullptr;
    m_JoystickId = -1;

    if (m_Receiving) {
        m_Receiving = false;
        emit updated();
    }
}

void GyroTester::reloadSettings()
{
    if (!m_DeviceKey.isEmpty()) {
        m_Filter.applySettings(GamepadSettings::loadGyro(m_DeviceKey));
    }
}

void GyroTester::recenter()
{
    m_CrosshairX = m_CrosshairY = 0;
    m_Dirty = true;
}

void GyroTester::onSensorEvent(const SDL_ControllerSensorEvent& event)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (event.which != m_JoystickId) {
        return;
    }

    m_LastReadingTime = event.timestamp;

    if (event.sensor == SDL_SENSOR_ACCEL) {
        // Only for the filter's stillness check and scale; nothing to show
        float accel[3];
        m_Filter.processAccel(event.data, accel);
        return;
    }
    if (event.sensor != SDL_SENSOR_GYRO) {
        return;
    }

    float filtered[3];
    m_Filter.processGyro(event.timestamp, event.data, filtered);

    float rawMagnitude = 0, filteredMagnitude = 0;
    for (int axis = 0; axis < 3; axis++) {
        float raw = (float)(event.data[axis] * RAD_TO_DEG);
        m_Raw[axis][m_TraceNext] = raw;
        m_Filtered[axis][m_TraceNext] = filtered[axis];
        rawMagnitude += raw * raw;
        filteredMagnitude += filtered[axis] * filtered[axis];
    }
    m_TraceNext = (m_TraceNext + 1) % TRACE_LENGTH;

    // The crosshair follows the filtered reading like a camera would: SDL's gyro axes are X to
    // the right, Y up and Z toward the player, counter-clockwise positive, so turning left
    // (+Y) moves it left and tilting the front up (+X) moves it up
    uint64_t timeUs;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    timeUs = event.timestamp_us ? event.timestamp_us : (uint64_t)event.timestamp * 1000;
#else
    timeUs = (uint64_t)event.timestamp * 1000;
#endif
    if (m_LastGyroTimeUs != 0 && timeUs > m_LastGyroTimeUs && timeUs - m_LastGyroTimeUs < 100000) {
        double dt = (timeUs - m_LastGyroTimeUs) / 1000000.0;
        m_CrosshairX = qBound(-CROSSHAIR_RANGE_DEG, m_CrosshairX - filtered[1] * dt, CROSSHAIR_RANGE_DEG);
        m_CrosshairY = qBound(-CROSSHAIR_RANGE_DEG, m_CrosshairY - filtered[0] * dt, CROSSHAIR_RANGE_DEG);
    }
    m_LastGyroTimeUs = timeUs;

    // Largest readings per second, for the noise shown while the gamepad is at rest
    if (m_RestWindowStart == 0 || SDL_TICKS_PASSED(event.timestamp, m_RestWindowStart + REST_WINDOW_MS)) {
        if (m_RestWindowStart != 0) {
            m_RestRawDps = m_RestRawMax;
            m_RestFilteredDps = m_RestFilteredMax;
        }
        m_RestWindowStart = event.timestamp ? event.timestamp : 1;
        m_RestRawMax = m_RestFilteredMax = 0;
    }
    m_RestRawMax = qMax(m_RestRawMax, SDL_sqrtf(rawMagnitude));
    m_RestFilteredMax = qMax(m_RestFilteredMax, SDL_sqrtf(filteredMagnitude));

    m_Dirty = true;
#else
    Q_UNUSED(event);
#endif
}

void GyroTester::onPublishTimer()
{
    bool receiving = m_LastReadingTime != 0 && !SDL_TICKS_PASSED(SDL_GetTicks(), m_LastReadingTime + RECEIVING_TIMEOUT_MS);
    if (receiving != m_Receiving) {
        m_Receiving = receiving;
        m_Dirty = true;
    }

    if (m_Dirty) {
        m_Dirty = false;
        emit updated();
    }
}

QVariantList GyroTester::trace(int axis, bool filtered) const
{
    QVariantList values;
    if (axis < 0 || axis > 2) {
        return values;
    }

    const QVector<float>& buffer = filtered ? m_Filtered[axis] : m_Raw[axis];
    values.reserve(TRACE_LENGTH);
    for (int i = 0; i < TRACE_LENGTH; i++) {
        values.append(buffer[(m_TraceNext + i) % TRACE_LENGTH]);
    }
    return values;
}

double GyroTester::crosshairRange() const
{
    return CROSSHAIR_RANGE_DEG;
}

int GyroTester::traceLength() const
{
    return TRACE_LENGTH;
}

QString GyroTester::offsetText() const
{
    if (!m_Filter.settings().enabled || !m_Filter.settings().removeOffset) {
        return tr("Off");
    }
    if (!m_Filter.hasOffset()) {
        return tr("Measuring… hold the gamepad still");
    }

    float offset[3];
    m_Filter.offsetDps(offset);
    return tr("%1, %2, %3 °/s").arg(offset[0], 0, 'f', 2).arg(offset[1], 0, 'f', 2).arg(offset[2], 0, 'f', 2);
}

QString GyroTester::accelText() const
{
    if (!m_Filter.settings().enabled || !m_Filter.settings().correctAccel) {
        return tr("Off");
    }
    if (!m_Filter.hasAccelScale()) {
        return tr("Measuring… hold the gamepad still");
    }
    return tr("Scaled by %1").arg(m_Filter.accelScale(), 0, 'f', 3);
}
