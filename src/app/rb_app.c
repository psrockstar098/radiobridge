/*
 * RadioBridge — rb_app.c (Phase 5 application layer)
 *
 * See rb_app.h. Notes on honesty boundaries:
 *  - SHA-256 here is a plain FIPS 180-4 implementation used for
 *    firmware integrity only; it is verified against NIST vectors.
 *    It is NOT used for the secure channel (that needs OpenSSL).
 *  - Reliable-transfer sequences are persistent per sender/receiver
 *    pair and never restart per message (a restart would let a stale
 *    ACK falsely acknowledge a new message's first frame); a failed
 *    send keeps the sender at the failed message's base so a retry
 *    replays safely through the receiver's duplicate suppression.
 *  - Frame CRC covers [flags][len][seq][payload]; ACK CRC covers
 *    [seq]. All multi-byte fields are big-endian on the wire.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rb_internal.h"
#include "radiobridge/rb_app.h"
#include "radiobridge/rb_bluetooth.h"
#include "radiobridge/rb_wifi.h"

/* ------------------------------------------------------------------ */
/* CRC32-IEEE (bitwise; messages here are <= 8 KB, speed is fine)      */
/* ------------------------------------------------------------------ */

uint32_t rb_crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4, one-shot)                                      */
/* ------------------------------------------------------------------ */

static const uint32_t RB_SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define RB_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void rb_sha256_block(uint32_t h[8], const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = RB_ROTR(w[i - 15], 7) ^ RB_ROTR(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = RB_ROTR(w[i - 2], 17) ^ RB_ROTR(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = RB_ROTR(e, 6) ^ RB_ROTR(e, 11) ^ RB_ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + RB_SHA_K[i] + w[i];
        uint32_t s0 = RB_ROTR(a, 2) ^ RB_ROTR(a, 13) ^ RB_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void rb_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t full_blocks = len / 64;
    for (size_t i = 0; i < full_blocks; i++)
        rb_sha256_block(h, data + i * 64);
    /* Padding: 0x80, zeros, 64-bit big-endian bit length. */
    uint8_t tail[128];
    size_t rem = len % 64;
    size_t tail_len = rem < 56 ? 64 : 128;
    memset(tail, 0, tail_len);
    if (rem > 0) memcpy(tail, data + full_blocks * 64, rem);
    tail[rem] = 0x80;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tail_len; i += 64)
        rb_sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Device registry                                                     */
/* ------------------------------------------------------------------ */

#define RB_REG_MAX 128

struct rb_registry {
    rb_device_profile_t items[RB_REG_MAX];
    size_t count;
};

rb_registry_t *rb_registry_create(void) {
    return (rb_registry_t *)calloc(1, sizeof(rb_registry_t));
}

void rb_registry_destroy(rb_registry_t *reg) { free(reg); }

rb_status_t rb_registry_add(rb_registry_t *reg,
                            const rb_device_profile_t *profile) {
    if (reg == NULL || profile == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].id, profile->id) == 0) {
            reg->items[i] = *profile;
            return RB_OK;
        }
    }
    if (reg->count >= RB_REG_MAX) return RB_ERROR_NO_MEMORY;
    reg->items[reg->count++] = *profile;
    return RB_OK;
}

rb_status_t rb_registry_remove(rb_registry_t *reg, const char *id) {
    if (reg == NULL || id == NULL) return RB_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].id, id) == 0) {
            for (size_t j = i + 1; j < reg->count; j++)
                reg->items[j - 1] = reg->items[j];
            reg->count--;
            return RB_OK;
        }
    }
    return RB_ERROR_NOT_FOUND;
}

const rb_device_profile_t *rb_registry_find_id(rb_registry_t *reg,
                                               const char *id) {
    if (reg == NULL || id == NULL) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->items[i].id, id) == 0) return &reg->items[i];
    return NULL;
}

const rb_device_profile_t *rb_registry_find_address(rb_registry_t *reg,
                                                    const char *address) {
    if (reg == NULL || address == NULL) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->items[i].address, address) == 0)
            return &reg->items[i];
    return NULL;
}

size_t rb_registry_count(rb_registry_t *reg) {
    return reg ? reg->count : 0;
}

const rb_device_profile_t *rb_registry_get_at(rb_registry_t *reg,
                                              size_t index) {
    if (reg == NULL || index >= reg->count) return NULL;
    return &reg->items[index];
}

static void json_escape(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
}

rb_status_t rb_registry_save(rb_registry_t *reg, const char *path) {
    if (reg == NULL || path == NULL) return RB_ERROR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (f == NULL) return RB_ERROR_INTERNAL;
    fprintf(f, "{\"devices\":[");
    for (size_t i = 0; i < reg->count; i++) {
        const rb_device_profile_t *p = &reg->items[i];
        if (i > 0) fprintf(f, ",");
        fprintf(f, "{\"id\":\"");
        json_escape(f, p->id);
        fprintf(f, "\",\"name\":\"");
        json_escape(f, p->name);
        fprintf(f, "\",\"address\":\"");
        json_escape(f, p->address);
        fprintf(f, "\",\"last_rssi_dbm\":%d,\"last_seen_ms\":%llu,",
                (int)p->last_rssi_dbm,
                (unsigned long long)p->last_seen_ms);
        fprintf(f, "\"services\":[");
        for (size_t j = 0; j < p->service_count; j++) {
            char uuid_str[64];
            rb_uuid_to_string(&p->services[j], uuid_str,
                              sizeof(uuid_str));
            fprintf(f, "%s\"%s\"", j > 0 ? "," : "", uuid_str);
        }
        fprintf(f, "],\"notes\":\"");
        json_escape(f, p->notes);
        fprintf(f, "\"}");
    }
    fprintf(f, "]}\n");
    if (fclose(f) != 0) return RB_ERROR_INTERNAL;
    return RB_OK;
}

/* Minimal JSON reader: enough for our own writer's output. */
typedef struct json_reader {
    const char *p;
} json_reader_t;

static void jr_ws(json_reader_t *r) {
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' ||
           *r->p == '\r')
        r->p++;
}

