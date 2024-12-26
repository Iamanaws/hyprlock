#include "Fingerprint.hpp"
#include "../core/hyprlock.hpp"
#include "../core/DBusManager.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"

#include <memory>
#include <unistd.h>
#include <pwd.h>

#include <cstring>

static const auto FPRINT        = sdbus::ServiceName{"net.reactivated.Fprint"};
static const auto DEVICE        = sdbus::ServiceName{"net.reactivated.Fprint.Device"};
static const auto MANAGER       = sdbus::ServiceName{"net.reactivated.Fprint.Manager"};
static const auto LOGIN_MANAGER = sdbus::ServiceName{"org.freedesktop.login1.Manager"};
static const auto RETRY_MESSAGE = "Could not match fingerprint. Try again.";

enum MatchResult {
    MATCH_INVALID = 0,
    MATCH_NO_MATCH,
    MATCH_MATCHED,
    MATCH_RETRY,
    MATCH_SWIPE_TOO_SHORT,
    MATCH_FINGER_NOT_CENTERED,
    MATCH_REMOVE_AND_RETRY,
    MATCH_DISCONNECTED,
    MATCH_UNKNOWN_ERROR,
};

static std::map<std::string, MatchResult> s_mapStringToTestType = {{"verify-no-match", MATCH_NO_MATCH},
                                                                   {"verify-match", MATCH_MATCHED},
                                                                   {"verify-retry-scan", MATCH_RETRY},
                                                                   {"verify-swipe-too-short", MATCH_SWIPE_TOO_SHORT},
                                                                   {"verify-finger-not-centered", MATCH_FINGER_NOT_CENTERED},
                                                                   {"verify-remove-and-retry", MATCH_REMOVE_AND_RETRY},
                                                                   {"verify-disconnected", MATCH_DISCONNECTED},
                                                                   {"verify-unknown-error", MATCH_UNKNOWN_ERROR}};

CFingerprint::CFingerprint() {
    static auto* const PFINGERPRINTREADY   = (Hyprlang::STRING*)(g_pConfigManager->getValuePtr("auth:fingerprint:ready_message"));
    m_sFingerprintReady                    = *PFINGERPRINTREADY;
    static auto* const PFINGERPRINTPRESENT = (Hyprlang::STRING*)(g_pConfigManager->getValuePtr("auth:fingerprint:present_message"));
    m_sFingerprintPresent                  = *PFINGERPRINTPRESENT;
}

CFingerprint::~CFingerprint() {
    ;
}

void CFingerprint::init() {
    auto& dbusManager = DBusManager::getInstance();
    m_sDBUSState.connection = dbusManager.getConnection();
    m_sDBUSState.login = dbusManager.getLoginProxy();

    m_sDBUSState.login->getPropertyAsync("PreparingForSleep").onInterface(LOGIN_MANAGER).uponReplyInvoke([this](std::optional<sdbus::Error> e, sdbus::Variant preparingForSleep) {
        if (e) {
            Debug::log(WARN, "fprint: Failed getting value for PreparingForSleep: {}", e->what());
            return;
        }
        m_sDBUSState.sleeping = preparingForSleep.get<bool>();
        // When entering sleep, the wake signal will trigger startVerify().
        if (m_sDBUSState.sleeping)
            return;
        inhibitSleep();
        startVerify();
    });
    
    m_sDBUSState.login->uponSignal("PrepareForSleep").onInterface(LOGIN_MANAGER).call([this](bool start) {
        Debug::log(LOG, "fprint: PrepareForSleep (start: {})", start);
        if (start) {
            m_sDBUSState.sleeping = true;
            stopVerify();
            m_sDBUSState.inhibitLock.reset();
        } else {
            m_sDBUSState.sleeping = false;
            inhibitSleep();
            startVerify();
        }
    });
}

void CFingerprint::handleInput(const std::string& input) {
    ;
}

std::optional<std::string> CFingerprint::getLastFailText() {
    return m_sDBUSState.message.empty() ? std::nullopt : std::optional(m_sDBUSState.message);
}

std::optional<std::string> CFingerprint::getLastPrompt() {
    return std::nullopt;
}

bool CFingerprint::checkWaiting() {
    return false;
}

void CFingerprint::terminate() {
    if (!m_sDBUSState.abort)
        releaseDevice();
}

void CFingerprint::inhibitSleep() {
    m_sDBUSState.login->callMethodAsync("Inhibit")
        .onInterface(LOGIN_MANAGER)
        .withArguments("sleep", "hyprlock", "Fingerprint verifcation must be stopped before sleep", "delay")
        .uponReplyInvoke([this](std::optional<sdbus::Error> e, sdbus::UnixFd fd) {
            if (e)
                Debug::log(WARN, "fprint: could not inhibit sleep: {}", e->what());
            else
                m_sDBUSState.inhibitLock = fd;
        });
}

