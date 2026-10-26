/*
 * RadioBridge — rb_jni.c (Android JNI bridge, Phase 3)
 *
 * COMPILED + SMOKE-TESTED on a desktop JVM (see TestSmoke.java): the
 * JVM loads the built library and drives the mock backend end to end
 * (scan/connect/read). That proves the JNI plumbing — JNI_OnLoad,
 * context lifecycle, callback marshaling, thread attach/detach,
 * reference handling — without an Android device.
 *
 * HONEST BOUNDARY: the Java helper that would call Android Framework
 * APIs (BluetoothLeScanner, BluetoothGatt, WifiManager) cannot run
 * without an Android device, so it is NOT in this file. What is here
 * is the real bridge over the portable C API: on Android, the same
 * native methods drive the Android backend (once its Java helper
 * exists) instead of the mock backend. Nothing here references
 * android.* classes, so this file compiles against a plain JDK.
 *
 * JNI rules followed (see bindings/android/README.md):
 *  - JNI_OnLoad caches the JavaVM; worker-thread callbacks attach
 *    (AttachCurrentThread) before touching JNI and detach when done.
 *  - No RadioBridge lock is ever held across a JNI call into Java.
 *  - Listeners are global refs. Scan records (and their callback
 *    args) are deleted by nativeBleScanStop (after the library scan
 *    thread is joined) or by nativeShutdown (after rb_shutdown joins
 *    all threads), so no callback can be in flight when they are
 *    freed. Connect records free themselves on the terminal
 *    CONNECTED/FAILED event, after the Java call returns.
 *  - Java exceptions from listener callbacks are cleared, never
 *    allowed to propagate into C worker threads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>

#include "radiobridge/rb.h"
#include "radiobridge/rb_bluetooth.h"
#include "radiobridge/rb_wifi.h"

/* Internal thread primitives; the JNI target builds with src/ on its
 * include path (see CMakeLists.txt). */
#include "core/rb_thread.h"

static JavaVM *g_vm = NULL;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* Get a JNIEnv for the calling thread; attach if needed. Sets
 * *attached_out to 1 when the caller must detach. */
static JNIEnv *jni_env(int *attached_out) {
    *attached_out = 0;
    if (g_vm == NULL) return NULL;
    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) !=
            0)
            return NULL;
        *attached_out = 1;
    } else if (rc != JNI_OK) {
        return NULL;
    }
    return env;
}

static rb_context_t *ctx_from_handle(jlong handle) {
    if (handle == 0) return NULL;
    return (rb_context_t *)(uintptr_t)handle;
}

static jstring new_string(JNIEnv *env, const char *s) {
    if (s == NULL) s = "";
    return (*env)->NewStringUTF(env, s);
}

/* ------------------------------------------------------------------ */
/* Scan records                                                        */
/* ------------------------------------------------------------------ */

typedef struct scan_record {
    rb_context_t *ctx;
    jobject listener; /* global ref */
    jmethodID mid;
    rb_operation_id_t op;
    void *cb_arg; /* scan callback arg, freed with the record */
    struct scan_record *next;
} scan_record_t;

typedef struct scan_cb_arg {
    scan_record_t *rec;
} scan_cb_arg_t;

static scan_record_t *g_scans = NULL;
static rb_mutex_t g_scans_lock;
static rb_once_t g_scans_once = RB_ONCE_INIT;

static void scans_init(void) { rb_mutex_init(&g_scans_lock); }

static void scan_record_add(scan_record_t *rec) {
    rb_once(&g_scans_once, scans_init);
    rb_mutex_lock(&g_scans_lock);
    rec->next = g_scans;
    g_scans = rec;
    rb_mutex_unlock(&g_scans_lock);
}

/* Remove the record for (ctx, op); returns it (caller deletes the
 * global ref and frees record + cb_arg). NULL when absent. */
static scan_record_t *scan_record_remove(rb_context_t *ctx,
                                         rb_operation_id_t op) {
    rb_once(&g_scans_once, scans_init);
    rb_mutex_lock(&g_scans_lock);
    scan_record_t **pp = &g_scans;
    while (*pp != NULL) {
        if ((*pp)->ctx == ctx && (*pp)->op == op) {
            scan_record_t *found = *pp;
            *pp = found->next;
            rb_mutex_unlock(&g_scans_lock);
            return found;
        }
        pp = &(*pp)->next;
    }
    rb_mutex_unlock(&g_scans_lock);
    return NULL;
}