static int jr_expect(json_reader_t *r, char c) {
    jr_ws(r);
    if (*r->p != c) return 0;
    r->p++;
    return 1;
}

/* Parse a quoted string into out (unescaping \" \\ \n \t). */
static int jr_string(json_reader_t *r, char *out, size_t out_len) {
    jr_ws(r);
    if (*r->p != '"') return 0;
    r->p++;
    size_t n = 0;
    while (*r->p && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\' && *r->p) {
            char e = *r->p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else c = e; /* \" \\ \/ and anything else: literal */
        }
        if (n + 1 < out_len) out[n++] = c;
    }
    if (*r->p != '"') return 0;
    r->p++;
    out[n] = '\0';
    return 1;
}

static int jr_number(json_reader_t *r, long long *out) {
    jr_ws(r);
    char *end = NULL;
    long long v = strtoll(r->p, &end, 10);
    if (end == r->p) return 0;
    r->p = end;
    *out = v;
    return 1;
}

/* Find "key": in the current object and leave the reader after the
 * colon. Returns 0 when absent. */
static int jr_find_key(json_reader_t *r, const char *key) {
    const char *save = r->p;
    jr_ws(r);
    while (*r->p && *r->p != '}') {
        char kbuf[64];
        if (!jr_string(r, kbuf, sizeof(kbuf))) break;
        if (!jr_expect(r, ':')) break;
        if (strcmp(kbuf, key) == 0) return 1;
        /* Skip the value: string, number, array, or object. */
        jr_ws(r);
        if (*r->p == '"') {
            char tmp[1024];
            if (!jr_string(r, tmp, sizeof(tmp))) break;
        } else if (*r->p == '[') {
            int depth = 0;
            while (*r->p) {
                if (*r->p == '[') depth++;
                else if (*r->p == ']') {
                    depth--;
                    r->p++;
                    if (depth == 0) break;
                    continue;
                } else if (*r->p == '"') {
                    char tmp[1024];
                    if (!jr_string(r, tmp, sizeof(tmp))) break;
                    continue;
                }
                r->p++;
            }
        } else if (*r->p == '{') {
            int depth = 0;
            while (*r->p) {
                if (*r->p == '{') depth++;
                else if (*r->p == '}') {
                    depth--;
                    r->p++;
                    if (depth == 0) break;
                    continue;
                } else if (*r->p == '"') {
                    char tmp[1024];
                    if (!jr_string(r, tmp, sizeof(tmp))) break;
                    continue;
                }
                r->p++;
            }
        } else {
            while (*r->p && *r->p != ',' && *r->p != '}') r->p++;
        }
        jr_ws(r);
        if (*r->p == ',') {
            r->p++;
            continue;
        }
        break;
    }
    r->p = save;
    return 0;
}

rb_status_t rb_registry_load(rb_registry_t *reg, const char *path) {
    if (reg == NULL || path == NULL) return RB_ERROR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "r");
    if (f == NULL) return RB_ERROR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 1024 * 1024) {
        fclose(f);
        return RB_ERROR_INVALID_ARGUMENT;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(f);
        return RB_ERROR_NO_MEMORY;
    }
    if (size > 0 && fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        fclose(f);
        return RB_ERROR_INTERNAL;
    }
    text[size] = '\0';
    fclose(f);

    json_reader_t r = {text};
    reg->count = 0;
    rb_status_t st = RB_OK;
    /* The reader starts AT the top-level '{'; step inside it before
     * searching for the "devices" key (jr_find_key expects to be
     * positioned inside an object, not at its brace). */
    if (!jr_expect(&r, '{') || !jr_find_key(&r, "devices") ||
        !jr_expect(&r, '[')) {
        st = RB_ERROR_INVALID_ARGUMENT;
        goto done;
    }
    jr_ws(&r);
    while (*r.p && *r.p != ']') {
        if (!jr_expect(&r, '{')) {
            st = RB_ERROR_INVALID_ARGUMENT;
            goto done;
        }
        rb_device_profile_t p;
        memset(&p, 0, sizeof(p));
        /* Each field is optional except id; missing fields stay 0. */
        json_reader_t field = r;
        if (jr_find_key(&field, "id"))
            jr_string(&field, p.id, sizeof(p.id));
        field = r;
        if (jr_find_key(&field, "name"))
            jr_string(&field, p.name, sizeof(p.name));
        field = r;
        if (jr_find_key(&field, "address"))
            jr_string(&field, p.address, sizeof(p.address));
        field = r;
        if (jr_find_key(&field, "last_rssi_dbm")) {
            long long v = 0;
            if (jr_number(&field, &v)) p.last_rssi_dbm = (int8_t)v;
        }
        field = r;
        if (jr_find_key(&field, "last_seen_ms")) {
            long long v = 0;
            if (jr_number(&field, &v)) p.last_seen_ms = (uint64_t)v;
        }
        field = r;
        if (jr_find_key(&field, "services") && jr_expect(&field, '[')) {
            jr_ws(&field);
            while (*field.p && *field.p != ']' &&
                   p.service_count < 8) {
                char uuid_str[64];
                if (!jr_string(&field, uuid_str, sizeof(uuid_str)))
                    break;
                if (rb_uuid_from_string(uuid_str,
                                        &p.services[p.service_count]) ==
                    RB_OK)
                    p.service_count++;
                jr_ws(&field);
                if (*field.p == ',') field.p++;
                jr_ws(&field);
            }
            jr_expect(&field, ']');
        }
        field = r;
        if (jr_find_key(&field, "notes"))
            jr_string(&field, p.notes, sizeof(p.notes));
        /* Advance past this object. r is positioned just after the
         * object's opening brace, so depth starts at 1: the matching
         * closing brace brings it to 0. */
        {
            int depth = 1;
            while (*r.p) {
                if (*r.p == '{') depth++;
                else if (*r.p == '}') {
                    depth--;
                    r.p++;
                    if (depth == 0) break;
                    continue;
                } else if (*r.p == '"') {
                    char tmp[1024];
                    if (!jr_string(&r, tmp, sizeof(tmp))) break;
                    continue;
                }
                r.p++;
            }
        }
        if (p.id[0] != '\0' && reg->count < RB_REG_MAX)
            reg->items[reg->count++] = p;
        jr_ws(&r);
        if (*r.p == ',') r.p++;
        jr_ws(&r);
    }
