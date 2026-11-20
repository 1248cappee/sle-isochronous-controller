#ifndef SLE_CIS_CONTROL_H
#define SLE_CIS_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CIS_SLOT_MASK UINT32_C(0x3fffffff)
#define CIS_CTRL_MAX_FRAME 59u
enum {
    CIS_REJECT = 0x0003, CIS_DISCONNECT = 0x001e, CIS_CREATE = 0x0022,
    CIS_PARAM_REQ = 0x0023, CIS_PARAM_RSP = 0x0024, CIS_UPDATE_REQ = 0x0025,
    CIS_UPDATE_IND = 0x0026
};
typedef struct {
    uint32_t link_id, sdu_period_us, crc_seed;
    uint16_t max_sdu, max_pdu, max_offset_slots, discard_cycles;
    uint8_t frame, bandwidth, pilot, new_packets, feedback;
    bool crc32, aperiodic;
} cis_direction;
typedef struct {
    uint8_t set_id, group_id, events, slot_us;
    uint16_t group_slots, event_slots, intra_us, inter_us;
    uint32_t anchor_delay_us, reference_delay_us;
    bool t_sends_first;
    cis_direction dir[2]; /* first and second transmission, not local TX/RX */
} cis_params;
typedef struct {
    uint16_t type, rejected_type, offset_slots;
    uint32_t slot, link_id;
    uint8_t tag, reason;
    cis_params params;
} cis_signal;
/* Exactly one complete section 7.3.2.1 element per call, including TL header.
   0x23/24/25/26 carry no slot duration; supply the active duration at runtime. */
size_t cis_wire_encode(const cis_signal *, uint8_t *, size_t);
bool cis_wire_decode(const uint8_t *, size_t, cis_signal *);

/* All operations serialized with monotonic times and callbacks non-reentrant.
   send copies bytes synchronously into an ordered reliable parent-link TX
   queue; the parent removes link-layer duplicates and reports parent loss.
   acquire reserves link
   context; apply is a non-failing commit after admission; release must drain
   tasks, buffers and in-flight radio operations. admit is side-effect free. */
typedef struct {
    void *ctx;
    bool (*send)(void *, const uint8_t *, size_t);
    bool (*admit)(void *, const cis_params *);
    bool (*acquire)(void *, const cis_params *);
    void (*apply)(void *, const cis_params *, uint64_t, uint32_t);
    void (*release)(void *);
} cis_port;
/* epoch_us is a timestamp of a parent event group start; slot is its
   30-bit parent scheduling slot. period_slots is the parent group period. */
typedef struct {
    uint64_t epoch_us;
    uint32_t slot;
    uint16_t period_slots;
    uint8_t slot_us;
} cis_parent;
typedef enum { CIS_OFF, CIS_ARMED, CIS_ACTIVE, CIS_CLOSING } cis_state;
typedef enum { CIS_CTL_OK, CIS_CTL_INVALID, CIS_CTL_BUSY, CIS_CTL_IO,
               CIS_CTL_REJECTED, CIS_CTL_TIMEOUT, CIS_CTL_STALE } cis_result;
typedef struct {
    cis_state state;
    cis_port port;
    cis_parent parent;
    bool parent_up, is_g, held, update_pending;
    uint8_t tag, reason;
    uint16_t waiting, known_tags;
    uint32_t timeout_us, anchor_slot, pending_slot;
    uint64_t anchor_us, effective_us, last_old_group_us, deadline;
    cis_params active, candidates[16], pending;
} cis_control;
bool cis_control_init(cis_control *, cis_port, bool is_g, cis_parent, uint32_t timeout_us);
cis_result cis_create(cis_control *, const cis_params *, uint32_t parent_effective_slot, uint64_t now);
cis_result cis_control_rx(cis_control *, const uint8_t *, size_t, uint64_t now);
cis_result cis_control_tick(cis_control *, uint64_t now);
/* Check before scheduling any event in a group. Call tick at the effective
   instant before scheduling a group under the new parameters. */
bool cis_group_permitted(const cis_control *, uint64_t group_start_us);
cis_result cis_propose(cis_control *, uint8_t tag, const cis_params *, uint64_t now);
cis_result cis_request_update(cis_control *, uint8_t tag, uint64_t now);
cis_result cis_update(cis_control *, uint8_t tag, uint32_t last_group_slot,
                      uint16_t offset_slots, uint64_t now);
cis_result cis_disconnect(cis_control *, uint8_t reason, uint64_t now);
/* Deliver parent-link link-layer ACK for the specific outgoing disconnect.
   Parent adapter must correlate its own generation before calling. */
cis_result cis_disconnect_ack(cis_control *);
void cis_parent_down(cis_control *);
#endif
