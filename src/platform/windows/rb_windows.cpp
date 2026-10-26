/*
 * RadioBridge — rb_windows.cpp (Windows backend: BLE + lifecycle)
 *
 * COMPILE-UNVERIFIED — written from documented WinRT APIs, never
 * compiled (no Windows SDK on the authoring machine). Requires
 * Windows SDK + a Bluetooth-capable Windows device to verify.
 *
 * Language choice: C++ with C++/WinRT, behind the extern "C" shim
 * in rb_windows.c. The alternative — raw COM ABI calls in C11 —
 * would mean hand-writing every WinRT interface vtable (including
 * the async-operation and event-handler machinery) with no
 * projection to check against. C++/WinRT is what a real project
 * ships here (SimpleBLE uses it): it is a header-only standard-C++
 * projection, so the public library stays C11 while only these
 * Windows-only translation units compile as C++17. See
 * rb_windows_errors.md for the integration snippet.
 *
 * Architecture (see rb_windows_state.h for ownership rules):
 *  - BLE scanning: one detached worker thread owns a
 *    BluetoothLEAdvertisementWatcher (Active mode, so scan responses
 *    — and therefore local names — arrive). Received events are
 *    filtered in software (service UUIDs, manufacturer id, RSSI
 *    floor, name substring) and delivered to the user callback from
 *    the WinRT thread-pool thread, never from the caller's thread.
 *  - BLE connect: one detached worker thread per device. Creating
 *    the BluetoothLEDevice alone does not necessarily connect, so
 *    the worker forces the connection with an uncached GATT
 *    discovery (GetGattServicesAsync(BluetoothCacheMode::Uncached))
 *    and then sets GattSession.MaintainConnection = true on every
 *    discovered service's session to hold the link. Connection loss
 *    arrives via ConnectionStatusChanged; managed reconnection is
 *    driven by the worker, not by the OS alone.
 *  - Synchronous GATT calls (discover/read/write/subscribe/
 *    unsubscribe) run their WinRT work on a detached worker thread
 *    and the caller waits on a completion event with a timeout, so
 *    no caller thread ever blocks on a WinRT async object directly.
 *  - Disconnect is best-effort and asynchronous by platform design:
 *    WinRT has no Disconnect method. This backend clears
 *    MaintainConnection, closes every service handle and the device
 *    handle, and lets the OS drop the link (after a small timeout,
 *    and only if no other app holds the device). rb_ble_disconnect()
 *    therefore returns once teardown has been REQUESTED, not once
 *    the link is physically down; ConnectionStatusChanged (or the
 *    general event on explicit disconnect) reports the real state.
 *  - Pairing is platform-managed only. The public vtable has no
 *    pairing slot, so pairing lives in helpers below: basic pairing
 *    (DeviceInformationPairing::PairAsync) is attempted once when an
 *    uncached discovery fails with AccessDenied, and custom pairing
 *    (ConfirmOnly auto-accept, ProvidePin with a caller-supplied
 *    PIN) is implemented for a future public pairing entry point.
 *    No PIN is ever logged.
 *  - RSSI: WinRT exposes signal strength ONLY on advertisement
 *    reports (RawSignalStrengthInDBm). There is no connected-device
 *    RSSI API, so ble_get_rssi() always returns
 *    RB_ERROR_NOT_SUPPORTED and the capability is NOT_SUPPORTED —
 *    never a fabricated number. Scan results DO carry RSSI, because
 *    that number genuinely comes from the advertisement.
 *  - Cancellation: WinRT cannot cancel an in-flight connection
 *    attempt or GATT request (documented in the GATT client guide).
 *    Scans stop promptly (watcher.Stop()); connects and GATT
 *    requests run to completion or their backend timeout, and a
 *    disconnect requested mid-attempt is honored as soon as the
 *    attempt resolves. rb_cancel() on a scan maps to ble_scan_stop()
 *    through the core.
 */
#ifndef _WIN32
#error "The Windows backend compiles on Windows only."
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "platform/windows/rb_windows_internal.h"
#include "platform/windows/rb_windows_state.h"

namespace rbwin {

/* ------------------------------------------------------------------ */
/* Apartment, errors, conversions                                      */
/* ------------------------------------------------------------------ */

void ensure_apartment() {
    thread_local bool initialized = false;
    if (!initialized) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        initialized = true;
    }
}

rb_status_t map_hresult(winrt::hresult hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0: return RB_OK;
        case 0x80070005: /* E_ACCESSDENIED */
            return RB_ERROR_PERMISSION_DENIED;
        case 0x80070490: /* HRESULT_FROM_WIN32(ERROR_NOT_FOUND) */
        case 0x80070491: /* ERROR_NOT_FOUND variant family */
            return RB_ERROR_NOT_FOUND;
        case 0x80070057: /* E_INVALIDARG */
            return RB_ERROR_INVALID_ARGUMENT;
        case 0x8007000E: /* E_OUTOFMEMORY */
            return RB_ERROR_NO_MEMORY;
        case 0x800704C7: /* HRESULT_FROM_WIN32(ERROR_CANCELLED) */
            return RB_ERROR_CANCELLED;
        default: return RB_ERROR_INTERNAL;
    }
}

rb_status_t map_hresult_error(const winrt::hresult_error &e) {
    return map_hresult(e.code());
}

rb_status_t map_gatt(gatt::GattCommunicationStatus status) {
    switch (status) {
        case gatt::GattCommunicationStatus::Success:
            return RB_OK;
        case gatt::GattCommunicationStatus::Unreachable:
            return RB_ERROR_NOT_CONNECTED;
        case gatt::GattCommunicationStatus::ProtocolError:
            return RB_ERROR_INTERNAL;
        case gatt::GattCommunicationStatus::AccessDenied:
            return RB_ERROR_PERMISSION_DENIED;
        default:
            return RB_ERROR_INTERNAL;
    }
}

winrt::guid guid_from_rb(const rb_uuid_t *uuid) {
    const uint8_t *b = uuid->bytes;
    uint32_t d1 = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                  ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    uint16_t d2 = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
    uint16_t d3 = (uint16_t)(((uint16_t)b[6] << 8) | b[7]);
    winrt::guid g{};
    g.Data1 = d1;
    g.Data2 = d2;
    g.Data3 = d3;
    for (int i = 0; i < 8; i++) g.Data4[i] = b[8 + i];
    return g;
}

void rb_from_guid(winrt::guid guid, rb_uuid_t *out) {
    uint8_t *b = out->bytes;
    b[0] = (uint8_t)(guid.Data1 >> 24);
    b[1] = (uint8_t)(guid.Data1 >> 16);
    b[2] = (uint8_t)(guid.Data1 >> 8);
    b[3] = (uint8_t)guid.Data1;
    b[4] = (uint8_t)(guid.Data2 >> 8);
    b[5] = (uint8_t)guid.Data2;
    b[6] = (uint8_t)(guid.Data3 >> 8);
    b[7] = (uint8_t)guid.Data3;
    for (int i = 0; i < 8; i++) b[8 + i] = guid.Data4[i];
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int parse_address(const char *str, uint64_t *out) {
    if (str == nullptr || out == nullptr) return -1;
    /* Accept "AA:BB:CC:DD:EE:FF" or a plain 12-hex-digit string. */
    char hex[13];
    size_t n = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == ':' || str[i] == '-') continue;
        if (n >= 12) return -1;
        if (hex_val(str[i]) < 0) return -1;
        hex[n++] = str[i];
    }
    if (n != 12) return -1;
    hex[12] = '\0';
    uint64_t addr = 0;
    for (int i = 0; i < 6; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        addr = (addr << 8) | (uint64_t)((hi << 4) | lo);
    }
    *out = addr;
    return 0;
}

void format_address(uint64_t addr, char *out, size_t out_len) {
    if (out == nullptr || out_len < 18) {
        if (out != nullptr && out_len > 0) out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)((addr >> 40) & 0xff),
             (unsigned)((addr >> 32) & 0xff),
             (unsigned)((addr >> 24) & 0xff),
             (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 8) & 0xff),
             (unsigned)(addr & 0xff));
}

std::vector<uint8_t> bytes_from_buffer(
    winrt::Windows::Storage::Streams::IBuffer buffer) {
    std::vector<uint8_t> bytes;
    if (!buffer) return bytes;
    auto reader =
        winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
    uint32_t len = reader.UnconsumedBufferLength();
    bytes.resize(len);
    if (len > 0)
        reader.ReadBytes(
            winrt::array_view<uint8_t>(bytes.data(), len));
    return bytes;
}

winrt::Windows::Storage::Streams::IBuffer buffer_from_bytes(
    const uint8_t *data, size_t len) {
    winrt::Windows::Storage::Streams::DataWriter writer;
    if (len > 0 && data != nullptr) {
        writer.WriteBytes(winrt::array_view<const uint8_t>(
            data, (uint32_t)len));
    }
    return writer.DetachBuffer();
}

