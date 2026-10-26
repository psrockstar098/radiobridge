/*
 * RadioBridge — rb_windows_state.h (C++-only shared backend state)
 *
 * COMPILE-UNVERIFIED — written from documented WinRT APIs, never
 * compiled (no Windows SDK on the authoring machine). Requires
 * Windows SDK + a Bluetooth-capable Windows device to verify.
 *
 * This header is included ONLY by the .cpp files, never by the C
 * shim. It defines the backend state and the per-operation records.
 * Ownership rules (read before touching these structs):
 *
 *  - WinState is reference-counted (std::shared_ptr). The C shim
 *    stores a heap "anchor" (a shared_ptr copy) in
 *    ctx->backend_state; rb_win_destroy() drops the anchor, and any
 *    detached worker or in-flight WinRT event handler that still
 *    holds a WinStatePtr keeps the state alive until it finishes.
 *  - Records (BleConnection / BleScan / WifiScan) are held by the
 *    state maps AND by their workers/handlers via shared_ptr, but a
 *    record's back-pointer to the state is RAW (non-owning) to avoid
 *    a reference cycle. Code that touches state through a record
 *    must itself hold a WinStatePtr for the duration.
 *  - ctx_live: set to false by rb_win_destroy() BEFORE the anchor is
 *    dropped. After that point no worker or handler may call
 *    rb_logf(), rb_emit_event(), rb_op_mark_done(), or any user
 *    callback, because the core frees the rb_context_t after
 *    destroy() returns. Every such call site checks ctx_live first
 *    under the state lock.
 *  - Threads come from rb_thread.h (rb_thread_create). The shim has
 *    no condition variable or event primitive, so Win32 HANDLE events
 *    are used ONLY to signal worker completion / stop — never to
 *    create threads. That is the one deliberate exception to
 *    "use the shim", forced by the shim's own surface.
 *  - WinRT objects are agile (BluetoothLEDevice, watchers, adapters
 *    and GATT objects are all marshalled as Agile in the metadata),
 *    so they may be touched from any MTA worker thread. Every worker
 *    thread calls ensure_apartment() (MTA) before its first WinRT
 *    call; WinRT's own event callbacks arrive on WinRT thread-pool
 *    threads that WinRT itself initialized.
 */
#ifndef RADIOBRIDGE_RB_WINDOWS_STATE_H
#define RADIOBRIDGE_RB_WINDOWS_STATE_H

#ifndef _WIN32
#error "The Windows backend compiles on Windows only."
#endif

#include <windows.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.WiFi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include "core/rb_internal.h"

