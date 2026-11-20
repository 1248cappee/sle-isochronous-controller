#ifndef SLE_ISOAL_H
#define SLE_ISOAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ISO_OK, ISO_INVALID, ISO_BUSY, ISO_WAIT, ISO_CRC, ISO_DUPLICATE,
               ISO_EXPIRED, ISO_RETRY, ISO_GAP, ISO_CAPACITY } iso_result;
typedef struct {
    bool linked;
    bool multiplexed; /* Sec. 7.3.3.1: prepend FE FF only with type multiplexing */
    uint16_t max_sdu, max_pdu; /* max useful bytes, not serialized buffer sizes */
    uint32_t group_period_us, sdu_period_us, anchor_delay_us;
    uint8_t discard_cycles;
} iso_config;
#define ISO_SDU_LIMIT 4095u
#define ISO_RX_SLOTS 4u
typedef struct {
    uint8_t pdu_index, group_low, segment, sdu_index;
    const uint8_t *data;
    size_t length;
} iso_fragment;
typedef struct {
    bool used, complete, retired_epoch;
    uint64_t group, due;
    uint8_t index, next_fragment;
    uint16_t length;
    uint8_t data[ISO_SDU_LIMIT];
} iso_rx_slot;
typedef struct {
    iso_rx_slot pending[ISO_RX_SLOTS];
    bool accepted;
    uint64_t last_payload;
} iso_rx;
bool iso_config_valid(const iso_config *);
unsigned iso_fragments(const iso_config *);
unsigned iso_new_packets(const iso_config *);
/* Ordinal in original group, zero based. Caller owns input and output spans. */
iso_result iso_encode(const iso_config *,uint64_t group,uint8_t sdu_index,uint8_t part,
                      const uint8_t *sdu,size_t len,uint8_t *wire,size_t cap,size_t *used);
iso_result iso_decode(const iso_config *,const uint8_t *wire,size_t len,iso_fragment *);
/* group and anchor refer to the first transmission even on a retry. Wire data
   is borrowed only until return. Initialize each stream with iso_rx rx={0}. */
iso_result iso_ingest(iso_rx *,const iso_config *,uint64_t group,uint64_t anchor,
                      unsigned ordinal,const uint8_t *wire,size_t len);
size_t iso_expire(iso_rx *,uint64_t now);
/* Start new group numbering on a parameter switch. Completed SDUs retain
   their original delivery deadlines and continue to occupy queue capacity. */
size_t iso_rx_reconfigure(iso_rx *);
iso_result iso_deliver(iso_rx *,uint64_t now,uint8_t *out,size_t capacity,size_t *used);
#endif