int name_contains(const char *name, const char *needle) {
    if (name == nullptr || needle == nullptr || needle[0] == '\0')
        return 1;
    std::string hay(name), ndl(needle);
    for (char &c : hay)
        c = (char)tolower((unsigned char)c);
    for (char &c : ndl)
        c = (char)tolower((unsigned char)c);
    return hay.find(ndl) != std::string::npos;
}

int ctx_live(WinState *state) {
    if (state == nullptr) return 0;
    rb_mutex_lock(&state->lock);
    int live = state->ctx_live_flag;
    rb_mutex_unlock(&state->lock);
    return live;
}

WinStatePtr state_from_ctx(rb_context_t *ctx) {
    if (ctx == nullptr || ctx->backend_state == nullptr)
        return WinStatePtr();
    return *static_cast<WinStatePtr *>(ctx->backend_state);
}

static void win_log(WinState *state, rb_log_level_t level,
                    const char *subsystem, const char *message) {
    if (state == nullptr) return;
    rb_mutex_lock(&state->lock);
    int live = state->ctx_live_flag;
    rb_context_t *ctx = state->ctx;
    rb_mutex_unlock(&state->lock);
    if (live && ctx != nullptr)
        rb_logf(ctx, level, subsystem, "%s", message);
}

/* ------------------------------------------------------------------ */
/* Shared synchronous-call machinery                                   */
/* ------------------------------------------------------------------ */

SharedState *shared_new() {
    SharedState *st = new SharedState();
    st->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rb_mutex_init(&st->mu);
    return st;
}

void shared_release(SharedState *st) {
    if (st == nullptr) return;
    rb_mutex_lock(&st->mu);
    int refs = --st->refs;
    rb_mutex_unlock(&st->mu);
    if (refs == 0) {
        if (st->services != nullptr) free(st->services);
        if (st->done != nullptr) CloseHandle(st->done);
        rb_mutex_destroy(&st->mu);
        delete st;
    }
}

struct SharedHolder {
    SharedState *st;
    SharedWorkFn work;
    void *arg;
};

extern "C" void *rb_win_shared_thread(void *p) {
    std::unique_ptr<SharedHolder> holder(static_cast<SharedHolder *>(p));
    SharedState *st = holder->st;
    ensure_apartment();
    try {
        holder->work(st, holder->arg);
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
    rb_mutex_lock(&st->mu);
    int abandoned = st->abandoned;
    rb_mutex_unlock(&st->mu);
    if (!abandoned && st->done != nullptr) SetEvent(st->done);
    shared_release(st);
    return nullptr;
}

void shared_run(SharedState *st, SharedWorkFn work, void *arg) {
    SharedHolder *holder = new SharedHolder();
    holder->st = st;
    holder->work = work;
    holder->arg = arg;
    rb_thread_t th = RB_THREAD_INVALID;
    if (rb_thread_create(&th, rb_win_shared_thread, holder) != 0) {
        /* Thread spawn failed: report synchronously on the caller. */
        delete holder;
        rb_mutex_lock(&st->mu);
        st->status = RB_ERROR_INTERNAL;
        rb_mutex_unlock(&st->mu);
        if (st->done != nullptr) SetEvent(st->done);
        shared_release(st);
        return;
    }
    rb_thread_detach(th);
}

int shared_wait(SharedState *st, unsigned int timeout_ms) {
    if (st->done == nullptr) return 0;
    DWORD r = WaitForSingleObject(st->done, (DWORD)timeout_ms);
    if (r == WAIT_OBJECT_0) return 1;
    rb_mutex_lock(&st->mu);
    st->abandoned = 1;
    rb_mutex_unlock(&st->mu);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

struct ProbeArg {
    rb_context_t *ctx; /* may be NULL (standalone probe): no logging */
};

static void probe_work(SharedState *st, void *arg) {
    std::unique_ptr<ProbeArg> parg(static_cast<ProbeArg *>(arg));
    rb_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    rb_copy_str(caps.backend_name, sizeof(caps.backend_name), "windows");
    rb_copy_str(caps.backend_version, sizeof(caps.backend_version),
                "0.1.0");
    rb_copy_str(caps.os_name, sizeof(caps.os_name), "windows");

    int ble_ok = 0;
    try {
        auto adapter = ble::BluetoothAdapter::GetDefaultAsync().get();
        ble_ok = (adapter != nullptr) ? 1 : 0;
    } catch (...) {
        ble_ok = 0;
    }
    if (ble_ok) st->radios |= 1;

    char wifi_detail[192] = "";
    int wifi = wifi_probe_part(wifi_detail, sizeof(wifi_detail));
    if (wifi == 1) {
        st->radios |= 2;
    } else if (wifi == -1) {
        st->wifi_permission = 1;
    }

    rb_cap_state_t ble_state =
        ble_ok ? RB_CAP_AVAILABLE : RB_CAP_UNAVAILABLE;
    caps.ble.scan = ble_state;
    caps.ble.connect = ble_state;
    caps.ble.gatt = ble_state;
    caps.ble.notify = ble_state;
    caps.ble.pairing = ble_state;
    caps.ble.reconnect = ble_state;
    /* WinRT has no connected-device RSSI API. See the file header. */
    caps.ble.rssi = RB_CAP_NOT_SUPPORTED;

    rb_cap_state_t wifi_state;
    if (wifi == 1) {
        wifi_state = RB_CAP_AVAILABLE;
    } else if (wifi == -1) {
        wifi_state = RB_CAP_REQUIRES_PERMISSION;
    } else {
        wifi_state = RB_CAP_UNAVAILABLE;
    }
    caps.wifi.status = wifi_state;
    caps.wifi.scan = wifi_state;
    caps.wifi.connect = wifi_state;
    /* No WinRT API for endpoint quality probes or saved networks. */
    caps.wifi.quality = RB_CAP_NOT_SUPPORTED;
    caps.wifi.saved_networks = RB_CAP_NOT_SUPPORTED;

    snprintf(caps.detail, sizeof(caps.detail),
             "windows backend: COMPILE-UNVERIFIED (see "
             "src/platform/windows/rb_windows_errors.md); bluetooth "
             "adapter %s; wifi %s%s%s",
             ble_ok ? "present" : "absent",
             wifi == 1 ? "adapter present"
                       : (wifi == -1 ? "capability-gated"
                                     : "no adapter"),
             wifi_detail[0] != '\0' ? " (" : "",
             wifi_detail[0] != '\0' ? wifi_detail : "");
    /* Close the dangling "(" when a wifi detail was appended. */
    if (wifi_detail[0] != '\0') {
        size_t len = strlen(caps.detail);
        if (len + 2 < sizeof(caps.detail)) {
            caps.detail[len] = ')';
            caps.detail[len + 1] = '\0';
        }
    }
    st->caps = caps;
    st->status = RB_OK;
}

} /* namespace rbwin */

/* ------------------------------------------------------------------ */
/* extern "C" entry points: probe / create / destroy                   */
/* ------------------------------------------------------------------ */

extern "C" rb_status_t rb_win_probe(rb_capabilities_t *caps) {
    if (caps == nullptr) return RB_ERROR_INVALID_ARGUMENT;
    rbwin::SharedState *st = rbwin::shared_new();
    rbwin::ProbeArg *arg = new rbwin::ProbeArg();
    arg->ctx = nullptr;
    rbwin::shared_run(st, rbwin::probe_work, arg);
    if (rbwin::shared_wait(st, 15000)) {
        *caps = st->caps;
        rb_status_t status = st->status;
        rbwin::shared_release(st);
        return status;
    }
    rbwin::shared_release(st);
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), "windows");
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.1.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), "windows");
    caps->ble.scan = RB_CAP_UNAVAILABLE;
    caps->ble.connect = RB_CAP_UNAVAILABLE;
    caps->ble.gatt = RB_CAP_UNAVAILABLE;
    caps->ble.notify = RB_CAP_UNAVAILABLE;
    caps->ble.pairing = RB_CAP_UNAVAILABLE;
    caps->ble.rssi = RB_CAP_NOT_SUPPORTED;
    caps->ble.reconnect = RB_CAP_UNAVAILABLE;
    caps->wifi.status = RB_CAP_UNAVAILABLE;
    caps->wifi.scan = RB_CAP_UNAVAILABLE;
    caps->wifi.connect = RB_CAP_UNAVAILABLE;
    caps->wifi.quality = RB_CAP_NOT_SUPPORTED;
    caps->wifi.saved_networks = RB_CAP_NOT_SUPPORTED;
    rb_copy_str(caps->detail, sizeof(caps->detail),
                "windows backend: COMPILE-UNVERIFIED; probe timed out "
                "after 15s, treating every radio as unavailable");
    return RB_OK;
}