namespace rbwin {

namespace ble = winrt::Windows::Devices::Bluetooth;
namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace en = winrt::Windows::Devices::Enumeration;
namespace wifi = winrt::Windows::Devices::WiFi;

/* ------------------------------------------------------------------ */
/* Small helpers (implemented in rb_windows.cpp)                       */
/* ------------------------------------------------------------------ */

/* Initialize this thread as MTA for WinRT, once per thread. */
void ensure_apartment();

/* Map a WinRT HRESULT to the library taxonomy. */
rb_status_t map_hresult(winrt::hresult hr);

/* Map a caught winrt::hresult_error to the library taxonomy. */
rb_status_t map_hresult_error(const winrt::hresult_error &e);

/* Map a GattCommunicationStatus to the library taxonomy. */
rb_status_t map_gatt(gatt::GattCommunicationStatus status);

/* Map a WiFiConnectionStatus to the library taxonomy. */
rb_status_t map_wifi_connection(wifi::WiFiConnectionStatus status);

/* UUID conversion. rb_uuid_t is network byte order; a WinRT guid is
 * Data1/Data2/Data3 in host (little-endian field) order with Data4
 * in wire order, so the first 8 bytes are shuffled, the last 8 are
 * copied verbatim. */
winrt::guid guid_from_rb(const rb_uuid_t *uuid);
void rb_from_guid(winrt::guid guid, rb_uuid_t *out);

/* Bluetooth address conversion. Wire format is "AA:BB:CC:DD:EE:FF"
 * (most-significant byte first); the WinRT address is a uint64 with
 * the same byte order. Returns 0 on success, -1 on malformed input. */
int parse_address(const char *str, uint64_t *out);
void format_address(uint64_t addr, char *out, size_t out_len);

/* Buffer conversion through DataReader/DataWriter (the documented
 * way to move bytes in and out of WinRT IBuffers). */
std::vector<uint8_t> bytes_from_buffer(winrt::Windows::Storage::Streams::IBuffer buffer);
winrt::Windows::Storage::Streams::IBuffer buffer_from_bytes(const uint8_t *data, size_t len);

/* Case-insensitive substring test (used for the scan name filter). */
int name_contains(const char *name, const char *needle);

/* True while the context is still owned by the core (see header). */
struct WinState;
int ctx_live(WinState *state);

/* ------------------------------------------------------------------ */
/* Shared synchronous-call state                                       */
/*                                                                     */
/* Synchronous public calls (discover/read/write/subscribe/           */
/* unsubscribe, probe, the Wi-Fi SSID pre-check) run their WinRT work  */
/* on a detached worker thread and wait on `done` with a timeout.     */
/* The state is reference-counted: the caller holds one reference,    */
/* the worker holds one. Whoever drops the last reference frees the   */
/* heap services array, the event, and the struct. On timeout the     */
/* caller marks `abandoned` and returns RB_ERROR_TIMEOUT; the worker  */
/* then frees everything itself and never touches caller memory.      */
/* ------------------------------------------------------------------ */

struct SharedState {
    HANDLE done = nullptr;
    rb_mutex_t mu{};
    int refs = 2; /* caller + worker */
    int abandoned = 0;
    rb_status_t status = RB_ERROR_INTERNAL;
    rb_ble_service_t *services = nullptr;
    size_t service_count = 0;
    uint8_t value[RB_BLE_MAX_VALUE_LEN]{};
    size_t value_len = 0;
    rb_capabilities_t caps{};
    int radios = 0;   /* bit 0 = BLE adapter, bit 1 = Wi-Fi adapter */
    int found = 0;    /* generic boolean result (Wi-Fi SSID check) */
    int wifi_permission = 0; /* Wi-Fi access gated (see probe notes) */
};

typedef void (*SharedWorkFn)(SharedState *st, void *arg);

SharedState *shared_new();
void shared_release(SharedState *st);
/* Spawn a detached worker running work(st, arg). The worker always
 * releases its reference; the caller must release its own after
 * shared_wait() (or after a timeout). */
void shared_run(SharedState *st, SharedWorkFn work, void *arg);
/* Wait up to timeout_ms for completion. Returns 1 when the worker
 * finished (st->status and result fields valid), 0 on timeout (the
 * caller must NOT read result fields; it must still call
 * shared_release() after marking abandoned). */
int shared_wait(SharedState *st, unsigned int timeout_ms);

/* ------------------------------------------------------------------ */
/* Records                                                             */
/* ------------------------------------------------------------------ */

struct WinState;

struct BleConnection {
    WinState *state = nullptr; /* RAW: caller must hold a WinStatePtr */
    std::string address;
    uint64_t bt_address = 0;
    rb_ble_connection_callback_t cb = nullptr;
    void *ud = nullptr;
    rb_operation_id_t op = 0;
    rb_ble_connect_options_t opts{};
    ble::BluetoothLEDevice device{nullptr};
    /* Services kept alive so their GattSessions (MaintainConnection)
     * keep the link up for the life of the connection. */
    std::vector<gatt::GattDeviceService> services;
    winrt::event_token conn_token{};
    bool conn_token_valid = false;
    rb_mutex_t mu{};
    bool expecting_disconnect = false;
    bool connected = false;
    bool op_done = false;
    bool need_reconnect = false;
    int attempts = 0;
    HANDLE wake_event = nullptr;
    HANDLE disconnect_event = nullptr;
    BleConnection() {
        rb_mutex_init(&mu);
        wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        disconnect_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~BleConnection() {
        if (wake_event != nullptr) CloseHandle(wake_event);
        if (disconnect_event != nullptr) CloseHandle(disconnect_event);
        rb_mutex_destroy(&mu);
    }
};

struct BleScan {
    WinState *state = nullptr; /* RAW: caller must hold a WinStatePtr */
    rb_ble_scan_options_t opts{};
    rb_ble_scan_callback_t cb = nullptr;
    void *ud = nullptr;
    rb_operation_id_t op = 0;
    adv::BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token received_token{};
    bool received_valid = false;
    winrt::event_token stopped_token{};
    bool stopped_valid = false;
    HANDLE stop_event = nullptr;
    HANDLE started_event = nullptr;
    rb_mutex_t mu{};
    bool stopped = false;
    bool start_failed = false;
    rb_status_t start_status = RB_OK;
    std::set<uint64_t> seen;
    BleScan() {
        rb_mutex_init(&mu);
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        started_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~BleScan() {
        if (stop_event != nullptr) CloseHandle(stop_event);
        if (started_event != nullptr) CloseHandle(started_event);
        rb_mutex_destroy(&mu);
    }
};

struct WifiScan {
    WinState *state = nullptr; /* RAW: caller must hold a WinStatePtr */
    rb_wifi_scan_options_t opts{};
    rb_wifi_scan_callback_t cb = nullptr;
    void *ud = nullptr;
    rb_operation_id_t op = 0;
    wifi::WiFiAdapter adapter{nullptr};
    HANDLE stop_event = nullptr;
    rb_mutex_t mu{};
    bool stopped = false;
    std::set<std::string> seen;
    WifiScan() {
        rb_mutex_init(&mu);
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~WifiScan() {
        if (stop_event != nullptr) CloseHandle(stop_event);
        rb_mutex_destroy(&mu);
    }
};

struct Subscription {
    WinState *state = nullptr; /* RAW: holder must hold a WinStatePtr */
    std::string key;           /* "address|service-uuid|char-uuid" */
    std::string address;
    rb_uuid_t service_uuid{};
    rb_uuid_t characteristic_uuid{};
    gatt::GattCharacteristic characteristic{nullptr};
    winrt::event_token token{};
    bool token_valid = false;
    rb_ble_notify_callback_t cb = nullptr;
    void *ud = nullptr;
};

struct WinState {
    rb_context_t *ctx = nullptr;
    rb_mutex_t lock{};
    int ctx_live_flag = 1;
    rb_reconnect_policy_t policy{};
    std::map<std::string, std::shared_ptr<BleConnection>> connections;
    std::map<rb_operation_id_t, std::shared_ptr<BleScan>> ble_scans;
    std::map<rb_operation_id_t, std::shared_ptr<WifiScan>> wifi_scans;
    std::map<std::string, std::shared_ptr<Subscription>> subscriptions;
    /* Last Wi-Fi scan results, for wifi_get_status() signal lookup. */
    std::vector<rb_wifi_network_t> last_wifi_networks;
    WinState() {
        rb_mutex_init(&lock);
        /* Library default policy (see rb_ble_set_reconnect_policy):
         * enabled, 3 attempts, 500 ms initial delay, 2x backoff. */
        policy.enabled = 1;
        policy.max_attempts = 3;
        policy.initial_delay_ms = 500;
        policy.backoff_factor_x100 = 200;
    }
    ~WinState() { rb_mutex_destroy(&lock); }
};

typedef std::shared_ptr<WinState> WinStatePtr;

/* Fetch the state anchor from ctx->backend_state. Returns an empty
 * shared_ptr when there is no live backend (e.g. after destroy). */
WinStatePtr state_from_ctx(rb_context_t *ctx);

/* Wi-Fi probe half, implemented in rb_windows_wifi.cpp and called
 * from the probe worker in rb_windows.cpp:
 *   returns 1 when a usable Wi-Fi adapter exists,
 *           0 when no adapter exists,
 *          -1 when access is capability-gated (REQUIRES_PERMISSION).
 * Fills *wifi_detail with a short diagnostic (may be empty). */
int wifi_probe_part(char *wifi_detail, size_t detail_len);

} /* namespace rbwin */

#endif /* RADIOBRIDGE_RB_WINDOWS_STATE_H */