done:
    free(text);
    return st;
}

/* ------------------------------------------------------------------ */
/* Reliable data transfer                                              */
/* ------------------------------------------------------------------ */

/* Frame: [0x01][flags][len u16 BE][seq u32 BE][payload][crc32 u32 BE].
 * flags bit 0 = LAST. CRC covers flags+len+seq+payload. Sequences
 * restart at 0 for each message; the receiver resets after each
 * completed message. */
#define RB_RT_SOH 0x01
#define RB_RT_ACK 0x06
#define RB_RT_FLAG_LAST 0x01
#define RB_RT_MAX_ATTEMPTS 5
#define RB_RT_ACK_TIMEOUT_MS 200

struct rb_rt_sender {
    uint32_t next_seq; /* first sequence of the next message */
};

struct rb_rt_receiver {
    uint32_t expected_seq; /* persistent across messages */
    uint8_t msg[RB_RT_MAX_MESSAGE];
    size_t msg_len;
};

rb_rt_sender_t *rb_rt_sender_create(void) {
    return (rb_rt_sender_t *)calloc(1, sizeof(rb_rt_sender_t));
}

void rb_rt_sender_destroy(rb_rt_sender_t *s) { free(s); }

rb_rt_receiver_t *rb_rt_receiver_create(void) {
    return (rb_rt_receiver_t *)calloc(1, sizeof(rb_rt_receiver_t));
}

void rb_rt_receiver_destroy(rb_rt_receiver_t *r) { free(r); }

static size_t rt_build_frame(uint8_t *out, size_t out_cap, uint8_t flags,
                             uint32_t seq, const uint8_t *payload,
                             size_t payload_len) {
    size_t total = 1 + 1 + 2 + 4 + payload_len + 4;
    if (out_cap < total) return 0;
    out[0] = RB_RT_SOH;
    out[1] = flags;
    out[2] = (uint8_t)(payload_len >> 8);
    out[3] = (uint8_t)(payload_len);
    out[4] = (uint8_t)(seq >> 24);
    out[5] = (uint8_t)(seq >> 16);
    out[6] = (uint8_t)(seq >> 8);
    out[7] = (uint8_t)(seq);
    if (payload_len > 0) memcpy(out + 8, payload, payload_len);
    uint32_t crc = rb_crc32_ieee(out + 1, 1 + 2 + 4 + payload_len);
    size_t cpos = 8 + payload_len;
    out[cpos] = (uint8_t)(crc >> 24);
    out[cpos + 1] = (uint8_t)(crc >> 16);
    out[cpos + 2] = (uint8_t)(crc >> 8);
    out[cpos + 3] = (uint8_t)(crc);
    return total;
}

static size_t rt_build_ack(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (out_cap < 9) return 0;
    out[0] = RB_RT_ACK;
    out[1] = (uint8_t)(seq >> 24);
    out[2] = (uint8_t)(seq >> 16);
    out[3] = (uint8_t)(seq >> 8);
    out[4] = (uint8_t)(seq);
    uint32_t crc = rb_crc32_ieee(out + 1, 4);
    out[5] = (uint8_t)(crc >> 24);
    out[6] = (uint8_t)(crc >> 16);
    out[7] = (uint8_t)(crc >> 8);
    out[8] = (uint8_t)(crc);
    return 9;
}

static int rt_parse_ack(const uint8_t *buf, size_t len, uint32_t want_seq) {
    if (len != 9 || buf[0] != RB_RT_ACK) return 0;
    uint32_t seq = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
                   ((uint32_t)buf[3] << 8) | (uint32_t)buf[4];
    if (seq != want_seq) return 0;
    uint32_t crc = ((uint32_t)buf[5] << 24) | ((uint32_t)buf[6] << 16) |
                   ((uint32_t)buf[7] << 8) | (uint32_t)buf[8];
    return crc == rb_crc32_ieee(buf + 1, 4);
}