extern "C" rb_status_t rb_win_create(rb_context_t *ctx) {
    if (ctx == nullptr) return RB_ERROR_INVALID_ARGUMENT;
    rbwin::WinStatePtr state = std::make_shared<rbwin::WinState>();
    state->ctx = ctx;
    rbwin::WinStatePtr *anchor = new rbwin::WinStatePtr(state);
    ctx->backend_state = anchor;
    /* Gate creation on the probe: fail fast when there is no usable
     * radio at all. A capability-gated Wi-Fi stack (no BLE adapter,
     * Wi-Fi present but permission-gated) still creates, so the
     * capability report can say REQUIRES_PERMISSION instead of
     * silently behaving like a missing radio. */
    rbwin::SharedState *st = rbwin::shared_new();
    rbwin::ProbeArg *arg = new rbwin::ProbeArg();
    arg->ctx = ctx;
    rbwin::shared_run(st, rbwin::probe_work, arg);
    int ok = 0;
    if (rbwin::shared_wait(st, 15000)) {
        ok = (st->radios != 0) || (st->wifi_permission != 0);
    }
    rbwin::shared_release(st);
    if (!ok) {
        ctx->backend_state = nullptr;
        delete anchor;
        return RB_ERROR_UNAVAILABLE;
    }
    return RB_OK;
}

extern "C" void rb_win_destroy(rb_context_t *ctx) {
    if (ctx == nullptr) return;
    /* Capture the anchor BEFORE clearing backend_state; deleting it
     * drops the state's creation reference, and workers/handlers
     * holding their own WinStatePtr free the state when they
     * finish. */
    rbwin::WinStatePtr *anchor =
        static_cast<rbwin::WinStatePtr *>(ctx->backend_state);
    if (anchor == nullptr) {
        ctx->backend_state = nullptr;
        return;
    }
    rbwin::WinStatePtr state = *anchor;
    /* Flip the live flag FIRST: from here on no worker or handler
     * may touch the context (the core frees it after destroy
     * returns). Then signal every stop/disconnect event and stop
     * every watcher best-effort. */
    rb_mutex_lock(&state->lock);
    state->ctx_live_flag = 0;
    std::vector<std::shared_ptr<rbwin::BleScan>> scans;
    std::vector<std::shared_ptr<rbwin::WifiScan>> wscans;
    std::vector<std::shared_ptr<rbwin::BleConnection>> conns;
    for (auto &kv : state->ble_scans) scans.push_back(kv.second);
    for (auto &kv : state->wifi_scans) wscans.push_back(kv.second);
    for (auto &kv : state->connections) conns.push_back(kv.second);
    rb_mutex_unlock(&state->lock);
    for (auto &scan : scans) {
        try {
            if (scan->watcher != nullptr) scan->watcher.Stop();
        } catch (...) {
        }
        if (scan->stop_event != nullptr) SetEvent(scan->stop_event);
    }
    for (auto &ws : wscans) {
        if (ws->stop_event != nullptr) SetEvent(ws->stop_event);
    }
    for (auto &conn : conns) {
        rb_mutex_lock(&conn->mu);
        conn->expecting_disconnect = true;
        rb_mutex_unlock(&conn->mu);
        if (conn->disconnect_event != nullptr)
            SetEvent(conn->disconnect_event);
    }
    ctx->backend_state = nullptr;
    delete anchor;
}