bool CFingerprint::createDeviceProxy() {
    auto              proxy = sdbus::createProxy(*m_sDBUSState.connection, FPRINT, sdbus::ObjectPath{"/net/reactivated/Fprint/Manager"});

    sdbus::ObjectPath path;
    try {
        proxy->callMethod("GetDefaultDevice").onInterface(MANAGER).storeResultsTo(path);
    } catch (sdbus::Error& e) {
        Debug::log(WARN, "fprint: couldn't connect to Fprint service ({})", e.what());
        return false;
    }
    Debug::log(LOG, "fprint: using device path {}", path.c_str());
    m_sDBUSState.device = sdbus::createProxy(*m_sDBUSState.connection, FPRINT, path);

    m_sDBUSState.device->uponSignal("VerifyFingerSelected").onInterface(DEVICE).call([](const std::string& finger) { Debug::log(LOG, "fprint: finger selected: {}", finger); });
    m_sDBUSState.device->uponSignal("VerifyStatus").onInterface(DEVICE).call([this](const std::string& result, const bool done) { handleVerifyStatus(result, done); });

    m_sDBUSState.device->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interface, const std::map<std::string, sdbus::Variant>& properties) {
            if (interface != DEVICE || m_sDBUSState.done)
                return;

            try {
                const auto presentVariant = properties.at("finger-present");
                bool       isPresent      = presentVariant.get<bool>();
                if (!isPresent)
                    return;
                m_sDBUSState.message = m_sFingerprintPresent;
                g_pHyprlock->enqueueForceUpdateTimers();
            } catch (std::out_of_range& e) {}
        });

    return true;
}

void CFingerprint::handleVerifyStatus(const std::string& result, bool done) {
    g_pAuth->postActivity(AUTH_IMPL_FINGERPRINT);
    Debug::log(LOG, "fprint: handling status {}", result);
    auto matchResult   = s_mapStringToTestType[result];
    bool authenticated = false;
    if (m_sDBUSState.sleeping && matchResult != MATCH_DISCONNECTED)
        return;
    switch (matchResult) {
        case MATCH_INVALID: Debug::log(WARN, "fprint: unknown status: {}", result); break;
        case MATCH_NO_MATCH:
            stopVerify();
            if (m_sDBUSState.retries >= 3) {
                m_sDBUSState.message = "Fingerprint auth disabled (too many failed attempts)";
            } else {
                done = false;
                startVerify(true);
                m_sDBUSState.message = "Fingerprint not matched";
            }
            break;
        case MATCH_UNKNOWN_ERROR:
            stopVerify();
            m_sDBUSState.message = "Fingerprint auth disabled (unknown error)";
            break;
        case MATCH_MATCHED:
            stopVerify();
            m_sDBUSState.message = "";
            authenticated        = true;
            g_pAuth->enqueueUnlock();
            break;
        case MATCH_RETRY: m_sDBUSState.message = "Please retry fingerprint scan"; break;
        case MATCH_SWIPE_TOO_SHORT: m_sDBUSState.message = "Swipe too short - try again"; break;
        case MATCH_FINGER_NOT_CENTERED: m_sDBUSState.message = "Finger not centered - try again"; break;
        case MATCH_REMOVE_AND_RETRY: m_sDBUSState.message = "Remove your finger and try again"; break;
        case MATCH_DISCONNECTED:
            m_sDBUSState.message = "Fingerprint device disconnected";
            m_sDBUSState.abort   = true;
            break;
    }

    if (!authenticated)
        g_pAuth->enqueueFail();

    if (done || m_sDBUSState.abort)
        m_sDBUSState.done = true;
}

void CFingerprint::claimDevice() {
    const auto currentUser = ""; // Empty string means use the caller's id.
    m_sDBUSState.device->callMethodAsync("Claim").onInterface(DEVICE).withArguments(currentUser).uponReplyInvoke([this](std::optional<sdbus::Error> e) {
        if (e)
            Debug::log(WARN, "fprint: could not claim device, {}", e->what());
        else {
            Debug::log(LOG, "fprint: claimed device");
            startVerify();
        }
    });
}

void CFingerprint::startVerify(bool isRetry) {
    if (!m_sDBUSState.device) {
        if (!createDeviceProxy())
            return;

        claimDevice();
        return;
    }
    auto finger = "any"; // Any finger.
    m_sDBUSState.device->callMethodAsync("VerifyStart").onInterface(DEVICE).withArguments(finger).uponReplyInvoke([this, isRetry](std::optional<sdbus::Error> e) {
        if (e) {
            Debug::log(WARN, "fprint: could not start verifying, {}", e->what());
            if (isRetry)
                m_sDBUSState.message = "Fingerprint auth disabled (failed to restart)";
        } else {
            Debug::log(LOG, "fprint: started verifying");
            if (isRetry) {
                m_sDBUSState.retries++;
                m_sDBUSState.message = RETRY_MESSAGE;
            } else
                m_sDBUSState.message = m_sFingerprintReady;
        }
        g_pHyprlock->enqueueForceUpdateTimers();
    });
}

bool CFingerprint::stopVerify() {
    if (!m_sDBUSState.device)
        return false;
    try {
        m_sDBUSState.device->callMethod("VerifyStop").onInterface(DEVICE);
    } catch (sdbus::Error& e) {
        Debug::log(WARN, "fprint: could not stop verifying, {}", e.what());
        return false;
    }
    Debug::log(LOG, "fprint: stopped verification");
    return true;
}

bool CFingerprint::releaseDevice() {
    if (!m_sDBUSState.device)
        return false;
    try {
        m_sDBUSState.device->callMethod("Release").onInterface(DEVICE);
    } catch (sdbus::Error& e) {
        Debug::log(WARN, "fprint: could not release device, {}", e.what());
        return false;
    }
    Debug::log(LOG, "fprint: released device");
    return true;
}