rb_status_t rb_rt_send(rb_rt_sender_t *s, rb_rt_send_fn send,
                       rb_rt_recv_fn recv, void *ud, const uint8_t *data,
                       size_t len) {
    if (s == NULL || send == NULL || recv == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (len > RB_RT_MAX_MESSAGE) return RB_ERROR_INVALID_ARGUMENT;
    if (len > 0 && data == NULL) return RB_ERROR_INVALID_ARGUMENT;
    size_t frame_count = len == 0 ? 1 : (len + RB_RT_MAX_PAYLOAD - 1) /
                                            RB_RT_MAX_PAYLOAD;
    uint8_t frame[1 + 1 + 2 + 4 + RB_RT_MAX_PAYLOAD + 4];
    uint8_t ackbuf[64];
    /* Sequences are persistent: the first frame of this message
     * uses s->next_seq, and next_seq advances only on full success,
     * so a failed message can be retried from the same base. */
    uint32_t base_seq = s->next_seq;
    for (size_t fi = 0; fi < frame_count; fi++) {
        size_t off = fi * RB_RT_MAX_PAYLOAD;
        size_t plen = len - off;
        if (plen > RB_RT_MAX_PAYLOAD) plen = RB_RT_MAX_PAYLOAD;
        uint8_t flags = fi + 1 == frame_count ? RB_RT_FLAG_LAST : 0;
        size_t flen = rt_build_frame(frame, sizeof(frame), flags,
                                     base_seq + (uint32_t)fi,
                                     len > 0 ? data + off : NULL, plen);
        if (flen == 0) return RB_ERROR_INTERNAL;
        int acked = 0;
        for (int attempt = 0; attempt < RB_RT_MAX_ATTEMPTS; attempt++) {
            rb_status_t st = send(ud, frame, flen);
            if (st != RB_OK) return st;
            size_t ack_len = 0;
            st = recv(ud, ackbuf, sizeof(ackbuf), &ack_len,
                      RB_RT_ACK_TIMEOUT_MS);
            if (st == RB_OK) {
                if (rt_parse_ack(ackbuf, ack_len,
                                 base_seq + (uint32_t)fi)) {
                    acked = 1;
                    break;
                }
                /* Wrong ACK: resend (the receiver dedups). */
            } else if (st != RB_ERROR_TIMEOUT) {
                return st;
            }
            /* Timeout or bad ACK: resend (up to max attempts). */
        }
        if (!acked) return RB_ERROR_TIMEOUT;
    }
    s->next_seq = base_seq + (uint32_t)frame_count;
    return RB_OK;
}

rb_status_t rb_rt_receiver_feed(rb_rt_receiver_t *r,
                                const uint8_t *frame, size_t frame_len,
                                uint8_t *ack_out, size_t ack_cap,
                                size_t *ack_len, uint8_t *msg_out,
                                size_t msg_cap, size_t *msg_len,
                                int *msg_complete) {
    if (r == NULL || frame == NULL || ack_out == NULL ||
        ack_len == NULL || msg_out == NULL || msg_len == NULL ||
        msg_complete == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    *ack_len = 0;
    *msg_len = 0;
    *msg_complete = 0;
    if (frame_len < 12 || frame[0] != RB_RT_SOH)
        return RB_ERROR_INVALID_ARGUMENT;
    uint8_t flags = frame[1];
    size_t plen = ((size_t)frame[2] << 8) | (size_t)frame[3];
    if (frame_len != 1 + 1 + 2 + 4 + plen + 4)
        return RB_ERROR_INVALID_ARGUMENT;
    uint32_t seq = ((uint32_t)frame[4] << 24) |
                   ((uint32_t)frame[5] << 16) |
                   ((uint32_t)frame[6] << 8) | (uint32_t)frame[7];
    size_t cpos = 8 + plen;
    uint32_t crc = ((uint32_t)frame[cpos] << 24) |
                   ((uint32_t)frame[cpos + 1] << 16) |
                   ((uint32_t)frame[cpos + 2] << 8) |
                   (uint32_t)frame[cpos + 3];
    if (crc != rb_crc32_ieee(frame + 1, 1 + 2 + 4 + plen))
        return RB_ERROR_INVALID_ARGUMENT;
    if (seq < r->expected_seq) {
        /* Duplicate: re-ACK, payload dropped. */
        size_t alen = rt_build_ack(ack_out, ack_cap, seq);
        if (alen == 0) return RB_ERROR_NO_MEMORY;
        *ack_len = alen;
        return RB_OK;
    }
    if (seq > r->expected_seq) {
        /* Out of order: unreachable under stop-and-wait; drop
         * without ACK so the sender retries the missing frame. */
        return RB_ERROR_INVALID_ARGUMENT;
    }
    if (r->msg_len + plen > sizeof(r->msg))
        return RB_ERROR_NO_MEMORY;
    if (plen > 0) memcpy(r->msg + r->msg_len, frame + 8, plen);
    r->msg_len += plen;
    r->expected_seq++;
    size_t alen = rt_build_ack(ack_out, ack_cap, seq);
    if (alen == 0) return RB_ERROR_NO_MEMORY;
    *ack_len = alen;
    if (flags & RB_RT_FLAG_LAST) {
        if (r->msg_len > msg_cap) return RB_ERROR_NO_MEMORY;
        memcpy(msg_out, r->msg, r->msg_len);
        *msg_len = r->msg_len;
        *msg_complete = 1;
        r->msg_len = 0;
        /* NOTE: expected_seq is NOT reset here — sequences are
         * persistent across messages (see rb_rt_send). */
    }
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Firmware update transport                                           */
/* ------------------------------------------------------------------ */

struct rb_fw_receiver {
    rb_fw_manifest_t manifest;
    uint8_t *image;
    uint8_t *received; /* 1 byte per chunk */
    size_t received_count;
};

rb_status_t rb_fw_manifest_build(const uint8_t *data, size_t len,
                                 rb_fw_manifest_t *out) {
    if (out == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (len > 0 && data == NULL) return RB_ERROR_INVALID_ARGUMENT;
    out->total_len = len;
    out->chunk_size = RB_FW_CHUNK_SIZE;
    out->chunk_count =
        len == 0 ? 0 : (len + RB_FW_CHUNK_SIZE - 1) / RB_FW_CHUNK_SIZE;
    rb_sha256(data, len, out->image_sha256);
    return RB_OK;
}

size_t rb_fw_chunk_packet_len(size_t data_len) {
    return 4 + 2 + 32 + data_len;
}

rb_status_t rb_fw_chunk_build(const uint8_t *data, size_t len,
                              const rb_fw_manifest_t *manifest,
                              size_t index, uint8_t *packet,
                              size_t packet_cap, size_t *packet_len) {
    if (data == NULL || manifest == NULL || packet == NULL ||
        packet_len == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (index >= manifest->chunk_count)
        return RB_ERROR_INVALID_ARGUMENT;
    if (len != manifest->total_len) return RB_ERROR_INVALID_ARGUMENT;
    size_t off = index * manifest->chunk_size;
    size_t clen = manifest->total_len - off;
    if (clen > manifest->chunk_size) clen = manifest->chunk_size;
    size_t total = rb_fw_chunk_packet_len(clen);
    if (packet_cap < total) return RB_ERROR_NO_MEMORY;
    packet[0] = (uint8_t)(index >> 24);
    packet[1] = (uint8_t)(index >> 16);
    packet[2] = (uint8_t)(index >> 8);
    packet[3] = (uint8_t)(index);
    packet[4] = (uint8_t)(clen >> 8);
    packet[5] = (uint8_t)(clen);
    rb_sha256(data + off, clen, packet + 6);
    memcpy(packet + 38, data + off, clen);
    *packet_len = total;
    return RB_OK;
}

rb_fw_receiver_t *rb_fw_receiver_create(
    const rb_fw_manifest_t *manifest) {
    if (manifest == NULL) return NULL;
    rb_fw_receiver_t *r =
        (rb_fw_receiver_t *)calloc(1, sizeof(rb_fw_receiver_t));
    if (r == NULL) return NULL;
    r->manifest = *manifest;
    r->image = (uint8_t *)calloc(
        manifest->total_len > 0 ? manifest->total_len : 1, 1);
    r->received = (uint8_t *)calloc(
        manifest->chunk_count > 0 ? manifest->chunk_count : 1, 1);
    if (r->image == NULL || r->received == NULL) {
        free(r->image);
        free(r->received);
        free(r);
        return NULL;
    }
    return r;
}

void rb_fw_receiver_destroy(rb_fw_receiver_t *r) {
    if (r == NULL) return;
    free(r->image);
    free(r->received);
    free(r);
}

rb_status_t rb_fw_receiver_feed(rb_fw_receiver_t *r,
                                const uint8_t *packet, size_t packet_len) {
    if (r == NULL || packet == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (packet_len < 38) return RB_ERROR_INVALID_ARGUMENT;
    size_t index = ((size_t)packet[0] << 24) | ((size_t)packet[1] << 16) |
                   ((size_t)packet[2] << 8) | (size_t)packet[3];
    size_t clen = ((size_t)packet[4] << 8) | (size_t)packet[5];
    if (index >= r->manifest.chunk_count) return RB_ERROR_INVALID_ARGUMENT;
    if (packet_len != 38 + clen) return RB_ERROR_INVALID_ARGUMENT;
    size_t off = index * r->manifest.chunk_size;
    size_t expect = r->manifest.total_len - off;
    if (expect > r->manifest.chunk_size) expect = r->manifest.chunk_size;
    if (clen != expect) return RB_ERROR_INVALID_ARGUMENT;
    uint8_t digest[32];
    rb_sha256(packet + 38, clen, digest);
    if (memcmp(digest, packet + 6, 32) != 0) {
        /* Corrupt chunk: rejected, may be re-sent. */
        return RB_ERROR_INTERNAL;
    }
    if (r->received[index] == 0) {
        r->received[index] = 1;
        r->received_count++;
    }
    memcpy(r->image + off, packet + 38, clen);
    return RB_OK;
}

rb_status_t rb_fw_receiver_finish(rb_fw_receiver_t *r, uint8_t *out,
                                  size_t out_cap, size_t *out_len) {
    if (r == NULL || out == NULL || out_len == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (r->received_count != r->manifest.chunk_count)
        return RB_ERROR_NOT_FOUND; /* chunks still missing */
    uint8_t digest[32];
    rb_sha256(r->image, r->manifest.total_len, digest);
    if (memcmp(digest, r->manifest.image_sha256, 32) != 0)
        return RB_ERROR_INTERNAL;
    if (out_cap < r->manifest.total_len) return RB_ERROR_NO_MEMORY;
    memcpy(out, r->image, r->manifest.total_len);
    *out_len = r->manifest.total_len;
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Secure data channels (OpenSSL only)                                 */
/* ------------------------------------------------------------------ */

#ifdef RB_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/kdf.h>

rb_status_t rb_secure_keypair(uint8_t pub[32], uint8_t priv[32]) {
    if (pub == NULL || priv == NULL) return RB_ERROR_INVALID_ARGUMENT;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (kctx == NULL) return RB_ERROR_INTERNAL;
    EVP_PKEY *key = NULL;
    rb_status_t st = RB_OK;
    if (EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_keygen(kctx, &key) <= 0 || key == NULL) {
        st = RB_ERROR_INTERNAL;
        goto done;
    }
    size_t plen = 32, slen = 32;
    if (EVP_PKEY_get_raw_public_key(key, pub, &plen) <= 0 ||
        plen != 32 ||
        EVP_PKEY_get_raw_private_key(key, priv, &slen) <= 0 ||
        slen != 32)
        st = RB_ERROR_INTERNAL;
done:
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(kctx);
    return st;
}

rb_status_t rb_secure_channel_init(rb_secure_channel_t *ch,
                                   const uint8_t my_priv[32],
                                   const uint8_t peer_pub[32],
                                   int is_initiator) {
    if (ch == NULL || my_priv == NULL || peer_pub == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    memset(ch, 0, sizeof(*ch));
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
                                                  my_priv, 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL,
                                                 peer_pub, 32);
    if (mine == NULL || peer == NULL) {
        EVP_PKEY_free(mine);
        EVP_PKEY_free(peer);
        return RB_ERROR_INTERNAL;
    }
    uint8_t shared[32];
    size_t shared_len = sizeof(shared);
    EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(mine, NULL);
    rb_status_t st = RB_OK;
    if (dctx == NULL ||
        EVP_PKEY_derive_init(dctx) <= 0 ||
        EVP_PKEY_derive_set_peer(dctx, peer) <= 0 ||
        EVP_PKEY_derive(dctx, shared, &shared_len) <= 0 ||
        shared_len != 32) {
        st = RB_ERROR_INTERNAL;
        goto done;
    }
    /* HKDF-SHA256: salt = 32 zero bytes, info =
     * "radiobridge-secure-v1", output 64 bytes (key A, key B). */
    {
        static const uint8_t salt[32] = {0};
        static const char info[] = "radiobridge-secure-v1";
        uint8_t keys[64];
        size_t keys_len = sizeof(keys);
        EVP_PKEY_CTX *hctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
        if (hctx == NULL) {
            st = RB_ERROR_INTERNAL;
            goto done;
        }
        if (EVP_PKEY_derive_init(hctx) <= 0 ||
            EVP_PKEY_CTX_set_hkdf_md(hctx, EVP_sha256()) <= 0 ||
            EVP_PKEY_CTX_set1_hkdf_salt(hctx, salt, sizeof(salt)) <= 0 ||
            EVP_PKEY_CTX_set1_hkdf_key(hctx, shared, sizeof(shared)) <=
                0 ||
            EVP_PKEY_CTX_add1_hkdf_info(hctx, (const uint8_t *)info,
                                        sizeof(info) - 1) <= 0 ||
            EVP_PKEY_derive(hctx, keys, &keys_len) <= 0 ||
            keys_len != 64) {
            EVP_PKEY_CTX_free(hctx);
            st = RB_ERROR_INTERNAL;
            goto done;
        }
        EVP_PKEY_CTX_free(hctx);
        if (is_initiator) {
            memcpy(ch->tx_key, keys, 32);
            memcpy(ch->rx_key, keys + 32, 32);
        } else {
            memcpy(ch->tx_key, keys + 32, 32);
            memcpy(ch->rx_key, keys, 32);
        }
        memset(keys, 0, sizeof(keys));
    }
    ch->tx_seq = 0;
    ch->rx_seq = 0;
    ch->established = 1;
done:
    memset(shared, 0, sizeof(shared));
    EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(peer);
    return st;
}

static void rb_nonce_for(uint64_t seq, uint8_t nonce[12]) {
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] = (uint8_t)(seq >> (8 * i));
}

rb_status_t rb_secure_encrypt(rb_secure_channel_t *ch,
                              const uint8_t *plaintext, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len) {
    if (ch == NULL || !ch->established || out == NULL ||
        out_len == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (len > 0 && plaintext == NULL) return RB_ERROR_INVALID_ARGUMENT;
    size_t total = 12 + len + 16;
    if (out_cap < total) return RB_ERROR_NO_MEMORY;
    uint8_t nonce[12];
    rb_nonce_for(ch->tx_seq, nonce);
    memcpy(out, nonce, 12);
    EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
    if (cctx == NULL) return RB_ERROR_INTERNAL;
    int clen = 0, flen = 0;
    rb_status_t st = RB_OK;
    if (EVP_EncryptInit_ex(cctx, EVP_aes_256_gcm(), NULL, NULL, NULL) !=
            1 ||
        EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) !=
            1 ||
        EVP_EncryptInit_ex(cctx, NULL, NULL, ch->tx_key, nonce) != 1 ||
        (len > 0 && EVP_EncryptUpdate(cctx, out + 12, &clen, plaintext,
                                      (int)len) != 1) ||
        EVP_EncryptFinal_ex(cctx, out + 12 + len, &flen) != 1 ||
        EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_GET_TAG, 16,
                            out + 12 + len) != 1) {
        st = RB_ERROR_INTERNAL;
        goto done;
    }
    (void)clen;
    (void)flen;
    ch->tx_seq++;
    *out_len = total;
done:
    EVP_CIPHER_CTX_free(cctx);
    return st;
}

rb_status_t rb_secure_decrypt(rb_secure_channel_t *ch,
                              const uint8_t *record, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len) {
    if (ch == NULL || !ch->established || record == NULL ||
        out == NULL || out_len == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (len < 12 + 16) return RB_ERROR_INVALID_ARGUMENT;
    /* Replay / out-of-order check: nonce counter must equal the
     * expected receive sequence. */
    uint64_t seq = 0;
    for (int i = 0; i < 8; i++)
        seq = (seq << 8) | record[4 + i];
    if (seq != ch->rx_seq) return RB_ERROR_INTERNAL;
    size_t ct_len = len - 12 - 16;
    if (out_cap < ct_len) return RB_ERROR_NO_MEMORY;
    EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
    if (cctx == NULL) return RB_ERROR_INTERNAL;
    int plen = 0, flen = 0;
    rb_status_t st = RB_OK;
    if (EVP_DecryptInit_ex(cctx, EVP_aes_256_gcm(), NULL, NULL, NULL) !=
            1 ||
        EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) !=
            1 ||
        EVP_DecryptInit_ex(cctx, NULL, NULL, ch->rx_key, record) != 1 ||
        (ct_len > 0 &&
         EVP_DecryptUpdate(cctx, out, &plen, record + 12,
                           (int)ct_len) != 1) ||
        EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_TAG, 16,
                            (void *)(record + 12 + ct_len)) != 1 ||
        EVP_DecryptFinal_ex(cctx, out + ct_len, &flen) != 1) {
        st = RB_ERROR_INTERNAL; /* includes tag mismatch */
        goto done;
    }
    (void)plen;
    (void)flen;
    ch->rx_seq++;
    *out_len = ct_len;
done:
    EVP_CIPHER_CTX_free(cctx);
    return st;
}

#else /* !RB_HAVE_OPENSSL */

rb_status_t rb_secure_keypair(uint8_t pub[32], uint8_t priv[32]) {
    (void)pub;
    (void)priv;
    return RB_ERROR_NOT_SUPPORTED;
}

rb_status_t rb_secure_channel_init(rb_secure_channel_t *ch,
                                   const uint8_t my_priv[32],
                                   const uint8_t peer_pub[32],
                                   int is_initiator) {
    (void)ch;
    (void)my_priv;
    (void)peer_pub;
    (void)is_initiator;
    return RB_ERROR_NOT_SUPPORTED;
}

rb_status_t rb_secure_encrypt(rb_secure_channel_t *ch,
                              const uint8_t *plaintext, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len) {
    (void)ch;
    (void)plaintext;
    (void)len;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return RB_ERROR_NOT_SUPPORTED;
}

rb_status_t rb_secure_decrypt(rb_secure_channel_t *ch,
                              const uint8_t *record, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len) {
    (void)ch;
    (void)record;
    (void)len;
    (void)out;
    (void)out_cap;
    (void)out_len;
    return RB_ERROR_NOT_SUPPORTED;
}

#endif /* RB_HAVE_OPENSSL */

/* ------------------------------------------------------------------ */
/* Multi-device sessions                                               */
/* ------------------------------------------------------------------ */

#define RB_SESS_MAX 8

struct rb_session {
    rb_context_t *ctx;
    char addresses[RB_SESS_MAX][32];
    rb_status_t last_status[RB_SESS_MAX];
    int connected[RB_SESS_MAX];
    int resolved[RB_SESS_MAX];
    size_t count;
};

rb_session_t *rb_session_create(rb_context_t *ctx) {
    if (ctx == NULL) return NULL;
    rb_session_t *s = (rb_session_t *)calloc(1, sizeof(rb_session_t));
    if (s == NULL) return NULL;
    s->ctx = ctx;
    for (size_t i = 0; i < RB_SESS_MAX; i++)
        s->last_status[i] = RB_ERROR_NOT_CONNECTED;
    return s;
}

void rb_session_destroy(rb_session_t *sess) { free(sess); }

rb_status_t rb_session_add_device(rb_session_t *sess,
                                  const char *address) {
    if (sess == NULL || address == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (sess->count >= RB_SESS_MAX) return RB_ERROR_NO_MEMORY;
    for (size_t i = 0; i < sess->count; i++)
        if (strcmp(sess->addresses[i], address) == 0) return RB_OK;
    rb_copy_str(sess->addresses[sess->count], 32, address);
    sess->last_status[sess->count] = RB_ERROR_NOT_CONNECTED;
    sess->connected[sess->count] = 0;
    sess->resolved[sess->count] = 0;
    sess->count++;
    return RB_OK;
}

typedef struct sess_collector {
    rb_session_t *sess;
    rb_mutex_t mu;
} sess_collector_t;

static void sess_on_conn(rb_context_t *ctx,
                         const rb_ble_connection_event_t *ev, void *ud) {
    (void)ctx;
    sess_collector_t *col = (sess_collector_t *)ud;
    rb_mutex_lock(&col->mu);
    for (size_t i = 0; i < col->sess->count; i++) {
        if (strcmp(col->sess->addresses[i], ev->device.address) == 0) {
            switch (ev->state) {
            case RB_BLE_CONNECTED:
                col->sess->connected[i] = 1;
                col->sess->resolved[i] = 1;
                col->sess->last_status[i] = RB_OK;
                break;
            case RB_BLE_FAILED:
                col->sess->connected[i] = 0;
                col->sess->resolved[i] = 1;
                col->sess->last_status[i] = ev->status;
                break;
            case RB_BLE_CONNECTING:
                col->sess->last_status[i] = RB_OK;
                break;
            case RB_BLE_DISCONNECTED:
            case RB_BLE_DISCONNECTING:
                col->sess->connected[i] = 0;
                col->sess->resolved[i] = 1;
                if (col->sess->last_status[i] == RB_OK)
                    col->sess->last_status[i] =
                        RB_ERROR_NOT_CONNECTED;
                break;
            }
            break;
        }
    }
    rb_mutex_unlock(&col->mu);
}

rb_status_t rb_session_connect_all(rb_session_t *sess,
                                   uint32_t timeout_ms) {
    if (sess == NULL) return RB_ERROR_INVALID_ARGUMENT;
    sess_collector_t col;
    col.sess = sess;
    rb_mutex_init(&col.mu);
    for (size_t i = 0; i < sess->count; i++) {
        rb_device_id_t dev;
        rb_copy_str(dev.address, sizeof(dev.address), sess->addresses[i]);
        rb_operation_id_t op = 0;
        rb_status_t st = rb_ble_connect(sess->ctx, &dev, NULL,
                                        sess_on_conn, &col, &op);
        if (st != RB_OK) {
            /* Synchronous rejection: no callback will arrive. */
            rb_mutex_lock(&col.mu);
            sess->resolved[i] = 1;
            sess->last_status[i] = st;
            rb_mutex_unlock(&col.mu);
        }
    }
    uint64_t deadline = rb_now_ms() + timeout_ms;
    for (;;) {
        int pending = 0;
        rb_mutex_lock(&col.mu);
        for (size_t i = 0; i < sess->count; i++)
            if (!sess->resolved[i]) pending = 1;
        rb_mutex_unlock(&col.mu);
        if (!pending) break;
        if (rb_now_ms() >= deadline) break;
        rb_sleep_ms(20);
    }
    rb_mutex_destroy(&col.mu);
    /* Mark still-pending devices as timed out. */
    int any_failed = 0;
    rb_status_t first_fail = RB_ERROR_TIMEOUT;
    for (size_t i = 0; i < sess->count; i++) {
        if (!sess->resolved[i]) {
            sess->resolved[i] = 1;
            sess->last_status[i] = RB_ERROR_TIMEOUT;
        }
        if (!sess->connected[i] && !any_failed) {
            first_fail = sess->last_status[i];
            any_failed = 1;
        }
    }
    return any_failed ? first_fail : RB_OK;
}

rb_status_t rb_session_broadcast_write(rb_session_t *sess,
                                       const rb_uuid_t *svc,
                                       const rb_uuid_t *chr,
                                       const uint8_t *data, size_t len) {
    if (sess == NULL || svc == NULL || chr == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    int successes = 0;
    rb_status_t first_err = RB_ERROR_NOT_CONNECTED;
    for (size_t i = 0; i < sess->count; i++) {
        if (!sess->connected[i]) continue;
        rb_device_id_t dev;
        rb_copy_str(dev.address, sizeof(dev.address), sess->addresses[i]);
        rb_status_t st = rb_ble_write(sess->ctx, &dev, svc, chr, data,
                                      len, 1);
        sess->last_status[i] = st;
        if (st == RB_OK) successes++;
        else if (first_err == RB_ERROR_NOT_CONNECTED) first_err = st;
    }
    return successes > 0 ? RB_OK : first_err;
}

rb_status_t rb_session_disconnect_all(rb_session_t *sess) {
    if (sess == NULL) return RB_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < sess->count; i++) {
        if (!sess->connected[i]) continue;
        rb_device_id_t dev;
        rb_copy_str(dev.address, sizeof(dev.address), sess->addresses[i]);
        rb_status_t st = rb_ble_disconnect(sess->ctx, &dev);
        sess->last_status[i] = st;
        if (st == RB_OK) sess->connected[i] = 0;
    }
    return RB_OK;
}

size_t rb_session_device_count(rb_session_t *sess) {
    return sess ? sess->count : 0;
}

rb_status_t rb_session_device_status(rb_session_t *sess, size_t index,
                                     char *address_out,
                                     size_t address_len,
                                     rb_status_t *status_out) {
    if (sess == NULL || index >= sess->count)
        return RB_ERROR_INVALID_ARGUMENT;
    if (address_out != NULL && address_len > 0)
        rb_copy_str(address_out, address_len, sess->addresses[index]);
    if (status_out != NULL) *status_out = sess->last_status[index];
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Radio operation scheduling                                          */
/* ------------------------------------------------------------------ */

#define RB_SCHED_MAX 16

struct rb_scheduler {
    struct {
        int priority;
        uint32_t interval_ms;
        uint64_t next_due;
        rb_sched_fn_t fn;
        void *ud;
        uint64_t last_gen;
        int active;
    } items[RB_SCHED_MAX];
    size_t count;
    uint64_t gen;
};

rb_scheduler_t *rb_scheduler_create(void) {
    return (rb_scheduler_t *)calloc(1, sizeof(rb_scheduler_t));
}

void rb_scheduler_destroy(rb_scheduler_t *sched) { free(sched); }

rb_status_t rb_scheduler_add(rb_scheduler_t *sched, int priority,
                             uint32_t interval_ms, rb_sched_fn_t fn,
                             void *ud) {
    if (sched == NULL || fn == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (sched->count >= RB_SCHED_MAX) return RB_ERROR_NO_MEMORY;
    size_t i = sched->count++;
    sched->items[i].priority = priority;
    sched->items[i].interval_ms = interval_ms;
    sched->items[i].next_due = 0; /* due on the first run */
    sched->items[i].fn = fn;
    sched->items[i].ud = ud;
    sched->items[i].last_gen = 0;
    sched->items[i].active = 1;
    return RB_OK;
}

int rb_scheduler_run_due(rb_scheduler_t *sched, uint64_t now_ms) {
    if (sched == NULL) return 0;
    sched->gen++;
    int ran = 0;
    for (;;) {
        /* Highest priority due item not yet run this generation. */
        int best = -1;
        for (size_t i = 0; i < sched->count; i++) {
            if (!sched->items[i].active) continue;
            if (sched->items[i].last_gen == sched->gen) continue;
            if (sched->items[i].next_due > now_ms) continue;
            if (best < 0 ||
                sched->items[i].priority > sched->items[best].priority)
                best = (int)i;
        }
        if (best < 0) break;
        sched->items[best].last_gen = sched->gen;
        sched->items[best].next_due =
            now_ms + sched->items[best].interval_ms;
        sched->items[best].fn(sched->items[best].ud);
        ran++;
    }
    return ran;
}

/* ------------------------------------------------------------------ */
/* Power-aware operation                                               */
/* ------------------------------------------------------------------ */

void rb_power_config_balanced(rb_power_config_t *out) {
    if (out == NULL) return;
    out->scan_interval_ms = 10000;
    out->scan_window_ms = 2000;
    out->connect_timeout_ms = 10000;
    out->notify_poll_ms = 150;
    out->background_mode = 0;
}

void rb_power_config_low_power(rb_power_config_t *out) {
    if (out == NULL) return;
    out->scan_interval_ms = 60000;
    out->scan_window_ms = 2000;
    out->connect_timeout_ms = 15000;
    out->notify_poll_ms = 500;
    out->background_mode = 1;
}

void rb_power_config_performance(rb_power_config_t *out) {
    if (out == NULL) return;
    out->scan_interval_ms = 2000;
    out->scan_window_ms = 2000;
    out->connect_timeout_ms = 5000;
    out->notify_poll_ms = 50;
    out->background_mode = 0;
}

rb_status_t rb_power_config_validate(const rb_power_config_t *cfg) {
    if (cfg == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (cfg->scan_interval_ms == 0 || cfg->scan_window_ms == 0 ||
        cfg->connect_timeout_ms == 0 || cfg->notify_poll_ms == 0)
        return RB_ERROR_INVALID_ARGUMENT;
    if (cfg->scan_window_ms > cfg->scan_interval_ms)
        return RB_ERROR_INVALID_ARGUMENT;
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Connection policy engine                                            */
/* ------------------------------------------------------------------ */

int rb_policy_score(const rb_policy_t *policy,
                    const rb_ble_device_t *device,
                    rb_registry_t *registry) {
    if (policy == NULL || device == NULL) return -1;
    for (size_t i = 0; i < policy->blocked_count; i++)
        if (strcmp(policy->blocked[i], device->address) == 0) return -1;
    if (policy->has_min_rssi) {
        if (device->rssi_dbm == 0) return -1; /* not reported */
        if (device->rssi_dbm < policy->min_rssi_dbm) return -1;
    }
    for (size_t i = 0; i < policy->required_count; i++) {
        int found = 0;
        for (size_t j = 0; j < device->service_uuid_count; j++) {
            if (rb_uuid_equals(&policy->required_services[i],
                              &device->service_uuids[j])) {
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }
    int score = (int)device->rssi_dbm + 100;
    for (size_t i = 0; i < policy->preferred_count; i++) {
        if (strcmp(policy->preferred[i], device->address) == 0) {
            score += 1000 - (int)i * 100;
            break;
        }
    }
    if (registry != NULL &&
        rb_registry_find_address(registry, device->address) != NULL)
        score += policy->known_device_bonus;
    return score;
}

size_t rb_policy_rank(const rb_policy_t *policy,
                      const rb_ble_device_t *devices, size_t count,
                      rb_registry_t *registry, size_t *out_indices,
                      size_t max_out) {
    if (policy == NULL || devices == NULL || out_indices == NULL ||
        max_out == 0)
        return 0;
    size_t ranked = 0;
    for (size_t i = 0; i < count; i++) {
        int score = rb_policy_score(policy, &devices[i], registry);
        if (score < 0) continue;
        /* Stable insertion: higher score first, ties keep order. */
        size_t pos = ranked;
        while (pos > 0) {
            int prev_score = rb_policy_score(
                policy, &devices[out_indices[pos - 1]], registry);
            if (prev_score >= score) break;
            pos--;
        }
        if (ranked < max_out) {
            for (size_t j = ranked; j > pos; j--)
                out_indices[j] = out_indices[j - 1];
            out_indices[pos] = i;
            ranked++;
        }
    }
    return ranked;
}