namespace rbwin {

/* ------------------------------------------------------------------ */
/* Connection callback + op bookkeeping helpers                        */
/* ------------------------------------------------------------------ */

static void emit_conn(WinStatePtr state,
                      const std::shared_ptr<BleConnection> &conn,
                      rb_ble_connection_state_t st, rb_status_t status,
                      const char *message) {
    if (!ctx_live(state.get())) return;
    if (conn->cb == nullptr) return;
    rb_ble_connection_event_t ev;
    memset(&ev, 0, sizeof(ev));
    rb_copy_str(ev.device.address, sizeof(ev.device.address),
                conn->address.c_str());
    ev.state = st;
    ev.status = status;
    rb_copy_str(ev.message, sizeof(ev.message),
                message != nullptr ? message : "");
    conn->cb(state->ctx, &ev, conn->ud);
}

static void mark_done_live(WinStatePtr state, rb_operation_id_t op) {
    if (!ctx_live(state.get())) return;
    rb_op_mark_done(state->ctx, op);
}

static void emit_general(WinStatePtr state, rb_event_type_t type,
                         rb_status_t status, const char *message) {
    if (!ctx_live(state.get())) return;
    rb_emit_event(state->ctx, type, status, message);
}

/* ------------------------------------------------------------------ */
/* Pairing (platform-managed only; no PIN is ever logged)              */
/*                                                                     */
/* The public vtable has no pairing slot, so these helpers are used    */
/* internally: basic pairing as a fallback when uncached discovery     */
/* fails with AccessDenied, and custom pairing for a future public     */
/* pairing entry point. Custom pairing supports exactly the kinds a    */
/* headless library can answer without UI: ConfirmOnly (accept) and    */
/* ProvidePin (accept with a caller-supplied PIN). DisplayPin and      */
/* ConfirmPinMatch would require showing the PIN to a user the        */
/* library cannot see, so they return NOT_SUPPORTED here rather       */
/* than auto-confirming a ceremony the caller never approved.          */
/* ------------------------------------------------------------------ */

static rb_status_t pair_basic(ble::BluetoothLEDevice device) {
    try {
        auto pairing = device.DeviceInformation().Pairing();
        if (!pairing.CanPair()) return RB_ERROR_NOT_SUPPORTED;
        if (pairing.IsPaired()) return RB_OK;
        auto result =
            pairing.PairAsync(en::DevicePairingProtectionLevel::None)
                .get();
        if (result.Status() == en::DevicePairingResultStatus::Paired)
            return RB_OK;
        return RB_ERROR_AUTH_FAILED;
    } catch (const winrt::hresult_error &e) {
        return map_hresult_error(e);
    } catch (...) {
        return RB_ERROR_INTERNAL;
    }
}

/* Non-static on purpose: this helper is implemented for a future
 * public pairing entry point (the vtable has no pairing slot yet)
 * and is not called by the current connect path, which uses
 * pair_basic() only. A static unused function would draw a
 * compiler warning; the helper stays as the documented hook. */
rb_status_t pair_custom(ble::BluetoothLEDevice device,
                        en::DevicePairingKinds kinds,
                        const std::string &pin) {
    if (kinds != en::DevicePairingKinds::ConfirmOnly &&
        kinds != en::DevicePairingKinds::ProvidePin)
        return RB_ERROR_NOT_SUPPORTED;
    if (kinds == en::DevicePairingKinds::ProvidePin && pin.empty())
        return RB_ERROR_INVALID_ARGUMENT;
    try {
        auto custom = device.DeviceInformation().Pairing().Custom();
        winrt::hstring pin_h = winrt::to_hstring(pin);
        auto token = custom.PairingRequested(
            [pin_h](en::DeviceInformationCustomPairing const &,
                    en::DevicePairingRequestedEventArgs const &args) {
                if (args.PairingKind() ==
                    en::DevicePairingKinds::ConfirmOnly) {
                    args.Accept();
                } else if (args.PairingKind() ==
                           en::DevicePairingKinds::ProvidePin) {
                    args.Accept(pin_h);
                }
                /* Any other kind is left unanswered on purpose:
                 * the pairing then fails instead of silently
                 * confirming an unapproved ceremony. */
            });
        rb_status_t out = RB_ERROR_INTERNAL;
        try {
            auto result = custom.PairAsync(kinds).get();
            out = (result.Status() ==
                   en::DevicePairingResultStatus::Paired)
                      ? RB_OK
                      : RB_ERROR_AUTH_FAILED;
        } catch (const winrt::hresult_error &e) {
            out = map_hresult_error(e);
        } catch (...) {
            out = RB_ERROR_INTERNAL;
        }
        try {
            custom.PairingRequested(token);
        } catch (...) {
        }
        return out;
    } catch (const winrt::hresult_error &e) {
        return map_hresult_error(e);
    } catch (...) {
        return RB_ERROR_INTERNAL;
    }
}

/* ------------------------------------------------------------------ */
/* GATT resolution (worker threads only)                               */
/* ------------------------------------------------------------------ */

static std::shared_ptr<BleConnection> find_connection(
    WinStatePtr state, const char *address) {
    rb_mutex_lock(&state->lock);
    auto it = state->connections.find(address != nullptr ? address : "");
    std::shared_ptr<BleConnection> conn =
        (it != state->connections.end()) ? it->second : nullptr;
    rb_mutex_unlock(&state->lock);
    return conn;
}

struct ResolvedChar {
    gatt::GattDeviceService service{nullptr};
    gatt::GattCharacteristic chr{nullptr};
};

static rb_status_t resolve_char(ble::BluetoothLEDevice device,
                                const rb_uuid_t *svc, const rb_uuid_t *chr,
                                ResolvedChar *out) {
    try {
        auto res = device
                       .GetGattServicesForUuidAsync(
                           guid_from_rb(svc),
                           ble::BluetoothCacheMode::Cached)
                       .get();
        if (res.Status() != gatt::GattCommunicationStatus::Success)
            return map_gatt(res.Status());
        if (res.Services().Size() == 0) return RB_ERROR_NOT_FOUND;
        auto service = res.Services().GetAt(0);
        auto cres = service.GetCharacteristicsAsync().get();
        if (cres.Status() != gatt::GattCommunicationStatus::Success)
            return map_gatt(cres.Status());
        for (uint32_t i = 0; i < cres.Characteristics().Size(); i++) {
            auto c = cres.Characteristics().GetAt(i);
            rb_uuid_t u;
            rb_from_guid(c.Uuid(), &u);
            if (rb_uuid_equals(&u, chr)) {
                out->service = service;
                out->chr = c;
                return RB_OK;
            }
        }
        return RB_ERROR_NOT_FOUND;
    } catch (const winrt::hresult_error &e) {
        return map_hresult_error(e);
    } catch (...) {
        return RB_ERROR_INTERNAL;
    }
}

static uint32_t gatt_props(gatt::GattCharacteristic chr) {
    return static_cast<uint32_t>(chr.CharacteristicProperties());
}

/* ------------------------------------------------------------------ */
/* Synchronous GATT work functions (run on shared worker threads)      */
/* ------------------------------------------------------------------ */

struct GattArg {
    WinStatePtr state;
    std::string address;
    rb_uuid_t svc{};
    rb_uuid_t chr{};
    std::vector<uint8_t> value; /* write payload */
    int with_response = 0;
    rb_ble_notify_callback_t cb = nullptr; /* subscribe */
    void *ud = nullptr;                    /* subscribe */
};

static void discover_work(SharedState *st, void *arg) {
    std::unique_ptr<GattArg> ga(static_cast<GattArg *>(arg));
    WinStatePtr state = ga->state;
    auto conn = find_connection(state, ga->address.c_str());
    if (!conn) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    rb_mutex_lock(&conn->mu);
    int connected = conn->connected ? 1 : 0;
    ble::BluetoothLEDevice device = conn->device;
    rb_mutex_unlock(&conn->mu);
    if (!connected || !device) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    try {
        auto res = device
                       .GetGattServicesAsync(
                           ble::BluetoothCacheMode::Uncached)
                       .get();
        if (res.Status() != gatt::GattCommunicationStatus::Success) {
            st->status = map_gatt(res.Status());
            return;
        }
        uint32_t nsvc = res.Services().Size();
        rb_ble_service_t *services = (rb_ble_service_t *)calloc(
            nsvc > 0 ? nsvc : 1, sizeof(rb_ble_service_t));
        if (services == nullptr) {
            st->status = RB_ERROR_NO_MEMORY;
            return;
        }
        for (uint32_t i = 0; i < nsvc; i++) {
            auto service = res.Services().GetAt(i);
            rb_ble_service_t *svc = &services[i];
            /* WinRT exposes no service attribute handle. */
            svc->handle = 0;
            rb_from_guid(service.Uuid(), &svc->uuid);
            try {
                auto cres = service.GetCharacteristicsAsync().get();
                if (cres.Status() !=
                    gatt::GattCommunicationStatus::Success) {
                    free(services);
                    st->status = map_gatt(cres.Status());
                    return;
                }
                uint32_t nchr = cres.Characteristics().Size();
                if (nchr > RB_BLE_MAX_CHARS_PER_SERVICE) {
                    win_log(state.get(), RB_LOG_WARN, "windows-ble",
                            "service reports more characteristics than "
                            "the public struct holds; truncating");
                    nchr = RB_BLE_MAX_CHARS_PER_SERVICE;
                }
                svc->characteristic_count = nchr;
                for (uint32_t j = 0; j < nchr; j++) {
                    auto c = cres.Characteristics().GetAt(j);
                    rb_ble_characteristic_t *ch =
                        &svc->characteristics[j];
                    rb_from_guid(c.Uuid(), &ch->uuid);
                    ch->handle = (uint16_t)c.AttributeHandle();
                    uint32_t bits = gatt_props(c);
                    uint32_t out_props = 0;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    Broadcast)
                        out_props |= RB_GATT_PROP_BROADCAST;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    Read)
                        out_props |= RB_GATT_PROP_READ;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    WriteWithoutResponse)
                        out_props |= RB_GATT_PROP_WRITE_NO_RSP;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    Write)
                        out_props |= RB_GATT_PROP_WRITE;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    Notify)
                        out_props |= RB_GATT_PROP_NOTIFY;
                    if (bits & (uint32_t)gatt::GattCharacteristicProperties::
                                    Indicate)
                        out_props |= RB_GATT_PROP_INDICATE;
                    ch->properties = out_props;
                    /* Descriptors are auxiliary: a failure here
                     * leaves the characteristic with zero
                     * descriptors rather than failing discovery. */
                    try {
                        auto dres = c.GetDescriptorsAsync().get();
                        if (dres.Status() ==
                            gatt::GattCommunicationStatus::Success) {
                            uint32_t ndesc = dres.Descriptors().Size();
                            if (ndesc > RB_BLE_MAX_DESCS_PER_CHAR)
                                ndesc = RB_BLE_MAX_DESCS_PER_CHAR;
                            ch->descriptor_count = ndesc;
                            for (uint32_t k = 0; k < ndesc; k++) {
                                auto d = dres.Descriptors().GetAt(k);
                                rb_from_guid(d.Uuid(),
                                             &ch->descriptors[k].uuid);
                                ch->descriptors[k].handle =
                                    (uint16_t)d.AttributeHandle();
                            }
                        }
                    } catch (...) {
                        ch->descriptor_count = 0;
                    }
                }
            } catch (const winrt::hresult_error &e) {
                free(services);
                st->status = map_hresult_error(e);
                return;
            } catch (...) {
                free(services);
                st->status = RB_ERROR_INTERNAL;
                return;
            }
        }
        st->services = services;
        st->service_count = nsvc;
        st->status = RB_OK;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