static void scan_record_free(JNIEnv *env, scan_record_t *rec) {
    if (rec == NULL) return;
    if (env != NULL && rec->listener != NULL)
        (*env)->DeleteGlobalRef(env, rec->listener);
    free(rec->cb_arg);
    free(rec);
}

static void scan_record_remove_all_for_ctx(rb_context_t *ctx,
                                           JNIEnv *env) {
    rb_once(&g_scans_once, scans_init);
    rb_mutex_lock(&g_scans_lock);
    scan_record_t *cur = g_scans;
    scan_record_t *prev = NULL;
    while (cur != NULL) {
        scan_record_t *next = cur->next;
        if (cur->ctx == ctx) {
            if (prev == NULL) {
                g_scans = next;
            } else {
                prev->next = next;
            }
            scan_record_free(env, cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
    rb_mutex_unlock(&g_scans_lock);
}

static void rb_scan_cb(rb_context_t *context, const rb_ble_device_t *dev,
                       void *ud) {
    (void)context;
    scan_cb_arg_t *arg = (scan_cb_arg_t *)ud;
    scan_record_t *rec = arg->rec;
    int attached = 0;
    JNIEnv *env = jni_env(&attached);
    if (env == NULL || rec->listener == NULL) {
        if (attached) (*g_vm)->DetachCurrentThread(g_vm);
        return;
    }
    jstring addr = new_string(env, dev->address);
    jstring name = new_string(env, dev->name);
    (*env)->CallVoidMethod(env, rec->listener, rec->mid, addr, name,
                           (jint)dev->rssi_dbm);
    if (addr) (*env)->DeleteLocalRef(env, addr);
    if (name) (*env)->DeleteLocalRef(env, name);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

/* ------------------------------------------------------------------ */
/* Connect records (self-freeing on terminal events)                   */
/* ------------------------------------------------------------------ */

typedef struct conn_record {
    jobject listener; /* global ref */
    jmethodID mid;
} conn_record_t;

static void rb_conn_cb(rb_context_t *context,
                       const rb_ble_connection_event_t *ev, void *ud) {
    (void)context;
    conn_record_t *rec = (conn_record_t *)ud;
    int attached = 0;
    JNIEnv *env = jni_env(&attached);
    if (env != NULL && rec->listener != NULL) {
        jstring addr = new_string(env, ev->device.address);
        (*env)->CallVoidMethod(env, rec->listener, rec->mid, addr,
                               (jint)ev->state, (jint)ev->status);
        if (addr) (*env)->DeleteLocalRef(env, addr);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
    /* Terminal states end the record's life: no further callbacks
     * will arrive for this connection attempt. */
    if (ev->state == RB_BLE_CONNECTED || ev->state == RB_BLE_FAILED) {
        int attached2 = 0;
        JNIEnv *env2 = jni_env(&attached2);
        if (env2 != NULL && rec->listener != NULL)
            (*env2)->DeleteGlobalRef(env2, rec->listener);
        if (attached2) (*g_vm)->DetachCurrentThread(g_vm);
        free(rec);
    }
}

/* ------------------------------------------------------------------ */
/* Native methods (class com.radiobridge.RadioBridge)                  */
/* ------------------------------------------------------------------ */

JNIEXPORT jlong JNICALL
Java_com_radiobridge_RadioBridge_nativeInitialize(JNIEnv *env,
                                                  jclass clazz,
                                                  jint backend) {
    (void)env;
    (void)clazz;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = RB_LOG_WARN;
    switch (backend) {
        case 1: cfg.backend = RB_BACKEND_MOCK; break;
        case 2: cfg.backend = RB_BACKEND_LINUX; break;
        case 5: cfg.backend = RB_BACKEND_WINDOWS; break;
        default: cfg.backend = RB_BACKEND_AUTO; break;
    }
    rb_context_t *ctx = NULL;
    if (rb_initialize(&cfg, &ctx) != RB_OK) return (jlong)0;
    return (jlong)(uintptr_t)ctx;
}

JNIEXPORT void JNICALL
Java_com_radiobridge_RadioBridge_nativeShutdown(JNIEnv *env,
                                                jclass clazz,
                                                jlong handle) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL) return;
    /* rb_shutdown joins backend worker threads first; only then is
     * it safe to delete outstanding listener global refs. */
    rb_shutdown(ctx);
    scan_record_remove_all_for_ctx(ctx, env);
}

JNIEXPORT jstring JNICALL
Java_com_radiobridge_RadioBridge_nativeVersion(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return new_string(env, rb_version_string());
}

JNIEXPORT jstring JNICALL
Java_com_radiobridge_RadioBridge_nativeCapabilities(JNIEnv *env,
                                                    jclass clazz,
                                                    jlong handle) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL) return NULL;
    rb_capabilities_t caps;
    if (rb_get_capabilities(ctx, &caps) != RB_OK) return NULL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s %s ble:%d/%d/%d/%d wifi:%d/%d/%d %s",
             caps.backend_name, caps.backend_version, caps.ble.scan,
             caps.ble.connect, caps.ble.gatt, caps.ble.notify,
             caps.wifi.status, caps.wifi.scan, caps.wifi.connect,
             caps.detail);
    return new_string(env, buf);
}

