/*
 * RadioBridge — rb_app.h (Phase 5 application layer)
 *
 * Building blocks for applications, on top of the portable C API.
 * Everything here is backend-agnostic and tested on the mock backend
 * (and, for the conformance suite, on the Linux fake-bus backend).
 *
 * Modules:
 *  - Device registry + capability profiles (in-memory + JSON file).
 *  - Reliable data transfer (framing, sequencing, retries, CRC32).
 *  - Firmware update transport (chunked, per-chunk SHA-256, manifest).
 *  - Secure data channels (X25519 + HKDF-SHA256 + AES-256-GCM).
 *    ONLY available when built with OpenSSL (RB_HAVE_OPENSSL);
 *    otherwise every secure function returns RB_ERROR_NOT_SUPPORTED.
 *    No hand-rolled crypto is ever claimed as secure.
 *  - Multi-device sessions (connect/broadcast/disconnect over N
 *    devices, with per-device results).
 *  - Radio operation scheduling (priority queue, single-flight).
 *  - Power-aware operation (app-controlled intervals/timeouts).
 *  - Connection policy engine (preferred/block lists, RSSI floor,
 *    required services -> ranked devices).
 *
 * SHA-256 (FIPS 180-4) is implemented in this layer and used for
 * firmware integrity; it is verified against NIST test vectors in
 * the test suite. It is NOT a substitute for the OpenSSL-backed
 * secure channel.
 */
#ifndef RADIOBRIDGE_RB_APP_H
#define RADIOBRIDGE_RB_APP_H

#include <stddef.h>
#include <stdint.h>

#include "radiobridge/rb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* CRC32 + SHA-256 (integrity primitives)                              */
/* ------------------------------------------------------------------ */

/* CRC32-IEEE (poly 0xEDB88320, init 0xFFFFFFFF, final xor). Test
 * vector: "123456789" -> 0xCBF43926. */
uint32_t rb_crc32_ieee(const uint8_t *data, size_t len);

/* SHA-256 (FIPS 180-4). Test vectors: "" ->
 * e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855,
 * "abc" -> ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad. */
void rb_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* ------------------------------------------------------------------ */
/* Device registry + capability profiles                               */
/* ------------------------------------------------------------------ */

typedef struct rb_device_profile {
    char id[64];     /* app-level id (address by default) */
    char name[128];
    char address[32];
    int8_t last_rssi_dbm;
    uint64_t last_seen_ms;
    rb_uuid_t services[8];
    size_t service_count;
    char notes[256];
} rb_device_profile_t;

typedef struct rb_registry rb_registry_t;

rb_registry_t *rb_registry_create(void);
void rb_registry_destroy(rb_registry_t *reg);
/* Add or replace by id. */
rb_status_t rb_registry_add(rb_registry_t *reg,
                            const rb_device_profile_t *profile);
rb_status_t rb_registry_remove(rb_registry_t *reg, const char *id);
const rb_device_profile_t *rb_registry_find_id(rb_registry_t *reg,
                                               const char *id);
const rb_device_profile_t *rb_registry_find_address(rb_registry_t *reg,
                                                    const char *address);
size_t rb_registry_count(rb_registry_t *reg);
const rb_device_profile_t *rb_registry_get_at(rb_registry_t *reg,
                                              size_t index);
/* JSON file round-trip (hand-rolled, no new dependency). */
rb_status_t rb_registry_save(rb_registry_t *reg, const char *path);
rb_status_t rb_registry_load(rb_registry_t *reg, const char *path);

/* ------------------------------------------------------------------ */
/* Reliable data transfer                                              */
/* ------------------------------------------------------------------ */

/* Frame format: [0x01][flags][len u16 BE][seq u32 BE][payload]
 * [crc32 u32 BE]. flags bit 0 = LAST. Max payload per frame
 * is 200 bytes; longer messages are fragmented. Sequences are
 * persistent per sender/receiver pair and NEVER restart per
 * message: restarting would let a stale ACK from a previous
 * message (same seq 0) falsely acknowledge a new message's first
 * frame. A failed rb_rt_send leaves the sender's sequence at the
 * failed message's base, so retrying the same message replays its
 * frames and the receiver's duplicate suppression makes the retry
 * safe (consumed frames re-ACK, the missing frame is delivered). */
#define RB_RT_MAX_PAYLOAD 200
#define RB_RT_MAX_MESSAGE 8192

typedef struct rb_rt_sender rb_rt_sender_t;
typedef struct rb_rt_receiver rb_rt_receiver_t;

