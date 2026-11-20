#ifndef SLE_CIS_NODE_H
#define SLE_CIS_NODE_H

#include "cis_control.h"
#include "cis_data.h"
#include "cis_radio.h"

typedef enum {
    CIS_LINK_UP, CIS_LINK_UPDATED, CIS_LINK_DOWN, CIS_DATA_DROPPED,
    CIS_FIRST_SDU
} cis_notice;

typedef struct {
    void *ctx;
    /* Parent asynchronous link: copy or enqueue the entire control element. */
    bool (*send_control)(void *, const uint8_t *, size_t);
    /* Stage and publish one pair of windows atomically. Copy TX bytes and
       descriptor before returning. A receiving first phase has a NULL header. */
    bool (*publish)(void *, const cis_radio_event *, const uint8_t *a3,
                    const uint8_t *pdu, size_t length);
    void (*drain)(void *, uint64_t token);
    void (*notice)(void *, cis_notice);
} cis_node_port;

typedef struct {
    cis_control control;
    cis_radio radio;
    cis_tx tx;
    iso_rx rx;
    iso_config format;
    cis_node_port port;
    bool multiplexed, delivered, reserved, group_closed, first_more, sent_empty;
    bool queued_before;
    uint8_t last_sdu_ordinal;
    uint64_t closed_cycle, last_sdu_group;
    uint8_t queued_wire[2047];
} cis_node;

/* Caller owns an instance for its entire parent-link lifetime. Initialize once,
   before receiving any interrupts. Serialize all operations in one task.
   Use cis_create/propose/update/disconnect etc. on node.control. */
bool cis_node_init(cis_node *, cis_node_port, bool is_g, cis_parent,
                   uint32_t timeout_us, sle_sch *, const uint8_t map[10], bool multiplexed);
/* Queue a copied periodic SDU for its original group/ordinal from host timing.
   One outstanding TX SDU per node. ISO_BUSY permits host-side backpressure. */
iso_result cis_node_queue(cis_node *, uint64_t group, uint8_t ordinal,
                          const uint8_t *sdu, size_t length);
/* Map a captured SDU timestamp to its periodic group and SDU ordinal.
   At an exact source-window boundary the sample belongs to the new window. */
iso_result cis_node_queue_at(cis_node *, uint64_t sample_us,
                             const uint8_t *sdu, size_t length);
cis_radio_result cis_node_schedule(cis_node *, uint64_t now, uint32_t first_air_us,
                                   uint32_t second_air_us);
cis_result cis_node_tick(cis_node *, uint64_t now);
/* IRQ timestamps are local capture times. ACK only when reply_ready is true.
   The PHY emits the four-byte A3 container's low 28 bits plus its own CRC12. */
iso_result cis_node_packet(cis_node *, uint64_t token, uint64_t now,
                           const uint8_t a3[4], const uint8_t *bytes, size_t length,
                           bool header_ok, bool data_ok, uint8_t reply[4], bool *reply_ready);
iso_result cis_node_feedback(cis_node *, uint64_t token, uint64_t now,
                             const uint8_t reply[4], bool header_ok);
iso_result cis_node_deliver(cis_node *, uint64_t now, uint8_t *out, size_t cap, size_t *used);

typedef enum { CIS_IRQ_FIRST_RX, CIS_IRQ_FEEDBACK_RX, CIS_IRQ_END } cis_irq_kind;
typedef struct {
    cis_irq_kind kind;
    uint64_t token, when_us;
    bool header_ok, data_ok;
    uint8_t a3[4];
    const uint8_t *bytes;
    size_t length;
} cis_irq;
/* peek acquires a stable queue entry and RX buffer until ack. reply copies
   the feedback header before the second window. Each dequeued entry is
   acknowledged, including obsolete tokens. Drain all pending IRQs before tick. */
typedef struct {
    void *ctx;
    bool (*peek)(void *, cis_irq *);
    bool (*reply)(void *, uint64_t token, const uint8_t a3[4]);
    void (*ack)(void *, const cis_irq *);
} cis_irq_port;
iso_result cis_node_service(cis_node *, const cis_irq_port *);
#endif
