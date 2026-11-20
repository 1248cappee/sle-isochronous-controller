#ifndef CIS_RADIO_H
#define CIS_RADIO_H

#include "cis_control.h"
#include "../SLE_BIS/sle_sch.h"

/* Same frequency for both phases of an event; descriptor times are monotonic us.
   Physical channels use the 2.4 GHz, 1 MHz mapping (0..75, Table 45).
   Copies are semantic descriptors, never a target EM register layout. */
typedef struct {
    uint64_t token, start_us, second_us, end_us, cycle;
    uint32_t slot, first_air_us, second_air_us;
    uint32_t link_id[2], crc_seed[2];
    bool crc32[2], first_tx;
    uint8_t index, channel;
} cis_radio_event;

typedef struct {
    void *ctx;
    /* Stage and publish atomically. On false nothing has been published.
       The BSP copies the descriptor before returning. */
    bool (*submit)(void *, const cis_radio_event *);
    /* Returns only after DMA and BOTH phases have quiesced. */
    void (*cancel_and_drain)(void *, uint64_t token);
} cis_radio_port;

typedef enum {
    RADIO_OK, RADIO_INVALID, RADIO_IDLE, RADIO_BUSY, RADIO_MISSED,
    RADIO_CONFLICT, RADIO_IO, RADIO_STALE, RADIO_TIMEOUT
} cis_radio_result;

typedef struct {
    sle_sch *sch;
    cis_radio_port port;
    cis_params cfg;
    cis_radio_event active;
    uint64_t anchor_us, next, token_sequence;
    uint32_t anchor_slot;
    uint8_t channel_map[10];
    bool is_g, enabled, pending;
} cis_radio;

/* Initialize once per controller instance, after its old physical queue drains. */
bool cis_radio_init(cis_radio *, sle_sch *, cis_radio_port,
                    const uint8_t channel_map[10], bool is_g);
/* Apply at the activation instant. Successful reconfiguration aborts the old
   pending descriptor; an invalid config leaves the existing one untouched.
   anchor_slot is the G scheduler slot at anchor_us, zero after a non125 reset. */
bool cis_radio_apply(cis_radio *, const cis_params *, uint64_t anchor_us, uint32_t anchor_slot);
/* Occupied interval is [first_start, second_end). Airtimes are calibrated PHY
   values for the supplied PDUs, not maximum PDU lengths or slot counts. */
cis_radio_result cis_radio_schedule(cis_radio *, uint64_t now_us, uint32_t first_air_us,
                                    uint32_t second_air_us);
/* Submit END only for actual end of both phases. Drain queued IRQs before tick. */
cis_radio_result cis_radio_finish(cis_radio *, uint64_t token);
cis_radio_result cis_radio_tick(cis_radio *, uint64_t now_us);
void cis_radio_stop(cis_radio *);
#endif