typedef rb_status_t (*rb_rt_send_fn)(void *ud, const uint8_t *data,
                                     size_t len);
typedef rb_status_t (*rb_rt_recv_fn)(void *ud, uint8_t *buf, size_t cap,
                                     size_t *len, uint32_t timeout_ms);

rb_rt_sender_t *rb_rt_sender_create(void);
void rb_rt_sender_destroy(rb_rt_sender_t *s);
/* Send a message stop-and-wait: each frame is retried (up to 5
 * attempts, 200 ms ACK timeout) until its ACK arrives. Returns
 * RB_ERROR_TIMEOUT when a frame exhausts retries. */
rb_status_t rb_rt_send(rb_rt_sender_t *s, rb_rt_send_fn send,
                       rb_rt_recv_fn recv, void *ud, const uint8_t *data,
                       size_t len);

rb_rt_receiver_t *rb_rt_receiver_create(void);
void rb_rt_receiver_destroy(rb_rt_receiver_t *r);
/* Feed one received frame. Always produces an ACK (ack_len > 0) for a
 * well-formed frame, including duplicates (re-ACK, payload dropped).
 * Malformed frames (bad magic/length/CRC) produce no ACK and
 * RB_ERROR_INVALID_ARGUMENT. When a full message completes,
 * *msg_complete is set and the message is in msg_out. */
rb_status_t rb_rt_receiver_feed(rb_rt_receiver_t *r,
                                const uint8_t *frame, size_t frame_len,
                                uint8_t *ack_out, size_t ack_cap,
                                size_t *ack_len, uint8_t *msg_out,
                                size_t msg_cap, size_t *msg_len,
                                int *msg_complete);

/* ------------------------------------------------------------------ */
/* Firmware update transport                                           */
/* ------------------------------------------------------------------ */

#define RB_FW_CHUNK_SIZE 512

typedef struct rb_fw_manifest {
    size_t total_len;
    size_t chunk_count;
    size_t chunk_size;
    uint8_t image_sha256[32];
} rb_fw_manifest_t;

/* Chunk packet: [index u32 BE][len u16 BE][sha256 32][data].
 * chunk_build slices the full image (data/len) at the given index. */
rb_status_t rb_fw_manifest_build(const uint8_t *data, size_t len,
                                 rb_fw_manifest_t *out);
size_t rb_fw_chunk_packet_len(size_t data_len);
rb_status_t rb_fw_chunk_build(const uint8_t *data, size_t len,
                              const rb_fw_manifest_t *manifest,
                              size_t index, uint8_t *packet,
                              size_t packet_cap, size_t *packet_len);

typedef struct rb_fw_receiver rb_fw_receiver_t;

rb_fw_receiver_t *rb_fw_receiver_create(const rb_fw_manifest_t *manifest);
void rb_fw_receiver_destroy(rb_fw_receiver_t *r);
/* Verify the chunk's SHA-256 and store it. Corrupt chunks are
 * rejected (RB_ERROR_INTERNAL) and may be re-sent. Out-of-order
 * chunks are accepted and reassembled by index. */
rb_status_t rb_fw_receiver_feed(rb_fw_receiver_t *r,
                                const uint8_t *packet, size_t packet_len);
/* Verify the assembled image against the manifest hash and copy it
 * out. Fails when any chunk is missing or the hash mismatches. */
rb_status_t rb_fw_receiver_finish(rb_fw_receiver_t *r, uint8_t *out,
                                  size_t out_cap, size_t *out_len);

/* ------------------------------------------------------------------ */
/* Secure data channels (OpenSSL only)                                 */
/* ------------------------------------------------------------------ */

/* Session keys. Wire format for encrypted records:
 * [nonce 12][ciphertext][tag 16]. Nonces are 4 zero bytes + an
 * 8-byte big-endian counter, so each direction's nonce sequence is
 * unique per session. Decryption enforces the expected counter
 * (replay of an old record fails). */
typedef struct rb_secure_channel {
    uint8_t tx_key[32];
    uint8_t rx_key[32];
    uint64_t tx_seq;
    uint64_t rx_seq;
    int established;
} rb_secure_channel_t;

/* X25519 keypair generation (OpenSSL EVP). */
rb_status_t rb_secure_keypair(uint8_t pub[32], uint8_t priv[32]);
/* ECDH + HKDF-SHA256 key agreement. is_initiator selects the
 * direction mapping (initiator tx = key A, responder tx = key B),
 * so two peers with swapped roles derive mirrored keys. */
