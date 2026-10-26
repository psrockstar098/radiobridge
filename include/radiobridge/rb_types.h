/*
 * RadioBridge — rb_types.h
 *
 * Shared data model for the public C API: versioning, logging levels,
 * capability states, permissions, backend selection, devices, GATT objects,
 * Wi-Fi objects, and callback types.
 *
 * ABI RULES (see docs/api.md for the full contract):
 *  - No platform-specific object ever appears here: no CBPeripheral, no
 *    Android BluetoothDevice, no Windows Runtime handle. Backends keep
 *    those inside their adapters and translate to these portable structs.
 *  - All structs are plain C11 with fixed-size arrays. No C++ types, no
 *    exceptions, no compiler-specific layout across the boundary.
 *  - Strings are UTF-8, NUL-terminated, and live inside fixed buffers.
 *  - Structs handed to a callback are owned by the library and are valid
 *    ONLY for the duration of that callback. Copy what you need.
 *  - Struct arrays returned by synchronous functions (discovery, list
 *    helpers) are heap-allocated by the library and freed with rb_free().
 *  - The library NEVER logs passwords, pairing secrets, or private device
 *    data. Wi-Fi passwords are passed to rb_wifi_connect() and never
 *    appear in any log line (covered by test_logging).
 */
#ifndef RADIOBRIDGE_RB_TYPES_H
#define RADIOBRIDGE_RB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "rb_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */

#define RB_VERSION_MAJOR 0
#define RB_VERSION_MINOR 1
#define RB_VERSION_PATCH 0
/* Packed ABI version: (major << 16) | (minor << 8) | patch. */
#define RB_API_VERSION \
    ((uint32_t)((RB_VERSION_MAJOR << 16) | (RB_VERSION_MINOR << 8) | \
                RB_VERSION_PATCH))

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

typedef enum rb_log_level {
    RB_LOG_ERROR = 0,
    RB_LOG_WARN = 1,
    RB_LOG_INFO = 2,
    RB_LOG_DEBUG = 3
} rb_log_level_t;

/* Log callback. `message` is valid only for the duration of the call.
 * Implementations must be thread-safe: the library may call this from
 * any internal thread. Never call rb_* functions from a log callback. */
typedef void (*rb_log_callback_t)(rb_log_level_t level,
                                  const char *subsystem,
                                  const char *message, void *user_data);

/* ------------------------------------------------------------------ */
/* Capability states                                                   */
/* ------------------------------------------------------------------ */

/* One state per feature, per backend. This is the capability-negotiation
 * model: a backend reports exactly what it can do, and callers check
 * before calling. Never assume identical capabilities across backends. */
typedef enum rb_cap_state {
    RB_CAP_AVAILABLE = 0,          /* feature works on this backend */
    RB_CAP_UNAVAILABLE = 1,        /* backend present but feature missing
                                      (e.g. no adapter, service down) */
    RB_CAP_REQUIRES_PERMISSION = 2,/* feature gated on an OS permission */
    RB_CAP_NOT_SUPPORTED = 3       /* backend can never do this (platform
                                      limit, e.g. WinRT has no RSSI read) */
} rb_cap_state_t;

typedef struct rb_ble_feature_caps {
    rb_cap_state_t scan;
    rb_cap_state_t connect;
    rb_cap_state_t gatt;       /* service/characteristic discovery */
    rb_cap_state_t notify;     /* notifications + indications */
    rb_cap_state_t pairing;    /* platform-managed pairing/bonding */
    rb_cap_state_t rssi;       /* signal-strength reads */
    rb_cap_state_t reconnect;  /* managed reconnection policy */
} rb_ble_feature_caps_t;

typedef struct rb_wifi_feature_caps {
    rb_cap_state_t status;
    rb_cap_state_t scan;
    rb_cap_state_t connect;
    rb_cap_state_t quality;        /* endpoint latency/stability probes */
    rb_cap_state_t saved_networks; /* platform-managed saved networks */
} rb_wifi_feature_caps_t;

typedef struct rb_capabilities {
    char backend_name[64];     /* "mock", "linux", "android", ... */
    char backend_version[64];  /* backend implementation version */
    char os_name[64];          /* host OS string reported by backend */
    rb_ble_feature_caps_t ble;
    rb_wifi_feature_caps_t wifi;
    /* Human-readable probe detail, e.g. why a backend is unavailable. */
    char detail[512];
} rb_capabilities_t;

