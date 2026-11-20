#ifndef SLE_BIS_H
#define SLE_BIS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIS_SLOT_US 125u
#define BIS_SLOT_MASK 0x3fffffffu
#define BIS_LINK_BYTES 46u  /* table 36 without GIV/GSKD, 80-bit map */
#define BIS_MAX_PDU 2047u
typedef enum {
    BIS_OK, BIS_IDLE, BIS_BUSY, BIS_INVALID, BIS_MISSED, BIS_CONFLICT,
    BIS_IO, BIS_TIMEOUT, BIS_STALE, BIS_CRC, BIS_FILTERED, BIS_DUPLICATE,
    BIS_STOPPED, BIS_LOST
} bis_result;
typedef enum { BIS_OFF, BIS_SCAN, BIS_WAIT_EXT, BIS_TX, BIS_RX } bis_state;

typedef struct {
    uint32_t base_slot, offset_us, anchor_delay_us, reference_delay_us;
    uint32_t link_base, crc_base, sdu_period_us;
    uint16_t group_slots, event_slots, max_sdu, max_pdu;
    uint8_t set_id, events, new_packets, pilot, map[10];
    bool crc32;
} bis_config;

/* Table 35 service supplies/consumes the management value independently. */
bool bis_link_encode(const bis_config *, uint8_t out[BIS_LINK_BYTES]);
bool bis_link_decode(const uint8_t *, size_t, bis_config *);
bool bis_config_valid(const bis_config *);
/* map indexed by physical channel; returns physical data channel 0..75. */
uint8_t bis_channel(const bis_config *, uint16_t scheduler_slot);

/* Fixed-width A6 fields excluding CRC12 (PHY owns its generation/check). */
typedef struct {
    uint8_t type, seq, group;
    bool last, skip_management;
    uint16_t length;
} bis_a6;
bool bis_a6_encode(const bis_a6 *, uint8_t out[4]); /* low 28 bits */
bool bis_a6_decode(const uint8_t in[4], bis_a6 *);

typedef struct {
    uint64_t token, time_us, cycle;
    uint32_t slot, link_id, crc_seed, duration_us;
    uint8_t index, channel, pilot;
    uint16_t max_pdu;
    bool tx, crc32;
} bis_event;

/* All callbacks run serialized by the controller task. submit copies TX data
   before return. Failed submit owns nothing. cancel drains DMA synchronously.
   release returns a reservation acquired by reserve. pdu returns true only
   after ISOAL takes/copies the borrowed bytes; it must not reenter. */
typedef struct {
    void *ctx;
    bool (*reserve)(void *, const bis_event *);
    void (*release)(void *, uint64_t);
    bool (*submit)(void *, const bis_event *, const bis_a6 *, const uint8_t *);
    void (*cancel)(void *, uint64_t);
    bool (*pdu)(void *, const bis_event *, const bis_a6 *, const uint8_t *);
    bool (*management)(void *, const uint8_t *, size_t, uint64_t);
} bis_port;

typedef struct {
    bis_port port;
    bis_state state;
    bis_config cfg;
    bis_event active;
    uint64_t anchor, deadline, next, stop_at, serial, ext_at;
    uint32_t duration, seen, misses, loss_limit;
    uint8_t ext_channel;
    bool pending, heard;
} bis_controller;

/* Initialize once per radio queue after draining old completions. Duration is
   the reserved radio window. loss_limit is consecutive empty RX events. */
bool bis_init(bis_controller *, bis_port, uint32_t duration, uint32_t loss_limit);
bis_result bis_create(bis_controller *, const bis_config *, uint64_t adv_us);
bis_result bis_scan(bis_controller *, uint64_t deadline_us);
/* Inputs start after the broadcast service strips MAC address fields.
   aux is the four-byte table 27 value; tlv is table 26 type/length/value. */
bis_result bis_basic(bis_controller *, const uint8_t aux[4], uint64_t basic_us);
bis_result bis_extended(bis_controller *, const uint8_t *tlv, size_t len,
                        uint8_t channel, uint64_t ext_us);
bis_result bis_schedule(bis_controller *, uint64_t now, const bis_a6 *tx, const uint8_t *payload);
bis_result bis_receive(bis_controller *, uint64_t token, bool control_crc_ok,
                       bool data_crc_ok, bis_a6, const uint8_t *, size_t);
bis_result bis_done(bis_controller *, uint64_t token);
bis_result bis_tick(bis_controller *, uint64_t now);
bis_result bis_disconnect(bis_controller *, uint32_t effective_slot, uint64_t now);
void bis_stop(bis_controller *);
#endif
