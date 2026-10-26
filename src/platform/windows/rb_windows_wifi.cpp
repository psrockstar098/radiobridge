/*
 * RadioBridge — rb_windows_wifi.cpp (Windows backend: Wi-Fi)
 *
 * COMPILE-UNVERIFIED — written from documented WinRT APIs, never
 * compiled (no Windows SDK on the authoring machine). Requires
 * Windows SDK + a Wi-Fi-capable Windows device to verify.
 *
 * Wi-Fi surface: Windows.Devices.WiFi.WiFiAdapter
 * (RequestAccessAsync, FindAllAdaptersAsync, ScanAsync,
 * NetworkReport.AvailableNetworks, ConnectAsync, Disconnect) plus
 * Windows.Networking.Connectivity.NetworkInformation for
 * wifi_get_status(). See rb_windows_errors.md for the capability
 * gating that shapes this file:
 *
 *  - The wifiControl capability is declared in a packaged app's
 *    manifest. Learn documents that without it, RequestAccessAsync
 *    always returns DeniedBySystem and FindAllAdaptersAsync /
 *    FromIdAsync fail. An UNPACKAGED Win32 desktop app has no
 *    manifest to declare it in, so on such a host this backend
 *    reports wifi REQUIRES_PERMISSION and wifi_scan_start /
 *    wifi_connect return RB_ERROR_PERMISSION_DENIED — honestly,
 *    instead of pretending Wi-Fi works there. A packaged (MSIX /
 *    sparse-package) consumer that declares wifiControl gets the
 *    full path below. This needs on-device confirmation for the
 *    exact unpackaged behaviour, because Microsoft's own wording
 *    covers the packaged case.
 *  - Location consent gates BSSID access (Learn note on every
 *    Wi-Fi member): without it, Bssid may come back empty. The
 *    code reports whatever the API returns and never fabricates a
 *    BSSID.
 *  - WiFiAvailableNetwork exposes NO security/authentication
 *    information, so every scanned network reports
 *    RB_WIFI_SEC_UNKNOWN. Reporting WPA2/WPA3 here would be a guess.
 *  - There is no saved-network enumeration and no endpoint quality
 *    probe in the WinRT Wi-Fi surface, so those capabilities are
 *    NOT_SUPPORTED (see rb_windows.c).
 *  - Hidden networks: rb_wifi_connect() reports unknown SSIDs as
 *    NOT_FOUND synchronously (public contract), so the hidden-
 *    network ConnectAsync overload (which takes an explicit SSID)
 *    is deliberately NOT used to guess at hidden networks.
 *  - wifi_get_status() reports the current WLAN profile's SSID and
 *    state. The WinRT Wi-Fi surface exposes no IP address or
 *    gateway for the connection, so those fields are left empty
 *    with an explanatory message rather than filled from an
 *    unrelated API.
 */
#ifndef _WIN32
#error "The Windows backend compiles on Windows only."
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Security.Credentials.h>

#include "platform/windows/rb_windows_internal.h"
#include "platform/windows/rb_windows_state.h"