static void read_work(SharedState *st, void *arg) {
    std::unique_ptr<GattArg> ga(static_cast<GattArg *>(arg));
    WinStatePtr state = ga->state;
    auto conn = find_connection(state, ga->address.c_str());
    if (!conn) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    rb_mutex_lock(&conn->mu);
    int connected = conn->connected ? 1 : 0;
    ble::BluetoothLEDevice device = conn->device;
    rb_mutex_unlock(&conn->mu);
    if (!connected || !device) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    ResolvedChar rc;
    rb_status_t r = resolve_char(device, &ga->svc, &ga->chr, &rc);
    if (r != RB_OK) {
        st->status = r;
        return;
    }
    if (!(gatt_props(rc.chr) &
          (uint32_t)gatt::GattCharacteristicProperties::Read)) {
        st->status = RB_ERROR_NOT_SUPPORTED;
        return;
    }
    try {
        auto res = rc.chr.ReadValueAsync().get();
        if (res.Status() != gatt::GattCommunicationStatus::Success) {
            st->status = map_gatt(res.Status());
            return;
        }
        std::vector<uint8_t> bytes = bytes_from_buffer(res.Value());
        if (bytes.size() > RB_BLE_MAX_VALUE_LEN) {
            win_log(state.get(), RB_LOG_WARN, "windows-ble",
                    "characteristic value longer than the public "
                    "buffer; truncating");
            bytes.resize(RB_BLE_MAX_VALUE_LEN);
        }
        memcpy(st->value, bytes.data(), bytes.size());
        st->value_len = bytes.size();
        st->status = RB_OK;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

static void write_work(SharedState *st, void *arg) {
    std::unique_ptr<GattArg> ga(static_cast<GattArg *>(arg));
    WinStatePtr state = ga->state;
    auto conn = find_connection(state, ga->address.c_str());
    if (!conn) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    rb_mutex_lock(&conn->mu);
    int connected = conn->connected ? 1 : 0;
    ble::BluetoothLEDevice device = conn->device;
    rb_mutex_unlock(&conn->mu);
    if (!connected || !device) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    ResolvedChar rc;
    rb_status_t r = resolve_char(device, &ga->svc, &ga->chr, &rc);
    if (r != RB_OK) {
        st->status = r;
        return;
    }
    uint32_t bits = gatt_props(rc.chr);
    gatt::GattWriteOption option;
    if (ga->with_response) {
        if (!(bits &
              (uint32_t)gatt::GattCharacteristicProperties::Write)) {
            st->status = RB_ERROR_NOT_SUPPORTED;
            return;
        }
        option = gatt::GattWriteOption::WriteWithResponse;
    } else {
        if (!(bits & (uint32_t)gatt::GattCharacteristicProperties::
                          WriteWithoutResponse)) {
            st->status = RB_ERROR_NOT_SUPPORTED;
            return;
        }
        option = gatt::GattWriteOption::WriteWithoutResponse;
    }
    try {
        auto status = rc.chr
                          .WriteValueAsync(
                              buffer_from_bytes(ga->value.data(),
                                                ga->value.size()),
                              option)
                          .get();
        st->status = map_gatt(status);
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

static std::string sub_key(const char *address, const rb_uuid_t *svc,
                           const rb_uuid_t *chr) {
    char sbuf[64], cbuf[64];
    rb_uuid_to_string(svc, sbuf, sizeof(sbuf));
    rb_uuid_to_string(chr, cbuf, sizeof(cbuf));
    std::string key(address != nullptr ? address : "");
    key += "|";
    key += sbuf;
    key += "|";
    key += cbuf;
    return key;
}

static void subscribe_work(SharedState *st, void *arg) {
    std::unique_ptr<GattArg> ga(static_cast<GattArg *>(arg));
    WinStatePtr state = ga->state;
    auto conn = find_connection(state, ga->address.c_str());
    if (!conn) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    rb_mutex_lock(&conn->mu);
    int connected = conn->connected ? 1 : 0;
    ble::BluetoothLEDevice device = conn->device;
    rb_mutex_unlock(&conn->mu);
    if (!connected || !device) {
        st->status = RB_ERROR_NOT_CONNECTED;
        return;
    }
    std::string key = sub_key(ga->address.c_str(), &ga->svc, &ga->chr);
    rb_mutex_lock(&state->lock);
    int exists = state->subscriptions.count(key) != 0;
    rb_mutex_unlock(&state->lock);
    if (exists) {
        st->status = RB_ERROR_ALREADY_IN_PROGRESS;
        return;
    }
    ResolvedChar rc;
    rb_status_t r = resolve_char(device, &ga->svc, &ga->chr, &rc);
    if (r != RB_OK) {
        st->status = r;
        return;
    }
    uint32_t bits = gatt_props(rc.chr);
    int can_notify =
        (bits & (uint32_t)gatt::GattCharacteristicProperties::Notify) != 0;
    int can_indicate =
        (bits & (uint32_t)gatt::GattCharacteristicProperties::Indicate) !=
        0;
    if (!can_notify && !can_indicate) {
        st->status = RB_ERROR_NOT_SUPPORTED;
        return;
    }
    auto sub = std::make_shared<Subscription>();
    sub->state = state.get();
    sub->key = key;
    sub->address = ga->address;
    sub->service_uuid = ga->svc;
    sub->characteristic_uuid = ga->chr;
    sub->characteristic = rc.chr;
    sub->cb = ga->cb;
    sub->ud = ga->ud;
    /* Register ValueChanged BEFORE writing the CCCD so no
     * notification can arrive unhandled in between. */
    try {
        sub->token = rc.chr.ValueChanged(
            [state, sub](gatt::GattCharacteristic const &,
                         gatt::GattValueChangedEventArgs const &args) {
                if (!ctx_live(state.get())) return;
                if (sub->cb == nullptr) return;
                std::vector<uint8_t> bytes =
                    bytes_from_buffer(args.CharacteristicValue());
                if (bytes.size() > RB_BLE_MAX_VALUE_LEN)
                    bytes.resize(RB_BLE_MAX_VALUE_LEN);
                rb_ble_notification_t notif;
                memset(&notif, 0, sizeof(notif));
                rb_copy_str(notif.device.address,
                            sizeof(notif.device.address),
                            sub->address.c_str());
                notif.service_uuid = sub->service_uuid;
                notif.characteristic_uuid = sub->characteristic_uuid;
                memcpy(notif.data, bytes.data(), bytes.size());
                notif.data_len = bytes.size();
                sub->cb(state->ctx, &notif, sub->ud);
            });
        sub->token_valid = true;
        auto cccd =
            can_notify
                ? gatt::GattClientCharacteristicConfigurationDescriptorValue::
                      Notify
                : gatt::GattClientCharacteristicConfigurationDescriptorValue::
                      Indicate;
        auto status =
            rc.chr
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    cccd)
                .get();
        if (status != gatt::GattCommunicationStatus::Success) {
            try {
                rc.chr.ValueChanged(sub->token);
            } catch (...) {
            }
            st->status = map_gatt(status);
            return;
        }
        rb_mutex_lock(&state->lock);
        state->subscriptions[key] = sub;
        rb_mutex_unlock(&state->lock);
        st->status = RB_OK;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

static void unsubscribe_work(SharedState *st, void *arg) {
    std::unique_ptr<GattArg> ga(static_cast<GattArg *>(arg));
    WinStatePtr state = ga->state;
    std::string key = sub_key(ga->address.c_str(), &ga->svc, &ga->chr);
    rb_mutex_lock(&state->lock);
    auto it = state->subscriptions.find(key);
    std::shared_ptr<Subscription> sub =
        (it != state->subscriptions.end()) ? it->second : nullptr;
    if (sub) state->subscriptions.erase(it);
    rb_mutex_unlock(&state->lock);
    if (!sub) {
        st->status = RB_ERROR_NOT_FOUND;
        return;
    }
    rb_status_t out = RB_OK;
    try {
        auto status =
            sub->characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    gatt::GattClientCharacteristicConfigurationDescriptorValue::
                        None)
                .get();
        out = map_gatt(status);
    } catch (const winrt::hresult_error &e) {
        out = map_hresult_error(e);
    } catch (...) {
        out = RB_ERROR_INTERNAL;
    }
    /* Revoke and drop the record even when the CCCD write failed
     * (e.g. the device is gone): the subscription is over either
     * way, and a stale record would block re-subscribing. */
    try {
        if (sub->token_valid)
            sub->characteristic.ValueChanged(sub->token);
    } catch (...) {
    }
    sub->token_valid = false;
    st->status = out;
}

/* Run a GattArg job on a shared worker and translate the outcome.
 * On success for discover, ownership of the services array moves to
 * the caller (who frees it with rb_free()). */
static rb_status_t run_gatt(WinStatePtr state, GattArg *arg,
                            SharedWorkFn work, unsigned int timeout_ms,
                            rb_ble_service_t **out_services,
                            size_t *out_count, uint8_t *out_value,
                            size_t *out_len) {
    SharedState *st = shared_new();
    shared_run(st, work, arg);
    if (!shared_wait(st, timeout_ms)) {
        shared_release(st);
        return RB_ERROR_TIMEOUT;
    }
    rb_status_t status = st->status;
    if (status == RB_OK) {
        if (out_services != nullptr && out_count != nullptr) {
            *out_services = st->services;
            *out_count = st->service_count;
            st->services = nullptr; /* caller now owns the array */
        }
        if (out_value != nullptr && out_len != nullptr) {
            memcpy(out_value, st->value, st->value_len);
            *out_len = st->value_len;
        }
    }
    shared_release(st);
    return status;
}

/* ------------------------------------------------------------------ */
/* BLE scan                                                            */
/* ------------------------------------------------------------------ */

static void on_advertisement(WinStatePtr state,
                             const std::shared_ptr<BleScan> &scan,
                             adv::BluetoothLEAdvertisementReceivedEventArgs const &args) {
    rb_ble_device_t dev;
    memset(&dev, 0, sizeof(dev));
    int deliver = 0;
    try {
        uint64_t addr = args.BluetoothAddress();
        int16_t raw = args.RawSignalStrengthInDBm();
        int8_t rssi = raw < -128 ? (int8_t)-128
                                 : (raw > 127 ? (int8_t)127 : (int8_t)raw);
        auto adv = args.Advertisement();
        std::string name = winrt::to_string(adv.LocalName());
        /* Service UUIDs (cap: public struct holds 16). */
        auto uuids = adv.ServiceUuids();
        size_t nuuid = uuids.Size();
        if (nuuid > RB_BLE_MAX_SERVICE_UUIDS)
            nuuid = RB_BLE_MAX_SERVICE_UUIDS;
        /* Manufacturer data: first section only (the public struct
         * holds one id + one payload). */
        int has_mfg = 0;
        uint16_t mfg_id = 0;
        std::vector<uint8_t> mfg_data;
        auto mfg = adv.ManufacturerData();
        if (mfg.Size() > 0) {
            auto section = mfg.GetAt(0);
            has_mfg = 1;
            mfg_id = section.CompanyId();
            mfg_data = bytes_from_buffer(section.Data());
            if (mfg_data.size() > RB_BLE_MAX_MANUFACTURER_DATA)
                mfg_data.resize(RB_BLE_MAX_MANUFACTURER_DATA);
        }
        /* TX power: parsed from the advertisement data sections
         * (GAP type 0x0A, one signed byte). The ReceivedEventArgs
         * transmit-power property was not used: its exact WinRT
         * nullability was not double-checked in this run, while
         * the section encoding is the documented GAP layout. */
        int has_tx = 0;
        int8_t tx = 0;
        auto sections = adv.DataSections();
        for (uint32_t i = 0; i < sections.Size(); i++) {
            auto section = sections.GetAt(i);
            if (section.DataType() == 0x0A) {
                std::vector<uint8_t> b =
                    bytes_from_buffer(section.Data());
                if (!b.empty()) {
                    has_tx = 1;
                    tx = (int8_t)b[0];
                }
                break;
            }
        }
        auto adv_type = args.AdvertisementType();
        int connectable =
            (adv_type ==
                 adv::BluetoothLEAdvertisementType::ConnectableUndirected ||
             adv_type ==
                 adv::BluetoothLEAdvertisementType::ConnectableDirected)
                ? 1
                : 0;

        rb_mutex_lock(&scan->mu);
        int stopped = scan->stopped ? 1 : 0;
        int drop = stopped;
        if (!drop && scan->opts.service_uuid_count > 0) {
            drop = 1;
            for (size_t k = 0; k < nuuid; k++) {
                rb_uuid_t u;
                rb_from_guid(uuids.GetAt((uint32_t)k), &u);
                for (size_t f = 0; f < scan->opts.service_uuid_count;
                     f++) {
                    if (rb_uuid_equals(&u,
                                       &scan->opts.service_uuids[f])) {
                        drop = 0;
                        break;
                    }
                }
                if (!drop) break;
            }
        }
        if (!drop && scan->opts.manufacturer_id >= 0) {
            if (!has_mfg ||
                mfg_id != (uint16_t)scan->opts.manufacturer_id)
                drop = 1;
        }
        if (!drop && scan->opts.has_min_rssi &&
            rssi < scan->opts.min_rssi_dbm)
            drop = 1;
        if (!drop && scan->opts.name_substring[0] != '\0' &&
            !name_contains(name.c_str(), scan->opts.name_substring))
            drop = 1;
        if (!drop && !scan->opts.allow_duplicates) {
            if (scan->seen.count(addr) != 0) {
                drop = 1;
            } else {
                scan->seen.insert(addr);
            }
        }
        if (!drop) {
            format_address(addr, dev.address, sizeof(dev.address));
            rb_copy_str(dev.name, sizeof(dev.name), name.c_str());
            dev.rssi_dbm = rssi;
            dev.has_tx_power = has_tx;
            dev.tx_power_dbm = tx;
            dev.connectable = connectable;
            dev.service_uuid_count = nuuid;
            for (size_t k = 0; k < nuuid; k++)
                rb_from_guid(uuids.GetAt((uint32_t)k),
                             &dev.service_uuids[k]);
            dev.has_manufacturer_data = has_mfg;
            dev.manufacturer_id = mfg_id;
            if (has_mfg) {
                memcpy(dev.manufacturer_data, mfg_data.data(),
                       mfg_data.size());
                dev.manufacturer_data_len = mfg_data.size();
            }
            deliver = 1;
        }
        rb_mutex_unlock(&scan->mu);
    } catch (...) {
        /* A malformed advertisement must never kill the scan. */
        return;
    }
    if (deliver && ctx_live(state.get()) && scan->cb != nullptr)
        scan->cb(state->ctx, &dev, scan->ud);
}

struct ScanHolder {
    WinStatePtr state;
    std::shared_ptr<BleScan> scan;
};

extern "C" void *rb_win_ble_scan_thread(void *p) {
    std::unique_ptr<ScanHolder> holder(static_cast<ScanHolder *>(p));
    WinStatePtr state = holder->state;
    std::shared_ptr<BleScan> scan = holder->scan;
    ensure_apartment();
    adv::BluetoothLEAdvertisementWatcher watcher{nullptr};
    try {
        watcher = adv::BluetoothLEAdvertisementWatcher();
        watcher.ScanningMode(adv::BluetoothLEScanningMode::Active);
        auto received = watcher.Received(
            [state, scan](
                adv::BluetoothLEAdvertisementWatcher const &,
                adv::BluetoothLEAdvertisementReceivedEventArgs const
                    &args) { on_advertisement(state, scan, args); });
        auto stopped = watcher.Stopped(
            [state](adv::BluetoothLEAdvertisementWatcher const &,
                    adv::BluetoothLEAdvertisementWatcherStoppedEventArgs const &) {
                win_log(state.get(), RB_LOG_INFO, "windows-ble",
                        "advertisement watcher stopped");
            });
        rb_mutex_lock(&scan->mu);
        scan->watcher = watcher;
        scan->received_token = received;
        scan->received_valid = true;
        scan->stopped_token = stopped;
        scan->stopped_valid = true;
        rb_mutex_unlock(&scan->mu);
        try {
            watcher.Start();
        } catch (const winrt::hresult_error &e) {
            rb_mutex_lock(&scan->mu);
            scan->start_failed = true;
            scan->start_status = map_hresult_error(e);
            rb_mutex_unlock(&scan->mu);
            if (scan->started_event != nullptr)
                SetEvent(scan->started_event);
            mark_done_live(state, scan->op);
            return nullptr;
        }
        if (scan->started_event != nullptr)
            SetEvent(scan->started_event);
        DWORD timeout = scan->opts.timeout_ms > 0
                            ? (DWORD)scan->opts.timeout_ms
                            : INFINITE;
        /* The watcher delivers on WinRT pool threads; this thread
         * only waits. If destroy flipped ctx_live, the worker still
         * stops the watcher below before exiting. */
        WaitForSingleObject(scan->stop_event, timeout);
        try {
            watcher.Stop();
        } catch (...) {
        }
        try {
            if (scan->received_valid) watcher.Received(received);
        } catch (...) {
        }
        try {
            if (scan->stopped_valid) watcher.Stopped(stopped);
        } catch (...) {
        }
        mark_done_live(state, scan->op);
    } catch (const winrt::hresult_error &e) {
        rb_mutex_lock(&scan->mu);
        int started_signaled = 0;
        (void)started_signaled;
        scan->start_failed = true;
        scan->start_status = map_hresult_error(e);
        rb_mutex_unlock(&scan->mu);
        if (scan->started_event != nullptr)
            SetEvent(scan->started_event);
        mark_done_live(state, scan->op);
    } catch (...) {
        if (scan->started_event != nullptr)
            SetEvent(scan->started_event);
        mark_done_live(state, scan->op);
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* BLE connect                                                         */
/* ------------------------------------------------------------------ */

static void teardown_connection(
    const std::shared_ptr<BleConnection> &conn) {
    /* Snapshot the handles under the mutex first: the status
     * handler (another thread) copies the device the same way, so
     * no thread ever reads a handle while this thread overwrites
     * it. Order then matters: revoke the status handler first
     * (against the still-open device), then drop
     * MaintainConnection, then close services, then close the
     * device. Every step is best-effort. */
    ble::BluetoothLEDevice device{nullptr};
    std::vector<gatt::GattDeviceService> services;
    winrt::event_token token{};
    bool token_valid = false;
    rb_mutex_lock(&conn->mu);
    device = conn->device;
    services = conn->services;
    token = conn->conn_token;
    token_valid = conn->conn_token_valid;
    conn->conn_token_valid = false;
    conn->services.clear();
    conn->device = nullptr;
    conn->connected = false;
    rb_mutex_unlock(&conn->mu);
    try {
        if (token_valid && device != nullptr)
            device.ConnectionStatusChanged(token);
    } catch (...) {
    }
    for (auto &service : services) {
        try {
            service.Session().MaintainConnection(false);
        } catch (...) {
        }
    }
    for (auto &service : services) {
        try {
            service.Close();
        } catch (...) {
        }
    }
    try {
        if (device != nullptr) device.Close();
    } catch (...) {
    }
}

struct ConnHolder {
    WinStatePtr state;
    std::shared_ptr<BleConnection> conn;
};

/* One discovery attempt. Returns RB_OK on success and leaves the
 * discovered services in conn->services (with MaintainConnection
 * set); on AccessDenied it tries one basic pairing and retries once. */
static rb_status_t connect_discover(WinStatePtr state,
                                    const std::shared_ptr<BleConnection> &conn) {
    ble::BluetoothLEDevice device = conn->device;
    auto res = device
                   .GetGattServicesAsync(
                       ble::BluetoothCacheMode::Uncached)
                   .get();
    if (res.Status() ==
        gatt::GattCommunicationStatus::AccessDenied) {
        win_log(state.get(), RB_LOG_INFO, "windows-ble",
                "uncached discovery denied; attempting one "
                "platform-managed basic pairing and retrying");
        if (pair_basic(device) == RB_OK) {
            res = device
                      .GetGattServicesAsync(
                          ble::BluetoothCacheMode::Uncached)
                      .get();
        }
    }
    if (res.Status() != gatt::GattCommunicationStatus::Success)
        return map_gatt(res.Status());
    conn->services.clear();
    for (uint32_t i = 0; i < res.Services().Size(); i++)
        conn->services.push_back(res.Services().GetAt(i));
    for (auto &service : conn->services) {
        try {
            service.Session().MaintainConnection(true);
        } catch (...) {
        }
    }
    return RB_OK;
}

extern "C" void *rb_win_ble_connect_thread(void *p) {
    std::unique_ptr<ConnHolder> holder(static_cast<ConnHolder *>(p));
    WinStatePtr state = holder->state;
    std::shared_ptr<BleConnection> conn = holder->conn;
    ensure_apartment();
    /* Snapshot the policy: set_reconnect_policy() may replace it
     * from another thread while this worker runs. */
    rb_reconnect_policy_t policy;
    rb_mutex_lock(&state->lock);
    policy = state->policy;
    rb_mutex_unlock(&state->lock);
    int ever_connected = 0;
    int attempt = 0;
    int done = 0;
    while (!done) {
        /* Abort promptly if a disconnect or shutdown arrived. */
        if (WaitForSingleObject(conn->disconnect_event, 0) ==
            WAIT_OBJECT_0)
            break;
        if (!ctx_live(state.get())) break;
        char msg[128];
        if (attempt == 0) {
            snprintf(msg, sizeof(msg), "connecting");
        } else {
            snprintf(msg, sizeof(msg), "reconnect attempt %d", attempt);
        }
        emit_conn(state, conn, RB_BLE_CONNECTING, RB_OK, msg);
        rb_status_t step = RB_OK;
        int have_device = 0;
        try {
            auto device =
                ble::BluetoothLEDevice::FromBluetoothAddressAsync(
                    conn->bt_address)
                    .get();
            if (!device) {
                step = RB_ERROR_NOT_FOUND;
            } else {
                rb_mutex_lock(&conn->mu);
                conn->device = device;
                rb_mutex_unlock(&conn->mu);
                have_device = 1;
            }
        } catch (const winrt::hresult_error &e) {
            step = map_hresult_error(e);
        } catch (...) {
            step = RB_ERROR_INTERNAL;
        }
        if (step == RB_OK && have_device) {
            /* Register the status handler before discovery so a
             * drop during discovery is observed, not missed. */
            try {
                conn->conn_token =
                    conn->device.ConnectionStatusChanged(
                        [state, conn](
                            ble::BluetoothLEDevice const &,
                            winrt::Windows::Foundation::IInspectable const &) {
                            /* Copy the device under the mutex (see
                             * teardown_connection): the worker may
                             * be closing it on another thread. */
                            ble::BluetoothLEDevice device{nullptr};
                            rb_mutex_lock(&conn->mu);
                            device = conn->device;
                            rb_mutex_unlock(&conn->mu);
                            if (!device) return;
                            try {
                                auto status = device.ConnectionStatus();
                                if (status !=
                                    ble::BluetoothConnectionStatus::
                                        Disconnected)
                                    return;
                            } catch (...) {
                                return;
                            }
                            rb_mutex_lock(&conn->mu);
                            int expecting =
                                conn->expecting_disconnect ? 1 : 0;
                            if (!expecting)
                                conn->need_reconnect = true;
                            rb_mutex_unlock(&conn->mu);
                            if (!expecting && conn->wake_event != nullptr)
                                SetEvent(conn->wake_event);
                        });
                conn->conn_token_valid = true;
            } catch (...) {
                conn->conn_token_valid = false;
            }
            try {
                step = connect_discover(state, conn);
            } catch (const winrt::hresult_error &e) {
                step = map_hresult_error(e);
            } catch (...) {
                step = RB_ERROR_INTERNAL;
            }
        }
        if (step != RB_OK) {
            if (ever_connected) {
                /* A failed re-attempt after a real connection:
                 * keep retrying while the policy allows, else
                 * report the drop (not a fresh failure). */
                int max_attempts =
                    conn->opts.max_reconnect_attempts != 0
                        ? (int)conn->opts.max_reconnect_attempts
                        : (int)policy.max_attempts;
                int unlimited = (max_attempts == 0);
                if ((unlimited || conn->attempts < max_attempts) &&
                    WaitForSingleObject(conn->disconnect_event, 0) !=
                        WAIT_OBJECT_0 &&
                    ctx_live(state.get())) {
                    conn->attempts++;
                    teardown_connection(conn);
                    uint32_t delay =
                        conn->opts.reconnect_delay_ms != 0
                            ? conn->opts.reconnect_delay_ms
                            : policy.initial_delay_ms;
                    uint32_t factor = policy.backoff_factor_x100;
                    for (int k = 1; k < conn->attempts; k++) {
                        delay = delay * factor / 100;
                        if (delay > 30000) delay = 30000;
                    }
                    if (delay > 30000) delay = 30000;
                    uint32_t slept = 0;
                    while (slept < delay) {
                        if (WaitForSingleObject(conn->disconnect_event,
                                                0) == WAIT_OBJECT_0)
                            break;
                        uint32_t chunk =
                            (delay - slept < 50) ? (delay - slept) : 50;
                        rb_sleep_ms(chunk);
                        slept += chunk;
                    }
                    if (WaitForSingleObject(conn->disconnect_event, 0) ==
                        WAIT_OBJECT_0)
                        break;
                    attempt++;
                    continue;
                }
                teardown_connection(conn);
                emit_conn(state, conn, RB_BLE_DISCONNECTED, RB_OK,
                          "device disconnected");
            } else {
                teardown_connection(conn);
                emit_conn(state, conn, RB_BLE_FAILED, step,
                          "connect failed");
            }
            break;
        }
        /* Connected. */
        ever_connected = 1;
        rb_mutex_lock(&conn->mu);
        conn->connected = true;
        int first = conn->op_done ? 0 : 1;
        conn->op_done = 1;
        rb_mutex_unlock(&conn->mu);
        emit_conn(state, conn, RB_BLE_CONNECTED, RB_OK, "connected");
        if (first) mark_done_live(state, conn->op);
        /* Wait for a drop (wake) or an explicit disconnect. */
        HANDLE events[2] = {conn->disconnect_event, conn->wake_event};
        DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            /* Explicit disconnect: no connection callback for this
             * (the caller already knows); the general event below
             * reports the state change. */
            break;
        }
        /* Wake: the status handler saw a drop. Decide in this thread
         * (never in the handler) whether policy allows a retry. */
        rb_mutex_lock(&conn->mu);
        int need = conn->need_reconnect ? 1 : 0;
        conn->need_reconnect = false;
        rb_mutex_unlock(&conn->mu);
        if (!need) continue;
        int max_attempts = conn->opts.max_reconnect_attempts != 0
                               ? (int)conn->opts.max_reconnect_attempts
                               : (int)policy.max_attempts;
        int unlimited = (max_attempts == 0);
        int policy_on = policy.enabled || conn->opts.auto_reconnect;
        if (!policy_on || (!unlimited && conn->attempts >= max_attempts)) {
            teardown_connection(conn);
            emit_conn(state, conn, RB_BLE_DISCONNECTED, RB_OK,
                      "device disconnected");
            break;
        }
        conn->attempts++;
        teardown_connection(conn);
        uint32_t delay = conn->opts.reconnect_delay_ms != 0
                             ? conn->opts.reconnect_delay_ms
                             : policy.initial_delay_ms;
        uint32_t factor = policy.backoff_factor_x100;
        for (int k = 1; k < conn->attempts; k++) {
            delay = delay * factor / 100;
            if (delay > 30000) delay = 30000;
        }
        if (delay > 30000) delay = 30000;
        uint32_t slept = 0;
        while (slept < delay) {
            if (WaitForSingleObject(conn->disconnect_event, 0) ==
                WAIT_OBJECT_0)
                break;
            uint32_t chunk = (delay - slept < 50) ? (delay - slept) : 50;
            rb_sleep_ms(chunk);
            slept += chunk;
        }
        if (WaitForSingleObject(conn->disconnect_event, 0) ==
            WAIT_OBJECT_0)
            break;
        attempt++;
    }
    /* Final teardown. On an explicit disconnect the connection
     * callback is deliberately NOT fired (public contract); the
     * general event reports the change. */
    int was_explicit = 0;
    rb_mutex_lock(&conn->mu);
    was_explicit = conn->expecting_disconnect ? 1 : 0;
    rb_mutex_unlock(&conn->mu);
    teardown_connection(conn);
    rb_mutex_lock(&state->lock);
    auto it = state->connections.find(conn->address);
    if (it != state->connections.end() && it->second == conn)
        state->connections.erase(it);
    rb_mutex_unlock(&state->lock);
    if (was_explicit)
        emit_general(state, RB_EVENT_BACKEND_STATE, RB_OK,
                     "BLE device disconnected");
    return nullptr;
}

} /* namespace rbwin */

/* ------------------------------------------------------------------ */
/* extern "C" BLE entry points                                         */
/* ------------------------------------------------------------------ */

extern "C" rb_status_t rb_win_ble_scan_start(
    rb_context_t *ctx, const rb_ble_scan_options_t *opts,
    rb_ble_scan_callback_t cb, void *ud, rb_operation_id_t op) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (opts == nullptr || cb == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&state->lock);
    int busy = state->ble_scans.empty() ? 0 : 1;
    rb_mutex_unlock(&state->lock);
    if (busy) return RB_ERROR_ALREADY_IN_PROGRESS;
    auto scan = std::make_shared<rbwin::BleScan>();
    scan->state = state.get();
    scan->opts = *opts;
    scan->cb = cb;
    scan->ud = ud;
    scan->op = op;
    rb_mutex_lock(&state->lock);
    state->ble_scans[op] = scan;
    rb_mutex_unlock(&state->lock);
    rbwin::ScanHolder *holder = new rbwin::ScanHolder();
    holder->state = state;
    holder->scan = scan;
    rb_thread_t th = RB_THREAD_INVALID;
    if (rb_thread_create(&th, rbwin::rb_win_ble_scan_thread, holder) !=
        0) {
        delete holder;
        rb_mutex_lock(&state->lock);
        state->ble_scans.erase(op);
        rb_mutex_unlock(&state->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    /* Wait briefly for the watcher to start so a missing radio or a
     * denied capability is reported synchronously instead of only
     * in the log. If the wait itself times out, the scan is still
     * running; a later start failure is logged and the op is marked
     * done by the worker. */
    if (scan->started_event != nullptr &&
        WaitForSingleObject(scan->started_event, 2000) == WAIT_OBJECT_0) {
        rb_mutex_lock(&scan->mu);
        int failed = scan->start_failed ? 1 : 0;
        rb_status_t status = scan->start_status;
        rb_mutex_unlock(&scan->mu);
        if (failed) {
            rb_mutex_lock(&state->lock);
            state->ble_scans.erase(op);
            rb_mutex_unlock(&state->lock);
            return status;
        }
    }
    return RB_OK;
}

extern "C" rb_status_t rb_win_ble_scan_stop(rb_context_t *ctx,
                                            rb_operation_id_t op) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    rb_mutex_lock(&state->lock);
    auto it = state->ble_scans.find(op);
    std::shared_ptr<rbwin::BleScan> scan =
        (it != state->ble_scans.end()) ? it->second : nullptr;
    if (scan) state->ble_scans.erase(it);
    rb_mutex_unlock(&state->lock);
    if (!scan) return RB_ERROR_NOT_FOUND;
    rb_mutex_lock(&scan->mu);
    scan->stopped = true;
    rb_mutex_unlock(&scan->mu);
    try {
        if (scan->watcher != nullptr) scan->watcher.Stop();
    } catch (...) {
    }
    if (scan->stop_event != nullptr) SetEvent(scan->stop_event);
    rbwin::mark_done_live(state, op);
    return RB_OK;
}

extern "C" rb_status_t rb_win_ble_connect(
    rb_context_t *ctx, const rb_device_id_t *dev,
    const rb_ble_connect_options_t *opts, rb_ble_connection_callback_t cb,
    void *ud, rb_operation_id_t op) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || cb == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    uint64_t addr = 0;
    if (rbwin::parse_address(dev->address, &addr) != 0)
        return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&state->lock);
    int exists = state->connections.count(dev->address) != 0;
    rb_mutex_unlock(&state->lock);
    if (exists) return RB_ERROR_ALREADY_IN_PROGRESS;
    auto conn = std::make_shared<rbwin::BleConnection>();
    conn->state = state.get();
    conn->address = dev->address;
    conn->bt_address = addr;
    conn->cb = cb;
    conn->ud = ud;
    conn->op = op;
    if (opts != nullptr) conn->opts = *opts;
    rb_mutex_lock(&state->lock);
    state->connections[conn->address] = conn;
    rb_mutex_unlock(&state->lock);
    rbwin::ConnHolder *holder = new rbwin::ConnHolder();
    holder->state = state;
    holder->conn = conn;
    rb_thread_t th = RB_THREAD_INVALID;
    if (rb_thread_create(&th, rbwin::rb_win_ble_connect_thread, holder) !=
        0) {
        delete holder;
        rb_mutex_lock(&state->lock);
        state->connections.erase(conn->address);
        rb_mutex_unlock(&state->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    return RB_OK;
}

extern "C" rb_status_t rb_win_ble_disconnect(rb_context_t *ctx,
                                             const rb_device_id_t *dev) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr) return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&state->lock);
    auto it = state->connections.find(dev->address);
    std::shared_ptr<rbwin::BleConnection> conn =
        (it != state->connections.end()) ? it->second : nullptr;
    if (conn) state->connections.erase(it);
    rb_mutex_unlock(&state->lock);
    if (!conn) return RB_ERROR_NOT_CONNECTED;
    /* Asynchronous by platform design: this only REQUESTS teardown.
     * The worker closes the handles and the general event reports
     * the change; WinRT itself drops the link only when no other
     * app holds the device. */
    rb_mutex_lock(&conn->mu);
    conn->expecting_disconnect = true;
    rb_mutex_unlock(&conn->mu);
    if (conn->disconnect_event != nullptr)
        SetEvent(conn->disconnect_event);
    return RB_OK;
}

extern "C" rb_status_t rb_win_ble_discover(rb_context_t *ctx,
                                           const rb_device_id_t *dev,
                                           rb_ble_service_t **out,
                                           size_t *count) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || out == nullptr || count == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    *count = 0;
    rbwin::GattArg *arg = new rbwin::GattArg();
    arg->state = state;
    arg->address = dev->address;
    return rbwin::run_gatt(state, arg, rbwin::discover_work, 120000, out,
                           count, nullptr, nullptr);
}