rb_status_t rb_secure_channel_init(rb_secure_channel_t *ch,
                                   const uint8_t my_priv[32],
                                   const uint8_t peer_pub[32],
                                   int is_initiator);
rb_status_t rb_secure_encrypt(rb_secure_channel_t *ch,
                              const uint8_t *plaintext, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len);
rb_status_t rb_secure_decrypt(rb_secure_channel_t *ch,
                              const uint8_t *record, size_t len,
                              uint8_t *out, size_t out_cap,
                              size_t *out_len);

/* ------------------------------------------------------------------ */
/* Multi-device sessions                                               */
/* ------------------------------------------------------------------ */

typedef struct rb_session rb_session_t;

rb_session_t *rb_session_create(rb_context_t *ctx);
void rb_session_destroy(rb_session_t *sess);
rb_status_t rb_session_add_device(rb_session_t *sess,
                                  const char *address);
/* Connect every device (waits up to timeout_ms total for all
 * CONNECTED events). Per-device results are in last_status[]. */
rb_status_t rb_session_connect_all(rb_session_t *sess,
                                   uint32_t timeout_ms);
/* Write to every connected device; per-device results in
 * last_status[] (RB_OK or the device's error). Returns RB_OK when
 * at least one device accepted the write. */
rb_status_t rb_session_broadcast_write(rb_session_t *sess,
                                       const rb_uuid_t *svc,
                                       const rb_uuid_t *chr,
                                       const uint8_t *data, size_t len);
rb_status_t rb_session_disconnect_all(rb_session_t *sess);
size_t rb_session_device_count(rb_session_t *sess);
rb_status_t rb_session_device_status(rb_session_t *sess, size_t index,
                                     char *address_out,
                                     size_t address_len,
                                     rb_status_t *status_out);

/* ------------------------------------------------------------------ */
/* Radio operation scheduling                                          */
/* ------------------------------------------------------------------ */

typedef void (*rb_sched_fn_t)(void *ud);

typedef struct rb_scheduler rb_scheduler_t;

rb_scheduler_t *rb_scheduler_create(void);
void rb_scheduler_destroy(rb_scheduler_t *sched);
/* Higher priority runs first when several items are due. interval_ms
 * gates re-execution: after running, next_due = now + interval_ms. */
rb_status_t rb_scheduler_add(rb_scheduler_t *sched, int priority,
                             uint32_t interval_ms, rb_sched_fn_t fn,
                             void *ud);
/* Run all due items in priority order, one at a time (never
 * concurrently — single-flight by construction). Returns the number
 * of items run. */
int rb_scheduler_run_due(rb_scheduler_t *sched, uint64_t now_ms);

/* ------------------------------------------------------------------ */
/* Power-aware operation                                               */
/* ------------------------------------------------------------------ */

typedef struct rb_power_config {
    uint32_t scan_interval_ms;
    uint32_t scan_window_ms; /* must be <= scan_interval_ms */
    uint32_t connect_timeout_ms;
    uint32_t notify_poll_ms;
    int background_mode; /* 1 = app backgrounded (long intervals) */
} rb_power_config_t;

void rb_power_config_balanced(rb_power_config_t *out);
void rb_power_config_low_power(rb_power_config_t *out);
void rb_power_config_performance(rb_power_config_t *out);
rb_status_t rb_power_config_validate(const rb_power_config_t *cfg);

/* ------------------------------------------------------------------ */
/* Connection policy engine                                            */
/* ------------------------------------------------------------------ */

typedef struct rb_policy {
    char preferred[8][32];
    size_t preferred_count;
    char blocked[8][32];
    size_t blocked_count;
    int has_min_rssi;
    int8_t min_rssi_dbm;
    rb_uuid_t required_services[4];
    size_t required_count;
    int known_device_bonus; /* score bonus for registry-known devices */
} rb_policy_t;

/* Score a device: -1 = excluded (blocked, below RSSI floor, missing
 * a required service). Otherwise base (rssi + 100) + preferred bonus
 * (1000 - 100 * preferred_index) + known bonus when the address is
 * in the registry (registry may be NULL). */
int rb_policy_score(const rb_policy_t *policy,
                    const rb_ble_device_t *device,
                    rb_registry_t *registry);
/* Rank devices by score (highest first); excluded devices are
 * dropped. out_indices holds indexes into devices[], up to
 * max_out entries. Returns the ranked count. */
size_t rb_policy_rank(const rb_policy_t *policy,
                      const rb_ble_device_t *devices, size_t count,
                      rb_registry_t *registry, size_t *out_indices,
                      size_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_APP_H */
