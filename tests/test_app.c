/* Phase 5 application layer: registry, reliable transfer, firmware,
 * secure channels (OpenSSL-gated), sessions, scheduler, power, policy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radiobridge/rb.h"
#include "radiobridge/rb_app.h"
#include "radiobridge/rb_bluetooth.h"
#include "test_common.h"

/* ------------------------------------------------------------------ */
/* Lossy in-memory link for the reliable-transfer tests                */
/* ------------------------------------------------------------------ */

typedef struct {
    rb_rt_receiver_t *rx;
    uint8_t frames[16][300];
    size_t flens[16];
    int fhead, fcount;
    uint8_t acks[32][16];
    size_t alens[32];
    int ahead, acount;
    unsigned send_count;
    int policy; /* 1 = drop/dup/corrupt, 0 = clean */
    uint8_t last_sent[300];
    size_t last_sent_len;
    uint8_t last_msg[RB_RT_MAX_MESSAGE];
    size_t last_msg_len;
    int got_msg;
} link_t;

static void link_init(link_t *l, int policy) {
    memset(l, 0, sizeof(*l));
    l->rx = rb_rt_receiver_create();
    l->policy = policy;
}

static void link_free(link_t *l) { rb_rt_receiver_destroy(l->rx); }

static rb_status_t link_send(void *ud, const uint8_t *data, size_t len) {
    link_t *l = (link_t *)ud;
    if (len > sizeof(l->last_sent)) return RB_ERROR_INVALID_ARGUMENT;
    memcpy(l->last_sent, data, len);
    l->last_sent_len = len;
    l->send_count++;
    if (l->policy && l->send_count % 3 == 0) {
        return RB_OK; /* dropped */
    }
    int copies = 1;
    if (l->policy && l->send_count % 5 == 0) copies = 2;
    for (int c = 0; c < copies; c++) {
        if (l->fcount >= 16) return RB_ERROR_NO_MEMORY;
        int idx = (l->fhead + l->fcount) % 16;
        memcpy(l->frames[idx], data, len);
        if (l->policy && l->send_count % 7 == 0 && len > 10)
            l->frames[idx][10] ^= 0xFF; /* corrupt one payload byte */
        l->flens[idx] = len;
        l->fcount++;
    }
    return RB_OK;
}

static rb_status_t link_recv(void *ud, uint8_t *buf, size_t cap,
                             size_t *len, uint32_t timeout_ms) {
    (void)timeout_ms;
    link_t *l = (link_t *)ud;
    while (l->fcount > 0) {
        uint8_t frame[300];
        size_t flen = l->flens[l->fhead];
        memcpy(frame, l->frames[l->fhead], flen);
        l->fhead = (l->fhead + 1) % 16;
        l->fcount--;
        uint8_t ack[16];
        size_t ack_len = 0;
        uint8_t msg[RB_RT_MAX_MESSAGE];
        size_t msg_len = 0;
        int complete = 0;
        rb_status_t st = rb_rt_receiver_feed(
            l->rx, frame, flen, ack, sizeof(ack), &ack_len, msg,
            sizeof(msg), &msg_len, &complete);
        if (st == RB_OK && ack_len > 0 && l->acount < 32) {
            int idx = (l->ahead + l->acount) % 32;
            memcpy(l->acks[idx], ack, ack_len);
            l->alens[idx] = ack_len;
            l->acount++;
        }
        if (st == RB_OK && complete) {
            memcpy(l->last_msg, msg, msg_len);
            l->last_msg_len = msg_len;
            l->got_msg = 1;
        }
    }
    if (l->acount > 0) {
        size_t alen = l->alens[l->ahead];
        if (alen > cap) return RB_ERROR_NO_MEMORY;
        memcpy(buf, l->acks[l->ahead], alen);
        *len = alen;
        l->ahead = (l->ahead + 1) % 32;
        l->acount--;
        return RB_OK;
    }
    return RB_ERROR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* Scheduler recording                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char order[64];
    size_t n;
} sched_rec_t;