JNIEXPORT jlong JNICALL
Java_com_radiobridge_RadioBridge_nativeBleScanStart(JNIEnv *env,
                                                    jclass clazz,
                                                    jlong handle,
                                                    jobject listener) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL || listener == NULL) return (jlong)0;
    jclass listener_class = (*env)->GetObjectClass(env, listener);
    if (listener_class == NULL) return (jlong)0;
    jmethodID mid = (*env)->GetMethodID(
        env, listener_class, "onDeviceFound",
        "(Ljava/lang/String;Ljava/lang/String;I)V");
    if (mid == NULL) return (jlong)0;
    jobject global = (*env)->NewGlobalRef(env, listener);
    if (global == NULL) return (jlong)0;
    scan_record_t *rec =
        (scan_record_t *)calloc(1, sizeof(*rec));
    if (rec == NULL) {
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    scan_cb_arg_t *arg = (scan_cb_arg_t *)calloc(1, sizeof(*arg));
    if (arg == NULL) {
        free(rec);
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    rec->ctx = ctx;
    rec->listener = global;
    rec->mid = mid;
    rec->cb_arg = arg;
    arg->rec = rec;
    rb_operation_id_t op = 0;
    rb_status_t st =
        rb_ble_scan_start(ctx, NULL, rb_scan_cb, arg, &op);
    if (st != RB_OK) {
        free(arg);
        free(rec);
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    rec->op = op;
    scan_record_add(rec);
    return (jlong)op;
}

JNIEXPORT jint JNICALL
Java_com_radiobridge_RadioBridge_nativeBleScanStop(JNIEnv *env,
                                                   jclass clazz,
                                                   jlong handle,
                                                   jlong op) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL) return (jint)RB_ERROR_INVALID_ARGUMENT;
    rb_status_t st = rb_ble_scan_stop(ctx, (rb_operation_id_t)op);
    if (st == RB_OK || st == RB_ERROR_NOT_FOUND) {
        /* Scan thread joined (OK) or already finished (NOT_FOUND):
         * no scan callback can be in flight now. */
        scan_record_t *rec =
            scan_record_remove(ctx, (rb_operation_id_t)op);
        scan_record_free(env, rec);
    }
    return (jint)st;
}