namespace rbwin {

namespace conn = winrt::Windows::Networking::Connectivity;
namespace cred = winrt::Windows::Security::Credentials;

rb_status_t map_wifi_connection(wifi::WiFiConnectionStatus status) {
    switch (status) {
        case wifi::WiFiConnectionStatus::Success:
            return RB_OK;
        case wifi::WiFiConnectionStatus::AccessRevoked:
            return RB_ERROR_PERMISSION_DENIED;
        case wifi::WiFiConnectionStatus::InvalidCredential:
            return RB_ERROR_AUTH_FAILED;
        case wifi::WiFiConnectionStatus::NetworkNotAvailable:
            return RB_ERROR_NOT_FOUND;
        case wifi::WiFiConnectionStatus::Timeout:
            return RB_ERROR_TIMEOUT;
        case wifi::WiFiConnectionStatus::
            UnsupportedAuthenticationProtocol:
            return RB_ERROR_NOT_SUPPORTED;
        case wifi::WiFiConnectionStatus::UnspecifiedFailure:
        default:
            return RB_ERROR_INTERNAL;
    }
}

static const char *wifi_connection_name(
    wifi::WiFiConnectionStatus status) {
    switch (status) {
        case wifi::WiFiConnectionStatus::Success: return "success";
        case wifi::WiFiConnectionStatus::AccessRevoked:
            return "access revoked";
        case wifi::WiFiConnectionStatus::InvalidCredential:
            return "invalid credential";
        case wifi::WiFiConnectionStatus::NetworkNotAvailable:
            return "network not available";
        case wifi::WiFiConnectionStatus::Timeout: return "timeout";
        case wifi::WiFiConnectionStatus::
            UnsupportedAuthenticationProtocol:
            return "unsupported authentication protocol";
        case wifi::WiFiConnectionStatus::UnspecifiedFailure:
        default: return "unspecified failure";
    }
}

static const char *wifi_access_name(wifi::WiFiAccessStatus status) {
    switch (status) {
        case wifi::WiFiAccessStatus::Allowed: return "allowed";
        case wifi::WiFiAccessStatus::DeniedByUser:
            return "denied by user";
        case wifi::WiFiAccessStatus::DeniedBySystem:
            return "denied by system (no wifiControl capability)";
        default: return "unspecified";
    }
}

/* Frequency (MHz) -> 802.11 channel. 2.4 GHz: (f - 2407) / 5;
 * 5 GHz: (f - 5000) / 5; 6 GHz: (f - 5950) / 5. Out-of-band
 * frequencies report channel 0 (unknown). */
static uint32_t channel_from_mhz(uint32_t mhz) {
    if (mhz >= 2400 && mhz < 2500) return (mhz - 2407) / 5;
    if (mhz >= 5000 && mhz < 6000) return (mhz - 5000) / 5;
    if (mhz >= 5925 && mhz < 7125) return (mhz - 5950) / 5;
    return 0;
}

static rb_wifi_network_t network_to_rb(wifi::WiFiAvailableNetwork net) {
    rb_wifi_network_t out;
    memset(&out, 0, sizeof(out));
    rb_copy_str(out.ssid, sizeof(out.ssid),
                winrt::to_string(net.Ssid()).c_str());
    /* Bssid may be empty without location consent (see header). */
    rb_copy_str(out.bssid, sizeof(out.bssid),
                winrt::to_string(net.Bssid()).c_str());
    auto rssi = net.NetworkRssiInDecibelMilliwatts();
    if (rssi) {
        int32_t v = rssi.Value();
        out.signal_dbm = v < -128 ? (int8_t)-128
                                  : (v > 127 ? (int8_t)127 : (int8_t)v);
    } else {
        out.signal_dbm = 0; /* not reported */
    }
    uint32_t khz = net.ChannelCenterFrequencyInKilohertz();
    out.frequency_mhz = khz / 1000;
    out.channel = channel_from_mhz(out.frequency_mhz);
    /* WinRT exposes no security information for available networks. */
    out.security = RB_WIFI_SEC_UNKNOWN;
    return out;
}

/* Local mark-done helper (the BLE file's static one is not visible
 * across translation units; rb_op_mark_done itself is idempotent). */
static void mark_done_live(WinStatePtr state, rb_operation_id_t op) {
    if (!ctx_live(state.get())) return;
    rb_op_mark_done(state->ctx, op);
}

int wifi_probe_part(char *wifi_detail, size_t detail_len) {
    try {
        auto access = wifi::WiFiAdapter::RequestAccessAsync().get();
        if (access != wifi::WiFiAccessStatus::Allowed) {
            snprintf(wifi_detail, detail_len, "access %s",
                     wifi_access_name(access));
            return -1;
        }
        auto adapters = wifi::WiFiAdapter::FindAllAdaptersAsync().get();
        if (adapters.Size() == 0) {
            snprintf(wifi_detail, detail_len, "no adapters");
            return 0;
        }
        if (wifi_detail != nullptr && detail_len > 0)
            wifi_detail[0] = '\0';
        return 1;
    } catch (const winrt::hresult_error &) {
        snprintf(wifi_detail, detail_len, "probe failed");
        return 0;
    } catch (...) {
        snprintf(wifi_detail, detail_len, "probe failed");
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi scan                                                          */
/* ------------------------------------------------------------------ */

struct WifiCheckArg {
    /* Pre-flight check only: access + at least one adapter. */
};

static void wifi_check_work(SharedState *st, void *arg) {
    delete static_cast<WifiCheckArg *>(arg);
    try {
        auto access = wifi::WiFiAdapter::RequestAccessAsync().get();
        if (access != wifi::WiFiAccessStatus::Allowed) {
            st->status = RB_ERROR_PERMISSION_DENIED;
            return;
        }
        auto adapters = wifi::WiFiAdapter::FindAllAdaptersAsync().get();
        if (adapters.Size() == 0) {
            st->status = RB_ERROR_UNAVAILABLE;
            return;
        }
        st->status = RB_OK;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

struct WifiScanHolder {
    WinStatePtr state;
    std::shared_ptr<WifiScan> scan;
};

extern "C" void *rb_win_wifi_scan_thread(void *p) {
    std::unique_ptr<WifiScanHolder> holder(
        static_cast<WifiScanHolder *>(p));
    WinStatePtr state = holder->state;
    std::shared_ptr<WifiScan> scan = holder->scan;
    ensure_apartment();
    uint64_t started = rb_now_ms();
    try {
        auto adapters = wifi::WiFiAdapter::FindAllAdaptersAsync().get();
        if (adapters.Size() == 0) {
            mark_done_live(state, scan->op);
            return nullptr;
        }
        scan->adapter = adapters.GetAt(0);
        for (;;) {
            /* Never touch the core context after destroy: the
             * context is freed once destroy returns, while this
             * detached worker may still be winding down. */
            if (!ctx_live(state.get())) break;
            rb_mutex_lock(&scan->mu);
            int stopped = scan->stopped ? 1 : 0;
            rb_mutex_unlock(&scan->mu);
            if (stopped) break;
            if (scan->opts.timeout_ms > 0 &&
                rb_now_ms() - started >= scan->opts.timeout_ms)
                break;
            if (rb_op_is_cancelled(state->ctx, scan->op)) break;
            try {
                scan->adapter.ScanAsync().get();
            } catch (...) {
                break; /* scan error ends the scan; logged below */
            }
            try {
                auto report = scan->adapter.NetworkReport();
                /* Cache the full report for wifi_get_status(). */
                std::vector<rb_wifi_network_t> fresh;
                for (uint32_t i = 0;
                     i < report.AvailableNetworks().Size(); i++) {
                    auto net = report.AvailableNetworks().GetAt(i);
                    rb_wifi_network_t rbnet = network_to_rb(net);
                    fresh.push_back(rbnet);
                    std::string key =
                        std::string(rbnet.ssid) + "|" + rbnet.bssid;
                    if (scan->seen.count(key) != 0) continue;
                    scan->seen.insert(key);
                    if (ctx_live(state.get()) && scan->cb != nullptr)
                        scan->cb(state->ctx, &rbnet, scan->ud);
                }
                rb_mutex_lock(&state->lock);
                state->last_wifi_networks = fresh;
                rb_mutex_unlock(&state->lock);
            } catch (...) {
                break;
            }
            /* One-shot ScanAsync: poll again after 4 s (or until
             * stopped), so a scan behaves like the continuous scan
             * the public API promises. */
            if (WaitForSingleObject(scan->stop_event, 4000) ==
                WAIT_OBJECT_0)
                break;
        }
    } catch (...) {
    }
    mark_done_live(state, scan->op);
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi connect (asynchronous worker)                                 */
/* ------------------------------------------------------------------ */

struct WifiConnectHolder {
    WinStatePtr state;
    std::string ssid;
    std::string password; /* never logged; cleared after use */
};

extern "C" void *rb_win_wifi_connect_thread(void *p) {
    std::unique_ptr<WifiConnectHolder> holder(
        static_cast<WifiConnectHolder *>(p));
    WinStatePtr state = holder->state;
    std::string ssid = holder->ssid;
    std::string password = holder->password;
    holder->password.clear();
    ensure_apartment();
    rb_status_t final_status = RB_ERROR_INTERNAL;
    wifi::WiFiConnectionStatus conn_status =
        wifi::WiFiConnectionStatus::UnspecifiedFailure;
    int attempted = 0;
    try {
        auto access = wifi::WiFiAdapter::RequestAccessAsync().get();
        if (access != wifi::WiFiAccessStatus::Allowed) {
            final_status = RB_ERROR_PERMISSION_DENIED;
        } else {
            auto adapters =
                wifi::WiFiAdapter::FindAllAdaptersAsync().get();
            if (adapters.Size() == 0) {
                final_status = RB_ERROR_UNAVAILABLE;
            } else {
                auto adapter = adapters.GetAt(0);
                adapter.ScanAsync().get();
                auto report = adapter.NetworkReport();
                wifi::WiFiAvailableNetwork found{nullptr};
                for (uint32_t i = 0;
                     i < report.AvailableNetworks().Size(); i++) {
                    auto net = report.AvailableNetworks().GetAt(i);
                    if (winrt::to_string(net.Ssid()) == ssid) {
                        found = net;
                        break;
                    }
                }
                if (!found) {
                    final_status = RB_ERROR_NOT_FOUND;
                } else {
                    attempted = 1;
                    wifi::WiFiConnectionResult result{nullptr};
                    if (password.empty()) {
                        result = adapter
                                     .ConnectAsync(
                                         found,
                                         wifi::WiFiReconnectionKind::
                                             Automatic)
                                     .get();
                    } else {
                        cred::PasswordCredential credential;
                        credential.Password(
                            winrt::to_hstring(password));
                        result = adapter
                                     .ConnectAsync(
                                         found,
                                         wifi::WiFiReconnectionKind::
                                             Automatic,
                                         credential)
                                     .get();
                    }
                    conn_status = result.ConnectionStatus();
                    final_status =
                        map_wifi_connection(conn_status);
                }
            }
        }
    } catch (const winrt::hresult_error &e) {
        final_status = map_hresult_error(e);
    } catch (...) {
        final_status = RB_ERROR_INTERNAL;
    }
    password.clear();
    if (ctx_live(state.get())) {
        char msg[256];
        if (attempted) {
            snprintf(msg, sizeof(msg),
                     "wifi connect to '%s': %s", ssid.c_str(),
                     wifi_connection_name(conn_status));
        } else if (final_status == RB_ERROR_NOT_FOUND) {
            snprintf(msg, sizeof(msg),
                     "wifi network '%s' no longer visible",
                     ssid.c_str());
        } else {
            snprintf(msg, sizeof(msg), "wifi connect to '%s' failed",
                     ssid.c_str());
        }
        rb_emit_event(state->ctx, RB_EVENT_WIFI_CHANGED, final_status,
                      msg);
    }
    return nullptr;
}

} /* namespace rbwin */

/* ------------------------------------------------------------------ */
/* extern "C" Wi-Fi entry points                                       */
/* ------------------------------------------------------------------ */

extern "C" rb_status_t rb_win_wifi_get_status(rb_context_t *ctx,
                                              rb_wifi_status_t *out) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (out == nullptr) return RB_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    /* GetInternetConnectionProfile() is a synchronous call, not a
     * WinRT async operation, so it may run on the caller thread. */
    try {
        auto profile =
            rbwin::conn::NetworkInformation::GetInternetConnectionProfile();
        if (!profile) {
            out->state = RB_WIFI_DISCONNECTED;
            out->diag = RB_WIFI_DIAG_DISCONNECTED;
            rb_copy_str(out->message, sizeof(out->message),
                        "no active connection profile");
            return RB_OK;
        }
        if (!profile.IsWlanConnectionProfile()) {
            out->state = RB_WIFI_DISCONNECTED;
            out->diag = RB_WIFI_DIAG_DISCONNECTED;
            rb_copy_str(out->message, sizeof(out->message),
                        "active connection is not Wi-Fi");
            return RB_OK;
        }
        std::string ssid = winrt::to_string(profile.ProfileName());
        out->state = RB_WIFI_CONNECTED;
        out->diag = RB_WIFI_DIAG_OK;
        rb_copy_str(out->ssid, sizeof(out->ssid), ssid.c_str());
        /* Best-effort signal/BSSID from the last scan report. */
        rb_mutex_lock(&state->lock);
        for (auto &net : state->last_wifi_networks) {
            if (ssid == net.ssid) {
                out->signal_dbm = net.signal_dbm;
                rb_copy_str(out->bssid, sizeof(out->bssid), net.bssid);
                break;
            }
        }
        rb_mutex_unlock(&state->lock);
        /* ip_address / gateway stay empty on purpose: the WinRT
         * Wi-Fi surface exposes neither for the connection. */
        rb_copy_str(out->message, sizeof(out->message),
                    "connected (WinRT Wi-Fi surface exposes no "
                    "IP/gateway for this connection)");
        return RB_OK;
    } catch (const winrt::hresult_error &e) {
        return rbwin::map_hresult_error(e);
    } catch (...) {
        return RB_ERROR_UNAVAILABLE;
    }
}

extern "C" rb_status_t rb_win_wifi_scan_start(
    rb_context_t *ctx, const rb_wifi_scan_options_t *opts,
    rb_wifi_scan_callback_t cb, void *ud, rb_operation_id_t op) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (opts == nullptr || cb == nullptr)
        return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&state->lock);
    int busy = state->wifi_scans.empty() ? 0 : 1;
    rb_mutex_unlock(&state->lock);
    if (busy) return RB_ERROR_ALREADY_IN_PROGRESS;
    /* Pre-flight on a worker so a capability denial or a missing
     * adapter is reported synchronously, not only in the log. */
    rbwin::SharedState *st = rbwin::shared_new();
    rbwin::shared_run(st, rbwin::wifi_check_work,
                      new rbwin::WifiCheckArg());
    if (!rbwin::shared_wait(st, 10000)) {
        rbwin::shared_release(st);
        return RB_ERROR_TIMEOUT;
    }
    rb_status_t pre = st->status;
    rbwin::shared_release(st);
    if (pre != RB_OK) return pre;
    auto scan = std::make_shared<rbwin::WifiScan>();
    scan->state = state.get();
    scan->opts = *opts;
    scan->cb = cb;
    scan->ud = ud;
    scan->op = op;
    rb_mutex_lock(&state->lock);
    state->wifi_scans[op] = scan;
    rb_mutex_unlock(&state->lock);
    rbwin::WifiScanHolder *holder = new rbwin::WifiScanHolder();
    holder->state = state;
    holder->scan = scan;
    rb_thread_t th = RB_THREAD_INVALID;
    if (rb_thread_create(&th, rbwin::rb_win_wifi_scan_thread, holder) !=
        0) {
        delete holder;
        rb_mutex_lock(&state->lock);
        state->wifi_scans.erase(op);
        rb_mutex_unlock(&state->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    return RB_OK;
}

extern "C" rb_status_t rb_win_wifi_scan_stop(rb_context_t *ctx,
                                             rb_operation_id_t op) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    rb_mutex_lock(&state->lock);
    auto it = state->wifi_scans.find(op);
    std::shared_ptr<rbwin::WifiScan> scan =
        (it != state->wifi_scans.end()) ? it->second : nullptr;
    if (scan) state->wifi_scans.erase(it);
    rb_mutex_unlock(&state->lock);
    if (!scan) return RB_ERROR_NOT_FOUND;
    rb_mutex_lock(&scan->mu);
    scan->stopped = true;
    rb_mutex_unlock(&scan->mu);
    if (scan->stop_event != nullptr) SetEvent(scan->stop_event);
    rbwin::mark_done_live(state, op);
    return RB_OK;
}

namespace rbwin {

struct WifiSsidCheckArg {
    std::string ssid;
};

static void wifi_ssid_check_work(SharedState *st, void *arg) {
    std::unique_ptr<WifiSsidCheckArg> ca(
        static_cast<WifiSsidCheckArg *>(arg));
    try {
        auto access =
            rbwin::wifi::WiFiAdapter::RequestAccessAsync().get();
        if (access != rbwin::wifi::WiFiAccessStatus::Allowed) {
            st->status = RB_ERROR_PERMISSION_DENIED;
            return;
        }
        auto adapters =
            rbwin::wifi::WiFiAdapter::FindAllAdaptersAsync().get();
        if (adapters.Size() == 0) {
            st->status = RB_ERROR_UNAVAILABLE;
            return;
        }
        auto adapter = adapters.GetAt(0);
        adapter.ScanAsync().get();
        auto report = adapter.NetworkReport();
        for (uint32_t i = 0; i < report.AvailableNetworks().Size();
             i++) {
            auto net = report.AvailableNetworks().GetAt(i);
            if (winrt::to_string(net.Ssid()) == ca->ssid) {
                st->found = 1;
                st->status = RB_OK;
                return;
            }
        }
        st->status = RB_ERROR_NOT_FOUND;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

} /* namespace rbwin */

extern "C" rb_status_t rb_win_wifi_connect(
    rb_context_t *ctx, const char *ssid, const char *password,
    const rb_wifi_connect_options_t *opts) {
    (void)opts; /* WinRT connect takes no timeout; see errors doc. */
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    if (ssid == nullptr || ssid[0] == '\0')
        return RB_ERROR_INVALID_ARGUMENT;
    /* Synchronous SSID verification (public contract: unknown SSIDs
     * report NOT_FOUND synchronously). The connection itself then
     * proceeds on a worker thread and reports via
     * RB_EVENT_WIFI_CHANGED. */
    rbwin::SharedState *st = rbwin::shared_new();
    rbwin::WifiSsidCheckArg *arg = new rbwin::WifiSsidCheckArg();
    arg->ssid = ssid;
    rbwin::shared_run(st, rbwin::wifi_ssid_check_work, arg);
    if (!rbwin::shared_wait(st, 30000)) {
        rbwin::shared_release(st);
        return RB_ERROR_TIMEOUT;
    }
    rb_status_t pre = st->status;
    rbwin::shared_release(st);
    if (pre != RB_OK) return pre;
    rbwin::WifiConnectHolder *holder = new rbwin::WifiConnectHolder();
    holder->state = state;
    holder->ssid = ssid;
    holder->password = password != nullptr ? password : "";
    rb_thread_t th = RB_THREAD_INVALID;
    if (rb_thread_create(&th, rbwin::rb_win_wifi_connect_thread,
                         holder) != 0) {
        delete holder;
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    return RB_OK;
}

namespace rbwin {

static void wifi_disconnect_work(SharedState *st, void *arg) {
    delete static_cast<WifiCheckArg *>(arg);
    try {
        auto adapters =
            wifi::WiFiAdapter::FindAllAdaptersAsync().get();
        if (adapters.Size() == 0) {
            st->status = RB_ERROR_UNAVAILABLE;
            return;
        }
        /* Disconnect() is synchronous (void), not an async op. */
        adapters.GetAt(0).Disconnect();
        st->status = RB_OK;
    } catch (const winrt::hresult_error &e) {
        st->status = map_hresult_error(e);
    } catch (...) {
        st->status = RB_ERROR_INTERNAL;
    }
}

} /* namespace rbwin */

extern "C" rb_status_t rb_win_wifi_disconnect(rb_context_t *ctx) {
    rbwin::WinStatePtr state = rbwin::state_from_ctx(ctx);
    if (!state) return RB_ERROR_NOT_INITIALIZED;
    rbwin::SharedState *st = rbwin::shared_new();
    rbwin::shared_run(st, rbwin::wifi_disconnect_work,
                      new rbwin::WifiCheckArg());
    if (!rbwin::shared_wait(st, 10000)) {
        rbwin::shared_release(st);
        return RB_ERROR_TIMEOUT;
    }
    rb_status_t status = st->status;
    rbwin::shared_release(st);
    if (status == RB_OK && rbwin::ctx_live(state.get()))
        rb_emit_event(state->ctx, RB_EVENT_WIFI_CHANGED, RB_OK,
                      "wifi disconnected");
    return status;
}