static void sched_cb_a(void *ud) {
    sched_rec_t *r = (sched_rec_t *)ud;
    r->order[r->n++] = 'A';
    r->order[r->n] = '\0';
}
static void sched_cb_b(void *ud) {
    sched_rec_t *r = (sched_rec_t *)ud;
    r->order[r->n++] = 'B';
    r->order[r->n] = '\0';
}
static void sched_cb_c(void *ud) {
    sched_rec_t *r = (sched_rec_t *)ud;
    r->order[r->n++] = 'C';
    r->order[r->n] = '\0';
}
static void sched_cb_d(void *ud) {
    sched_rec_t *r = (sched_rec_t *)ud;
    r->order[r->n++] = 'D';
    r->order[r->n] = '\0';
}

int main(void) {
    /* CRC32 + SHA-256 vectors. */
    CHECK(rb_crc32_ieee((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    uint8_t digest[32];
    rb_sha256((const uint8_t *)"", 0, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495"
                      "991b7852b855") == 0);
    rb_sha256((const uint8_t *)"abc", 3, digest);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb41"
                      "0ff61f20015ad") == 0);

    /* Registry: add/find/save/load/remove. */
    rb_registry_t *reg = rb_registry_create();
    CHECK(reg != NULL);
    rb_device_profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.id, "heart-1");
    strcpy(p.name, "RB Heart Rate");
    strcpy(p.address, "AA:BB:CC:00:00:01");
    p.last_rssi_dbm = -52;
    p.last_seen_ms = 123456789;
    CHECK_ST(rb_uuid_from_string("180D", &p.services[0]), RB_OK);
    p.service_count = 1;
    strcpy(p.notes, "chest strap");
    CHECK_ST(rb_registry_add(reg, &p), RB_OK);
    memset(&p, 0, sizeof(p));
    strcpy(p.id, "thermo-1");
    strcpy(p.address, "AA:BB:CC:00:00:02");
    p.last_rssi_dbm = -68;
    CHECK_ST(rb_registry_add(reg, &p), RB_OK);
    CHECK(rb_registry_count(reg) == 2);
    CHECK(rb_registry_find_id(reg, "heart-1") != NULL);
    CHECK(strcmp(rb_registry_find_address(reg, "AA:BB:CC:00:00:02")->id,
                 "thermo-1") == 0);
    CHECK(rb_registry_find_id(reg, "nope") == NULL);
    const char *tmpdir = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/rb_app_registry_test.json",
             tmpdir ? tmpdir : "/tmp");
    CHECK_ST(rb_registry_save(reg, path), RB_OK);
    rb_registry_t *reg2 = rb_registry_create();
    CHECK_ST(rb_registry_load(reg2, path), RB_OK);
    CHECK(rb_registry_count(reg2) == 2);
    const rb_device_profile_t *loaded =
        rb_registry_find_id(reg2, "heart-1");
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        CHECK(strcmp(loaded->name, "RB Heart Rate") == 0);
        CHECK(loaded->last_rssi_dbm == -52);
        CHECK(loaded->last_seen_ms == 123456789);
        CHECK(loaded->service_count == 1);
        CHECK(strcmp(loaded->notes, "chest strap") == 0);
    }
    CHECK_ST(rb_registry_remove(reg2, "thermo-1"), RB_OK);
    CHECK(rb_registry_count(reg2) == 1);
    CHECK_ST(rb_registry_remove(reg2, "thermo-1"),
             RB_ERROR_NOT_FOUND);
    CHECK_ST(rb_registry_load(reg2, "/tmp/rb_app_no_such_file.json"),
             RB_ERROR_NOT_FOUND);
    remove(path);
    rb_registry_destroy(reg);
    rb_registry_destroy(reg2);

    /* Reliable transfer over the lossy link. */
    uint8_t msg[600];
    for (int i = 0; i < 600; i++) msg[i] = (uint8_t)(i * 7 + 3);
    link_t link;
    link_init(&link, 1);
    rb_rt_sender_t *sender = rb_rt_sender_create();
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, msg,
                        sizeof(msg)),
             RB_OK);
    CHECK(link.got_msg);
    CHECK(link.last_msg_len == sizeof(msg));
    CHECK(memcmp(link.last_msg, msg, sizeof(msg)) == 0);
    /* Empty message and exact-frame-boundary message. */
    link.got_msg = 0;
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, NULL, 0),
             RB_OK);
    CHECK(link.got_msg);
    CHECK(link.last_msg_len == 0);
    uint8_t msg200[200];
    memset(msg200, 0x5A, sizeof(msg200));
    link.got_msg = 0;
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, msg200,
                        sizeof(msg200)),
             RB_OK);
    CHECK(link.last_msg_len == sizeof(msg200));
    CHECK(memcmp(link.last_msg, msg200, sizeof(msg200)) == 0);
    rb_rt_sender_destroy(sender);
    link_free(&link);

    /* Direct receiver checks: duplicate re-ACK, bad CRC rejected,
     * out-of-order rejected. */
    link_init(&link, 0);
    sender = rb_rt_sender_create();
    uint8_t short_msg[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, short_msg,
                        sizeof(short_msg)),
             RB_OK);
    CHECK(link.last_sent_len > 0);
    rb_rt_receiver_t *rx = rb_rt_receiver_create();
    uint8_t ack[16], out_msg[RB_RT_MAX_MESSAGE];
    size_t ack_len = 0, out_len = 0;
    int complete = 0;
    CHECK_ST(rb_rt_receiver_feed(rx, link.last_sent, link.last_sent_len,
                                 ack, sizeof(ack), &ack_len, out_msg,
                                 sizeof(out_msg), &out_len, &complete),
             RB_OK);
    CHECK(ack_len == 9);
    CHECK(complete == 1);
    CHECK(out_len == sizeof(short_msg));
    CHECK(memcmp(out_msg, short_msg, sizeof(short_msg)) == 0);
    /* Duplicate of the same frame to the SAME receiver: re-ACK, no
     * second delivery (sequences are persistent, so seq 0 is now
     * below the expected sequence). */
    ack_len = 0;
    complete = 0;
    out_len = 0;
    CHECK_ST(rb_rt_receiver_feed(rx, link.last_sent, link.last_sent_len,
                                 ack, sizeof(ack), &ack_len, out_msg,
                                 sizeof(out_msg), &out_len, &complete),
             RB_OK);
    CHECK(ack_len == 9);
    CHECK(complete == 0);
    CHECK(out_len == 0);
    rb_rt_receiver_destroy(rx);
    rx = rb_rt_receiver_create();
    /* Duplicate suppression at message level: send a 250-byte
     * (two-frame) message over the lossy link (which duplicates
     * frames) and confirm exactly one correct delivery. The sender
     * is recreated with the fresh link: sequences are persistent
     * per sender/receiver pair, so a stale sender would start past
     * the fresh receiver's expected sequence. */
    uint8_t two[250];
    for (int i = 0; i < 250; i++) two[i] = (uint8_t)i;
    rb_rt_sender_destroy(sender);
    link_free(&link);
    link_init(&link, 1);
    sender = rb_rt_sender_create();
    link.got_msg = 0;
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, two,
                        sizeof(two)),
             RB_OK);
    CHECK(link.got_msg);
    CHECK(link.last_msg_len == sizeof(two));
    CHECK(memcmp(link.last_msg, two, sizeof(two)) == 0);
    rb_rt_sender_destroy(sender);
    link_free(&link);
    /* Re-capture a frame through a clean link for the CRC test. */
    link_init(&link, 0);
    sender = rb_rt_sender_create();
    CHECK_ST(rb_rt_send(sender, link_send, link_recv, &link, short_msg,
                        sizeof(short_msg)),
             RB_OK);
    uint8_t corrupted[300];
    memcpy(corrupted, link.last_sent, link.last_sent_len);
    corrupted[10] ^= 0xFF;
    ack_len = 0;
    complete = 0;
    CHECK_ST(rb_rt_receiver_feed(rx, corrupted, link.last_sent_len, ack,
                                 sizeof(ack), &ack_len, out_msg,
                                 sizeof(out_msg), &out_len, &complete),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK(ack_len == 0);
    /* Out-of-order: hand-built seq-1 LAST frame, empty payload. */
    uint8_t ooo[12];
    ooo[0] = 0x01;
    ooo[1] = 0x01; /* LAST */
    ooo[2] = 0;
    ooo[3] = 0; /* len 0 */
    ooo[4] = 0;
    ooo[5] = 0;
    ooo[6] = 0;
    ooo[7] = 1; /* seq 1 */
    uint32_t crc = rb_crc32_ieee(ooo + 1, 7);
    ooo[8] = (uint8_t)(crc >> 24);
    ooo[9] = (uint8_t)(crc >> 16);
    ooo[10] = (uint8_t)(crc >> 8);
    ooo[11] = (uint8_t)(crc);
    ack_len = 0;
    CHECK_ST(rb_rt_receiver_feed(rx, ooo, sizeof(ooo), ack, sizeof(ack),
                                 &ack_len, out_msg, sizeof(out_msg),
                                 &out_len, &complete),
             RB_ERROR_INVALID_ARGUMENT);
    rb_rt_receiver_destroy(rx);
    rb_rt_sender_destroy(sender);
    link_free(&link);

    /* Firmware transport: out-of-order chunks, corrupt rejection,
     * missing-chunk failure, hash-verified reassembly. */
    uint8_t image[1500];
    for (int i = 0; i < 1500; i++) image[i] = (uint8_t)(i * 13 + 1);
    rb_fw_manifest_t man;
    CHECK_ST(rb_fw_manifest_build(image, sizeof(image), &man), RB_OK);
    CHECK(man.chunk_count == 3);
    uint8_t digest2[32];
    rb_sha256(image, sizeof(image), digest2);
    CHECK(memcmp(digest2, man.image_sha256, 32) == 0);
    rb_fw_receiver_t *fw = rb_fw_receiver_create(&man);
    CHECK(fw != NULL);
    uint8_t packet[1024];
    size_t plen = 0;
    /* Missing chunk: finish must fail. */
    CHECK_ST(rb_fw_chunk_build(image, sizeof(image), &man, 0, packet,
                               sizeof(packet), &plen),
             RB_OK);
    CHECK_ST(rb_fw_receiver_feed(fw, packet, plen), RB_OK);
    uint8_t assembled[1600];
    size_t assembled_len = 0;
    CHECK_ST(rb_fw_receiver_finish(fw, assembled, sizeof(assembled),
                                   &assembled_len),
             RB_ERROR_NOT_FOUND);
    /* Corrupt chunk 1: rejected, then accepted when correct. */
    CHECK_ST(rb_fw_chunk_build(image, sizeof(image), &man, 1, packet,
                               sizeof(packet), &plen),
             RB_OK);
    packet[40] ^= 0xFF;
    CHECK_ST(rb_fw_receiver_feed(fw, packet, plen),
             RB_ERROR_INTERNAL);
    CHECK_ST(rb_fw_chunk_build(image, sizeof(image), &man, 1, packet,
                               sizeof(packet), &plen),
             RB_OK);
    CHECK_ST(rb_fw_receiver_feed(fw, packet, plen), RB_OK);
    /* Chunk 2 (last, short) out of order relative to 0/1 is fine. */
    CHECK_ST(rb_fw_chunk_build(image, sizeof(image), &man, 2, packet,
                               sizeof(packet), &plen),
             RB_OK);
    CHECK_ST(rb_fw_receiver_feed(fw, packet, plen), RB_OK);
    CHECK_ST(rb_fw_receiver_finish(fw, assembled, sizeof(assembled),
                                   &assembled_len),
             RB_OK);
    CHECK(assembled_len == sizeof(image));
    CHECK(memcmp(assembled, image, sizeof(image)) == 0);
    rb_fw_receiver_destroy(fw);

    /* Secure channels. */
#ifdef RB_HAVE_OPENSSL
    uint8_t pub_a[32], priv_a[32], pub_b[32], priv_b[32];
    CHECK_ST(rb_secure_keypair(pub_a, priv_a), RB_OK);
    CHECK_ST(rb_secure_keypair(pub_b, priv_b), RB_OK);
    rb_secure_channel_t ch_a, ch_b;
    CHECK_ST(rb_secure_channel_init(&ch_a, priv_a, pub_b, 1), RB_OK);
    CHECK_ST(rb_secure_channel_init(&ch_b, priv_b, pub_a, 0), RB_OK);
    const char *plain = "hello secure radio";
    uint8_t record[256], decrypted[256];
    size_t record_len = 0, decrypted_len = 0;
    CHECK_ST(rb_secure_encrypt(&ch_a, (const uint8_t *)plain,
                               strlen(plain), record, sizeof(record),
                               &record_len),
             RB_OK);
    CHECK(record_len == 12 + strlen(plain) + 16);
    CHECK_ST(rb_secure_decrypt(&ch_b, record, record_len, decrypted,
                               sizeof(decrypted), &decrypted_len),
             RB_OK);
    CHECK(decrypted_len == strlen(plain));
    CHECK(memcmp(decrypted, plain, decrypted_len) == 0);
    /* Tamper: flip a ciphertext byte -> tag check fails. */
    uint8_t tampered[256];
    memcpy(tampered, record, record_len);
    tampered[15] ^= 0xFF;
    CHECK_ST(rb_secure_decrypt(&ch_b, tampered, record_len, decrypted,
                               sizeof(decrypted), &decrypted_len),
             RB_ERROR_INTERNAL);
    /* Replay: the same record twice -> second fails. */
    CHECK_ST(rb_secure_decrypt(&ch_b, record, record_len, decrypted,
                               sizeof(decrypted), &decrypted_len),
             RB_ERROR_INTERNAL);
    /* Reverse direction works. */
    const char *reply = "ack";
    CHECK_ST(rb_secure_encrypt(&ch_b, (const uint8_t *)reply,
                               strlen(reply), record, sizeof(record),
                               &record_len),
             RB_OK);
    CHECK_ST(rb_secure_decrypt(&ch_a, record, record_len, decrypted,
                               sizeof(decrypted), &decrypted_len),
             RB_OK);
    CHECK(decrypted_len == strlen(reply));
    /* Wrong peer: a channel derived with a third keypair cannot
     * decrypt A's records. */
    uint8_t pub_c[32], priv_c[32];
    CHECK_ST(rb_secure_keypair(pub_c, priv_c), RB_OK);
    rb_secure_channel_t ch_c;
    CHECK_ST(rb_secure_channel_init(&ch_c, priv_c, pub_a, 0), RB_OK);
    CHECK_ST(rb_secure_encrypt(&ch_a, (const uint8_t *)plain,
                               strlen(plain), record, sizeof(record),
                               &record_len),
             RB_OK);
    CHECK_ST(rb_secure_decrypt(&ch_c, record, record_len, decrypted,
                               sizeof(decrypted), &decrypted_len),
             RB_ERROR_INTERNAL);
#else
    uint8_t pub[32], priv[32];
    CHECK_ST(rb_secure_keypair(pub, priv), RB_ERROR_NOT_SUPPORTED);
#endif

    /* Multi-device sessions on the mock backend. */
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);
    rb_session_t *sess = rb_session_create(ctx);
    CHECK(sess != NULL);
    CHECK_ST(rb_session_add_device(sess, "AA:BB:CC:00:00:01"), RB_OK);
    CHECK_ST(rb_session_add_device(sess, "AA:BB:CC:00:00:02"), RB_OK);
    CHECK_ST(rb_session_add_device(sess, "AA:BB:CC:00:00:03"), RB_OK);
    CHECK(rb_session_device_count(sess) == 3);
    CHECK_ST(rb_session_connect_all(sess, 10000), RB_OK);
    for (size_t i = 0; i < 3; i++) {
        char addr[32];
        rb_status_t st = RB_ERROR_INTERNAL;
        CHECK_ST(rb_session_device_status(sess, i, addr, sizeof(addr),
                                          &st),
                 RB_OK);
        CHECK(st == RB_OK);
    }
    /* Broadcast write to the demo tag's writable char: only device
     * 03 has it, so the broadcast still succeeds (1 of 3). */
    rb_uuid_t svc, chr;
    CHECK_ST(rb_uuid_from_string(
                 "12345678-1234-5678-9ABC-DEF012345678", &svc),
             RB_OK);
    CHECK_ST(rb_uuid_from_string(
                 "12345678-1234-5678-9ABC-DEF012345679", &chr),
             RB_OK);
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK_ST(rb_session_broadcast_write(sess, &svc, &chr, payload,
                                        sizeof(payload)),
             RB_OK);
    {
        rb_status_t st0 = RB_OK, st1 = RB_OK, st2 = RB_OK;
        CHECK_ST(rb_session_device_status(sess, 0, NULL, 0, &st0),
                 RB_OK);
        CHECK_ST(rb_session_device_status(sess, 1, NULL, 0, &st1),
                 RB_OK);
        CHECK_ST(rb_session_device_status(sess, 2, NULL, 0, &st2),
                 RB_OK);
        CHECK(st0 == RB_ERROR_NOT_FOUND);
        CHECK(st1 == RB_ERROR_NOT_FOUND);
        CHECK(st2 == RB_OK);
    }
    CHECK_ST(rb_session_disconnect_all(sess), RB_OK);
    rb_session_destroy(sess);
    /* Unknown address: connect_all reports the async failure. */
    sess = rb_session_create(ctx);
    CHECK_ST(rb_session_add_device(sess, "AA:BB:CC:FF:FF:FF"), RB_OK);
    CHECK_ST(rb_session_connect_all(sess, 10000),
             RB_ERROR_NOT_FOUND);
    rb_session_destroy(sess);
    rb_shutdown(ctx);

    /* Scheduler: priority order, intervals, single-flight. */
    rb_scheduler_t *sched = rb_scheduler_create();
    sched_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    CHECK_ST(rb_scheduler_add(sched, 1, 1000, sched_cb_a, &rec), RB_OK);
    CHECK_ST(rb_scheduler_add(sched, 3, 1000, sched_cb_b, &rec), RB_OK);
    CHECK_ST(rb_scheduler_add(sched, 2, 500, sched_cb_c, &rec), RB_OK);
    CHECK(rb_scheduler_run_due(sched, 1000) == 3);
    CHECK(strcmp(rec.order, "BCA") == 0);
    CHECK(rb_scheduler_run_due(sched, 1000) == 0);
    memset(&rec, 0, sizeof(rec));
    CHECK(rb_scheduler_run_due(sched, 1500) == 1);
    CHECK(strcmp(rec.order, "C") == 0);
    memset(&rec, 0, sizeof(rec));
    CHECK(rb_scheduler_run_due(sched, 2000) == 3);
    CHECK(strcmp(rec.order, "BCA") == 0);
    CHECK_ST(rb_scheduler_add(sched, 0, 0, sched_cb_d, &rec), RB_OK);
    memset(&rec, 0, sizeof(rec));
    CHECK(rb_scheduler_run_due(sched, 2000) == 1);
    CHECK(strcmp(rec.order, "D") == 0);
    rb_scheduler_destroy(sched);

    /* Power configs. */
    rb_power_config_t pw;
    rb_power_config_balanced(&pw);
    CHECK_ST(rb_power_config_validate(&pw), RB_OK);
    CHECK(pw.scan_interval_ms == 10000);
    rb_power_config_low_power(&pw);
    CHECK_ST(rb_power_config_validate(&pw), RB_OK);
    CHECK(pw.background_mode == 1);
    rb_power_config_performance(&pw);
    CHECK_ST(rb_power_config_validate(&pw), RB_OK);
    pw.scan_window_ms = pw.scan_interval_ms + 1;
    CHECK_ST(rb_power_config_validate(&pw),
             RB_ERROR_INVALID_ARGUMENT);
    pw.scan_window_ms = 0;
    CHECK_ST(rb_power_config_validate(&pw),
             RB_ERROR_INVALID_ARGUMENT);

    /* Policy engine. */
    rb_ble_device_t devs[3];
    memset(devs, 0, sizeof(devs));
    strcpy(devs[0].address, "AA:BB:CC:00:00:01");
    devs[0].rssi_dbm = -52;
    CHECK_ST(rb_uuid_from_string("180D", &devs[0].service_uuids[0]),
             RB_OK);
    devs[0].service_uuid_count = 1;
    strcpy(devs[1].address, "AA:BB:CC:00:00:02");
    devs[1].rssi_dbm = -68;
    CHECK_ST(rb_uuid_from_string("1809", &devs[1].service_uuids[0]),
             RB_OK);
    devs[1].service_uuid_count = 1;
    strcpy(devs[2].address, "AA:BB:CC:00:00:03");
    devs[2].rssi_dbm = -81;
    CHECK_ST(rb_uuid_from_string("180D", &devs[2].service_uuids[0]),
             RB_OK);
    devs[2].service_uuid_count = 1;
    rb_registry_t *preg = rb_registry_create();
    rb_device_profile_t kp;
    memset(&kp, 0, sizeof(kp));
    strcpy(kp.id, "heart-1");
    strcpy(kp.address, "AA:BB:CC:00:00:01");
    CHECK_ST(rb_registry_add(preg, &kp), RB_OK);
    rb_policy_t pol;
    memset(&pol, 0, sizeof(pol));
    strcpy(pol.preferred[0], "AA:BB:CC:00:00:02");
    pol.preferred_count = 1;
    pol.has_min_rssi = 1;
    pol.min_rssi_dbm = -70;
    CHECK_ST(rb_uuid_from_string("180D", &pol.required_services[0]),
             RB_OK);
    pol.required_count = 1;
    pol.known_device_bonus = 500;
    /* Only device 01 survives (02 lacks 180D, 03 below RSSI floor). */
    size_t ranked_idx[3];
    size_t n = rb_policy_rank(&pol, devs, 3, preg, ranked_idx, 3);
    CHECK(n == 1);
    CHECK(ranked_idx[0] == 0);
    CHECK(rb_policy_score(&pol, &devs[0], preg) == 48 + 500);
    /* Open policy: preferred device wins, then known, then weakest. */
    memset(&pol, 0, sizeof(pol));
    strcpy(pol.preferred[0], "AA:BB:CC:00:00:02");
    pol.preferred_count = 1;
    pol.known_device_bonus = 500;
    n = rb_policy_rank(&pol, devs, 3, preg, ranked_idx, 3);
    CHECK(n == 3);
    CHECK(ranked_idx[0] == 1);
    CHECK(ranked_idx[1] == 0);
    CHECK(ranked_idx[2] == 2);
    /* Blocked device is excluded. */
    strcpy(pol.blocked[0], "AA:BB:CC:00:00:02");
    pol.blocked_count = 1;
    n = rb_policy_rank(&pol, devs, 3, preg, ranked_idx, 3);
    CHECK(n == 2);
    CHECK(ranked_idx[0] == 0);
    rb_registry_destroy(preg);

    TEST_END();
    return 0;
}