/* ------------------------------------------------------------------ */
/* Backends and permissions                                            */
/* ------------------------------------------------------------------ */

typedef enum rb_backend_kind {
    RB_BACKEND_AUTO = 0,   /* prefer the native backend for this host;
                              fall back to mock (with a warning log) when
                              the native backend is unavailable */
    RB_BACKEND_MOCK = 1,   /* deterministic fake radios, no hardware */
    RB_BACKEND_LINUX = 2,  /* BlueZ D-Bus + NetworkManager */
    RB_BACKEND_ANDROID = 3,/* Android Bluetooth + connectivity via JNI */
    RB_BACKEND_APPLE = 4,  /* Core Bluetooth + Network framework */
    RB_BACKEND_WINDOWS = 5 /* Windows Runtime Bluetooth + WLAN APIs */
} rb_backend_kind_t;

/* Permission bits. The library never requests permissions itself — it
 * REPORTS what the OS requires for an operation (rb_get_required_
 * permissions) so the application can drive the OS-managed flow
 * (Android runtime request, iOS Info.plist usage description, Windows
 * app-capability declaration, Linux PolicyKit). */
typedef uint32_t rb_permission_mask_t;
#define RB_PERM_NONE               ((rb_permission_mask_t)0)
#define RB_PERM_BLUETOOTH_SCAN     ((rb_permission_mask_t)(1u << 0))
#define RB_PERM_BLUETOOTH_CONNECT  ((rb_permission_mask_t)(1u << 1))
#define RB_PERM_BLUETOOTH_ADVERTISE ((rb_permission_mask_t)(1u << 2))
#define RB_PERM_LOCATION           ((rb_permission_mask_t)(1u << 3))
#define RB_PERM_WIFI_SCAN          ((rb_permission_mask_t)(1u << 4))
#define RB_PERM_WIFI_CONNECT       ((rb_permission_mask_t)(1u << 5))
#define RB_PERM_NEARBY_DEVICES     ((rb_permission_mask_t)(1u << 6))

typedef enum rb_operation {
    RB_OP_BLE_SCAN = 0,
    RB_OP_BLE_CONNECT = 1,
    RB_OP_BLE_READ = 2,
    RB_OP_BLE_WRITE = 3,
    RB_OP_BLE_NOTIFY = 4,
    RB_OP_WIFI_SCAN = 5,
    RB_OP_WIFI_CONNECT = 6,
    RB_OP_WIFI_STATUS = 7,
    RB_OP_WIFI_QUALITY = 8
} rb_operation_t;

/* ------------------------------------------------------------------ */
/* Context, operations, events                                         */
/* ------------------------------------------------------------------ */

typedef struct rb_context rb_context_t;
typedef uint64_t rb_operation_id_t; /* 0 is never a valid operation id */

typedef enum rb_event_type {
    RB_EVENT_WIFI_CHANGED = 0, /* Wi-Fi state/network changed */
    RB_EVENT_BACKEND_STATE = 1,/* backend availability changed */
    RB_EVENT_ERROR = 2         /* backend-level error not tied to one op */
} rb_event_type_t;

typedef struct rb_event {
    rb_event_type_t type;
    rb_status_t status;        /* RB_OK unless this event reports a failure */
    char message[256];
} rb_event_t;

/* General event callback (Wi-Fi changes, backend state, errors).
 * Fired from internal threads; must be thread-safe. Valid only during
 * the call. Never call rb_shutdown() from an event callback. */
typedef void (*rb_event_callback_t)(rb_context_t *context,
                                    const rb_event_t *event,
                                    void *user_data);

typedef struct rb_config {
    rb_backend_kind_t backend;          /* default: RB_BACKEND_AUTO */
    rb_log_level_t log_level;           /* default: RB_LOG_WARN */
    rb_log_callback_t log_callback;     /* NULL = default stderr logger */
    void *log_user_data;
    rb_event_callback_t event_callback; /* NULL = events dropped */
    void *event_user_data;
} rb_config_t;

/* ------------------------------------------------------------------ */
/* Bluetooth data model                                                */
/* ------------------------------------------------------------------ */

/* 128-bit UUID in network byte order. Use rb_uuid_from_string() /
 * rb_uuid_to_string() / rb_uuid_from_16() — never hand-roll bytes. */
typedef struct rb_uuid {
    uint8_t bytes[16];
} rb_uuid_t;