extern "C" rb_status_t rb_win_ble_read(rb_context_t *ctx,
                                       const rb_device_id_t *dev,
                                       const rb_uuid_t *svc,
                                       const rb_uuid_t *chr,
                                       uint8_t *out_value,
                                       size_t *out_len) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || svc == nullptr || chr == nullptr ||
        out_value == nullptr || out_len == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    *out_len = 0;
    rbwin::GattArg *arg = new rbwin::GattArg();
    arg->state = state;
    arg->address = dev->address;
    arg->svc = *svc;
    arg->chr = *chr;
    return rbwin::run_gatt(state, arg, rbwin::read_work, 60000, nullptr,
                           nullptr, out_value, out_len);
}

extern "C" rb_status_t rb_win_ble_write(rb_context_t *ctx,
                                        const rb_device_id_t *dev,
                                        const rb_uuid_t *svc,
                                        const rb_uuid_t *chr,
                                        const uint8_t *value, size_t len,
                                        int with_response) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || svc == nullptr || chr == nullptr ||
        (value == nullptr && len > 0))
        return RB_ERROR_INVALID_ARGUMENT;
    rbwin::GattArg *arg = new rbwin::GattArg();
    arg->state = state;
    arg->address = dev->address;
    arg->svc = *svc;
    arg->chr = *chr;
    if (len > 0 && value != nullptr)
        arg->value.assign(value, value + len);
    arg->with_response = with_response;
    return rbwin::run_gatt(state, arg, rbwin::write_work, 60000, nullptr,
                           nullptr, nullptr, nullptr);
}