JNIEXPORT jlong JNICALL
Java_com_radiobridge_RadioBridge_nativeBleConnect(JNIEnv *env,
                                                  jclass clazz,
                                                  jlong handle,
                                                  jstring address,
                                                  jobject listener) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL || address == NULL || listener == NULL)
        return (jlong)0;
    jclass listener_class = (*env)->GetObjectClass(env, listener);
    if (listener_class == NULL) return (jlong)0;
    jmethodID mid = (*env)->GetMethodID(env, listener_class,
                                        "onStateChanged",
                                        "(Ljava/lang/String;II)V");
    if (mid == NULL) return (jlong)0;
    jobject global = (*env)->NewGlobalRef(env, listener);
    if (global == NULL) return (jlong)0;
    conn_record_t *rec =
        (conn_record_t *)calloc(1, sizeof(*rec));
    if (rec == NULL) {
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    rec->listener = global;
    rec->mid = mid;
    const char *addr = (*env)->GetStringUTFChars(env, address, NULL);
    if (addr == NULL) {
        free(rec);
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    size_t n = strlen(addr);
    if (n >= sizeof(dev.address)) n = sizeof(dev.address) - 1;
    memcpy(dev.address, addr, n);
    (*env)->ReleaseStringUTFChars(env, address, addr);
    rb_operation_id_t op = 0;
    rb_status_t st =
        rb_ble_connect(ctx, &dev, NULL, rb_conn_cb, rec, &op);
    if (st != RB_OK) {
        free(rec);
        (*env)->DeleteGlobalRef(env, global);
        return (jlong)0;
    }
    return (jlong)op;
}

JNIEXPORT jint JNICALL
Java_com_radiobridge_RadioBridge_nativeBleDisconnect(JNIEnv *env,
                                                     jclass clazz,
                                                     jlong handle,
                                                     jstring address) {
    (void)env;
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL || address == NULL)
        return (jint)RB_ERROR_INVALID_ARGUMENT;
    const char *addr = (*env)->GetStringUTFChars(env, address, NULL);
    if (addr == NULL) return (jint)RB_ERROR_NO_MEMORY;
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    size_t n = strlen(addr);
    if (n >= sizeof(dev.address)) n = sizeof(dev.address) - 1;
    memcpy(dev.address, addr, n);
    (*env)->ReleaseStringUTFChars(env, address, addr);
    return (jint)rb_ble_disconnect(ctx, &dev);
}

JNIEXPORT jbyteArray JNICALL
Java_com_radiobridge_RadioBridge_nativeBleRead(JNIEnv *env, jclass clazz,
                                               jlong handle,
                                               jstring address,
                                               jstring service_uuid,
                                               jstring characteristic_uuid) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL || address == NULL || service_uuid == NULL ||
        characteristic_uuid == NULL)
        return NULL;
    const char *addr = (*env)->GetStringUTFChars(env, address, NULL);
    const char *svc = (*env)->GetStringUTFChars(env, service_uuid, NULL);
    const char *chr =
        (*env)->GetStringUTFChars(env, characteristic_uuid, NULL);
    if (addr == NULL || svc == NULL || chr == NULL) {
        if (addr) (*env)->ReleaseStringUTFChars(env, address, addr);
        if (svc)
            (*env)->ReleaseStringUTFChars(env, service_uuid, svc);
        if (chr)
            (*env)->ReleaseStringUTFChars(env, characteristic_uuid, chr);
        return NULL;
    }
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    size_t n = strlen(addr);
    if (n >= sizeof(dev.address)) n = sizeof(dev.address) - 1;
    memcpy(dev.address, addr, n);
    rb_uuid_t svc_uuid, chr_uuid;
    rb_status_t st = rb_uuid_from_string(svc, &svc_uuid);
    if (st == RB_OK) st = rb_uuid_from_string(chr, &chr_uuid);
    (*env)->ReleaseStringUTFChars(env, address, addr);
    (*env)->ReleaseStringUTFChars(env, service_uuid, svc);
    (*env)->ReleaseStringUTFChars(env, characteristic_uuid, chr);
    if (st != RB_OK) return NULL;
    uint8_t buf[RB_BLE_MAX_VALUE_LEN];
    size_t len = 0;
    st = rb_ble_read(ctx, &dev, &svc_uuid, &chr_uuid, buf, &len);
    if (st != RB_OK) return NULL;
    jbyteArray out = (*env)->NewByteArray(env, (jsize)len);
    if (out == NULL) return NULL;
    if (len > 0)
        (*env)->SetByteArrayRegion(env, out, 0, (jsize)len,
                                   (const jbyte *)buf);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_radiobridge_RadioBridge_nativeWifiStatus(JNIEnv *env,
                                                  jclass clazz,
                                                  jlong handle) {
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL) return NULL;
    rb_wifi_status_t st;
    if (rb_wifi_get_status(ctx, &st) != RB_OK) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf), "%d|%s|%s", (int)st.state, st.ssid,
             st.ip_address);
    return new_string(env, buf);
}

JNIEXPORT jint JNICALL
Java_com_radiobridge_RadioBridge_nativeCancel(JNIEnv *env, jclass clazz,
                                              jlong handle, jlong op) {
    (void)env;
    (void)clazz;
    rb_context_t *ctx = ctx_from_handle(handle);
    if (ctx == NULL) return (jint)RB_ERROR_INVALID_ARGUMENT;
    return (jint)rb_cancel(ctx, (rb_operation_id_t)op);
}