#define RB_BLE_MAX_SERVICE_UUIDS 16
#define RB_BLE_MAX_MANUFACTURER_DATA 32
#define RB_BLE_MAX_CHARS_PER_SERVICE 16
#define RB_BLE_MAX_DESCS_PER_CHAR 8

typedef struct rb_ble_device {
    char address[32];      /* BLE address "AA:BB:CC:DD:EE:FF" or, on Apple
                              backends, the platform UUID string — the
                              public API never exposes CBPeripheral */
    char name[128];        /* advertised name, may be empty */
    int8_t rssi_dbm;       /* e.g. -52; 0 means "not reported" */
    int has_tx_power;
    int8_t tx_power_dbm;
    int connectable;
    rb_uuid_t service_uuids[RB_BLE_MAX_SERVICE_UUIDS];
    size_t service_uuid_count;
    int has_manufacturer_data;
    uint16_t manufacturer_id;
    uint8_t manufacturer_data[RB_BLE_MAX_MANUFACTURER_DATA];
    size_t manufacturer_data_len;
} rb_ble_device_t;

/* Opaque portable device handle for connect/disconnect. This is an
 * address string copy owned by the caller — never a platform object. */
typedef struct rb_device_id {
    char address[32];
} rb_device_id_t;

typedef struct rb_ble_scan_options {
    rb_uuid_t service_uuids[8]; /* empty list = match any service */
    size_t service_uuid_count;
    int manufacturer_id;        /* -1 = any manufacturer */
    int has_min_rssi;           /* 0 = no RSSI floor */
    int8_t min_rssi_dbm;        /* inclusive floor, e.g. -70 */
    char name_substring[128];   /* empty = any name */
    uint32_t timeout_ms;        /* 0 = run until stopped/cancelled */
    int allow_duplicates;       /* real stacks may re-deliver; mock
                                   delivers each device once per scan */
} rb_ble_scan_options_t;

typedef enum rb_ble_connection_state {
    RB_BLE_DISCONNECTED = 0,
    RB_BLE_CONNECTING = 1,
    RB_BLE_CONNECTED = 2,
    RB_BLE_DISCONNECTING = 3,
    RB_BLE_FAILED = 4
} rb_ble_connection_state_t;

typedef struct rb_ble_connection_event {
    rb_device_id_t device;
    rb_ble_connection_state_t state;
    rb_status_t status;         /* RB_OK unless state is FAILED */
    char message[256];
} rb_ble_connection_event_t;

typedef struct rb_ble_connect_options {
    uint32_t timeout_ms;        /* 0 = backend default */
    int auto_reconnect;         /* apply reconnect policy on drop */
    uint32_t max_reconnect_attempts; /* 0 = policy default */
    uint32_t reconnect_delay_ms;    /* 0 = policy default */
} rb_ble_connect_options_t;

/* Managed reconnection policy (mock + future native backends). */
typedef struct rb_reconnect_policy {
    int enabled;
    uint32_t max_attempts;      /* 0 = unlimited while context lives */
    uint32_t initial_delay_ms;
    uint32_t backoff_factor_x100; /* 200 = 2x per attempt */
} rb_reconnect_policy_t;

/* GATT property bits (mirror the standard GATT property set). */
#define RB_GATT_PROP_BROADCAST   ((uint32_t)(1u << 0))
#define RB_GATT_PROP_READ        ((uint32_t)(1u << 1))
#define RB_GATT_PROP_WRITE_NO_RSP ((uint32_t)(1u << 2))
#define RB_GATT_PROP_WRITE       ((uint32_t)(1u << 3))
#define RB_GATT_PROP_NOTIFY      ((uint32_t)(1u << 4))
#define RB_GATT_PROP_INDICATE    ((uint32_t)(1u << 5))

typedef struct rb_ble_descriptor {
    rb_uuid_t uuid;
    uint16_t handle;
} rb_ble_descriptor_t;

typedef struct rb_ble_characteristic {
    rb_uuid_t uuid;
    uint16_t handle;
    uint32_t properties;        /* RB_GATT_PROP_* bits */
    rb_ble_descriptor_t descriptors[RB_BLE_MAX_DESCS_PER_CHAR];
    size_t descriptor_count;
} rb_ble_characteristic_t;

typedef struct rb_ble_service {
    rb_uuid_t uuid;
    uint16_t handle;
    rb_ble_characteristic_t characteristics[RB_BLE_MAX_CHARS_PER_SERVICE];
    size_t characteristic_count;
} rb_ble_service_t;