extern "C" rb_status_t rb_win_ble_subscribe(rb_context_t *ctx,
                                            const rb_device_id_t *dev,
                                            const rb_uuid_t *svc,
                                            const rb_uuid_t *chr,
                                            rb_ble_notify_callback_t cb,
                                            void *ud) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || svc == nullptr || chr == nullptr ||
        cb == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    rbwin::GattArg *arg = new rbwin::GattArg();
    arg->state = state;
    arg->address = dev->address;
    arg->svc = *svc;
    arg->chr = *chr;
    arg->cb = cb;
    arg->ud = ud;
    return rbwin::run_gatt(state, arg, rbwin::subscribe_work, 60000,
                           nullptr, nullptr, nullptr, nullptr);
}

extern "C" rb_status_t rb_win_ble_unsubscribe(rb_context_t *ctx,
                                              const rb_device_id_t *dev,
                                              const rb_uuid_t *svc,
                                              const rb_uuid_t *chr) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (dev == nullptr || svc == nullptr || chr == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    rbwin::GattArg *arg = new rbwin::GattArg();
    arg->state = state;
    arg->address = dev->address;
    arg->svc = *svc;
    arg->chr = *chr;
    return rbwin::run_gatt(state, arg, rbwin::unsubscribe_work, 60000,
                           nullptr, nullptr, nullptr, nullptr);
}

extern "C" rb_status_t rb_win_ble_set_reconnect_policy(
    rb_context_t *ctx, const rb_reconnect_policy_t *policy) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (policy == nullptr) return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&state->lock);
    state->policy = *policy;
    rb_mutex_unlock(&state->lock);
    return RB_OK;
}
