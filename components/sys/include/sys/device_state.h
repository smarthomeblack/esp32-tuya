#pragma once

/// Hub lifecycle states (shared by application, LED, web status).
enum DeviceState {
    kDeviceStateStarting = 0,     ///< Boot in progress
    kDeviceStateWifiConfig,       ///< AP provisioning mode (no WiFi credentials)
    kDeviceStateNeedsSetup,       ///< WiFi up, Tuya account not linked yet
    kDeviceStateIdle,             ///< Running, cloud services not active
    kDeviceStateCloudConnecting,  ///< Discovery / MQTT setup in progress
    kDeviceStateLinked,           ///< Fully operational
    kDeviceStateFatalError,       ///< Unrecoverable error
    kDeviceStateCount,            ///< Sentinel (keep last)
};