#define RB_BLE_MAX_VALUE_LEN 512

typedef struct rb_ble_notification {
    rb_device_id_t device;
    rb_uuid_t service_uuid;
    rb_uuid_t characteristic_uuid;
    uint8_t data[RB_BLE_MAX_VALUE_LEN];
    size_t data_len;
} rb_ble_notification_t;

/* Scan callback: one call per discovered device. `device` is valid only
 * during the call. Called from an internal thread. */
typedef void (*rb_ble_scan_callback_t)(rb_context_t *context,
                                       const rb_ble_device_t *device,
                                       void *user_data);

/* Connection-state callback: CONNECTING/CONNECTED/DISCONNECTED/FAILED
 * transitions, plus reconnect attempts (message names the attempt). */
typedef void (*rb_ble_connection_callback_t)(
    rb_context_t *context, const rb_ble_connection_event_t *event,
    void *user_data);

/* Notification/indication callback. `notification` valid only during
 * the call. */
typedef void (*rb_ble_notify_callback_t)(rb_context_t *context,
                                         const rb_ble_notification_t *notif,
                                         void *user_data);

/* ------------------------------------------------------------------ */
/* Wi-Fi data model                                                    */
/* ------------------------------------------------------------------ */

typedef enum rb_wifi_security {
    RB_WIFI_SEC_OPEN = 0,
    RB_WIFI_SEC_WEP = 1,
    RB_WIFI_SEC_WPA = 2,
    RB_WIFI_SEC_WPA2 = 3,
    RB_WIFI_SEC_WPA3 = 4,
    RB_WIFI_SEC_ENTERPRISE = 5,
    RB_WIFI_SEC_UNKNOWN = 6
} rb_wifi_security_t;

typedef struct rb_wifi_network {
    char ssid[64];              /* may be empty for hidden networks */
    char bssid[32];
    int8_t signal_dbm;          /* e.g. -45; 0 = not reported */
    uint32_t frequency_mhz;     /* 2412, 5180, ...; 0 = unknown */
    uint32_t channel;           /* 0 = unknown */
    rb_wifi_security_t security;
} rb_wifi_network_t;

typedef enum rb_wifi_state {
    RB_WIFI_DISCONNECTED = 0,
    RB_WIFI_CONNECTING = 1,
    RB_WIFI_CONNECTED = 2,
    RB_WIFI_UNAVAILABLE = 3,    /* no Wi-Fi hardware/service */
    RB_WIFI_PERMISSION_DENIED = 4,
    RB_WIFI_FAILED = 5
} rb_wifi_state_t;

/* Diagnostic code distinguishing the failure families the spec calls
 * for: disconnected vs unavailable vs permission-denied vs failed. */
typedef enum rb_wifi_diag {
    RB_WIFI_DIAG_OK = 0,
    RB_WIFI_DIAG_DISCONNECTED = 1,
    RB_WIFI_DIAG_UNAVAILABLE = 2,
    RB_WIFI_DIAG_PERMISSION_DENIED = 3,
    RB_WIFI_DIAG_FAILED = 4,
    RB_WIFI_DIAG_AUTH_FAILED = 5
} rb_wifi_diag_t;

typedef struct rb_wifi_status {
    rb_wifi_state_t state;
    rb_wifi_diag_t diag;
    char ssid[64];              /* current network, if connected */
    char bssid[32];
    char ip_address[64];        /* dotted quad, if connected */
    char gateway[64];
    int8_t signal_dbm;
    char message[256];          /* human-readable diagnostic detail */
} rb_wifi_status_t;

typedef struct rb_wifi_scan_options {
    uint32_t timeout_ms;        /* 0 = backend default */
} rb_wifi_scan_options_t;

typedef struct rb_wifi_connect_options {
    uint32_t timeout_ms;        /* 0 = backend default */
} rb_wifi_connect_options_t;

/* Network-quality probe result. On the mock backend the values are
 * simulated from signal strength and documented as simulated; on real
 * backends they come from real endpoint probes. */
typedef struct rb_wifi_quality {
    char endpoint[128];
    int reachable;
    double latency_ms;
    double jitter_ms;
    double packet_loss_percent;
} rb_wifi_quality_t;

typedef void (*rb_wifi_scan_callback_t)(rb_context_t *context,
                                        const rb_wifi_network_t *network,
                                        void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_TYPES_H */
