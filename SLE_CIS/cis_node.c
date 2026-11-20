#include "cis_node.h"
#include <string.h>

static iso_config stream_format(const cis_node *n, const cis_params *p)
{
    const cis_direction *d = &p->dir[0];
    return (iso_config){.linked=true, .multiplexed=n->multiplexed,
        .max_sdu=d->max_sdu, .max_pdu=d->max_pdu, .sdu_period_us=d->sdu_period_us,
        .group_period_us=(uint32_t)p->group_slots*p->slot_us,
        .anchor_delay_us=p->anchor_delay_us, .discard_cycles=(uint8_t)d->discard_cycles};
}

static bool admit(void *ctx, const cis_params *p)
{
    const cis_node *n = ctx;
    iso_config format = stream_format(n, p);
    return !p->t_sends_first && !p->dir[0].aperiodic && !p->dir[1].aperiodic &&
           p->dir[0].discard_cycles <= UINT8_MAX &&
           p->dir[0].new_packets <= p->events &&
           iso_config_valid(&format) && iso_new_packets(&format) == p->dir[0].new_packets;
}

static bool acquire(void *ctx, const cis_params *p)
{
    cis_node *n = ctx;
    if (n->reserved || !admit(n, p)) return false;
    n->reserved = true;
    return true;
}

static void clear_stream(cis_node *n, bool keep_completed)
{
    bool dropped = n->tx.active;
    if (keep_completed) {
        if (iso_rx_reconfigure(&n->rx)) dropped=true;
    } else {
        for (size_t i=0; i<ISO_RX_SLOTS; ++i) dropped |= n->rx.pending[i].used;
        memset(&n->rx, 0, sizeof(n->rx));
    }
    memset(&n->tx, 0, sizeof(n->tx));
    n->group_closed = false;
    n->queued_before = false;
    if (dropped) n->port.notice(n->port.ctx, CIS_DATA_DROPPED);
}

static void activate(void *ctx, const cis_params *p, uint64_t anchor, uint32_t slot)
{
    cis_node *n = ctx;
    bool update = n->radio.enabled;
    if (!cis_radio_apply(&n->radio, p, anchor, slot)) return;
    clear_stream(n, update);
    n->format = stream_format(n, p);
    n->port.notice(n->port.ctx, update ? CIS_LINK_UPDATED : CIS_LINK_UP);
}

static void release(void *ctx)
{
    cis_node *n = ctx;
    cis_radio_stop(&n->radio);
    clear_stream(n, false);
    n->reserved = false;
    n->delivered = false;
    n->port.notice(n->port.ctx, CIS_LINK_DOWN);
}

static bool send_control(void *ctx, const uint8_t *bytes, size_t len)
{
    cis_node *n = ctx;
    return n->port.send_control(n->port.ctx, bytes, len);
}

static bool publish(void *ctx, const cis_radio_event *event)
{
    cis_node *n = ctx;
    uint64_t group_start=event->start_us-(uint64_t)event->index*
                         n->radio.cfg.event_slots*n->radio.cfg.slot_us;
    if (!cis_group_permitted(&n->control,group_start) ||
        (n->control.update_pending && event->end_us > n->control.effective_us))
        return false;
    if (!event->first_tx)
        return n->port.publish(n->port.ctx, event, NULL, NULL, 0);

    size_t len = 0;
    uint8_t header[4];
    iso_result rc = ISO_WAIT;
    if (n->tx.active && event->cycle >= n->tx.origin_group)
        rc = cis_tx_pdu(&n->tx, event->cycle, header, n->queued_wire, sizeof(n->queued_wire), &len);
    if (rc == ISO_EXPIRED) n->port.notice(n->port.ctx, CIS_DATA_DROPPED);
    if (rc != ISO_OK) {
        if (rc != ISO_WAIT && rc != ISO_EXPIRED) return false;
        cis_a3 empty = {.empty=true};
        if (!cis_a3_encode(&empty, header)) return false;
    }
    cis_a3 a3;
    if (!cis_a3_decode(header, &a3)) return false;
    n->first_more = a3.more;
    n->sent_empty = a3.empty;
    bool ok = n->port.publish(n->port.ctx, event, header, n->queued_wire, len);
    if (!ok) n->tx.submitted = false;
    return ok;
}

static void drain(void *ctx, uint64_t token)
{
    cis_node *n = ctx;
    n->port.drain(n->port.ctx, token);
}

bool cis_node_init(cis_node *n, cis_node_port port, bool is_g, cis_parent parent,
                   uint32_t timeout, sle_sch *sch, const uint8_t map[10], bool mux)
{
    if (!n || !port.send_control || !port.publish || !port.drain || !port.notice) return false;
    memset(n, 0, sizeof(*n));
    n->port = port;
    n->multiplexed = mux;
    cis_radio_port radio_port = {n, publish, drain};
    cis_port control_port = {n, send_control, admit, acquire, activate, release};
    return cis_radio_init(&n->radio, sch, radio_port, map, is_g) &&
           cis_control_init(&n->control, control_port, is_g, parent, timeout);
}

iso_result cis_node_queue(cis_node *n, uint64_t group, uint8_t ordinal,
                          const uint8_t *sdu, size_t length)
{
    if (n->control.state != CIS_ACTIVE || !n->control.is_g) return ISO_INVALID;
    if (n->group_closed && group <= n->closed_cycle) return ISO_EXPIRED;
    if (n->queued_before && (group < n->last_sdu_group ||
        (group == n->last_sdu_group && ordinal <= n->last_sdu_ordinal))) return ISO_INVALID;
    iso_result rc = cis_tx_load(&n->tx, &n->format, group, ordinal, sdu, length);
    if (rc == ISO_OK) {
        n->queued_before = true;
        n->last_sdu_group = group;
        n->last_sdu_ordinal = ordinal;
    }
    return rc;
}

iso_result cis_node_queue_at(cis_node *n, uint64_t sample_us,
                             const uint8_t *sdu, size_t length)
{
    if (n->control.state != CIS_ACTIVE) return ISO_INVALID;
    uint32_t period=n->format.group_period_us, sdu_period=n->format.sdu_period_us;
    uint64_t lead=(uint64_t)(period-sdu_period)+n->control.active.reference_delay_us;
    uint64_t anchor=n->control.anchor_us;
    uint64_t position;
    if (sample_us < anchor) {
        if (anchor-sample_us > lead) return ISO_EXPIRED;
        position=lead-(anchor-sample_us);
    } else {
        if (UINT64_MAX-(sample_us-anchor)<lead) return ISO_INVALID;
        position=sample_us-anchor+lead;
    }
    return cis_node_queue(n,position/period,(uint8_t)((position%period)/sdu_period),sdu,length);
}

cis_radio_result cis_node_schedule(cis_node *n, uint64_t now, uint32_t first_us,
                                   uint32_t second_us)
{
    cis_result rc = cis_control_tick(&n->control, now);
    if (rc != CIS_CTL_OK || n->control.state != CIS_ACTIVE) return RADIO_IDLE;
    if (n->group_closed && !n->radio.pending) {
        uint64_t cycle = n->radio.next / n->radio.cfg.events;
        if (cycle == n->closed_cycle)
            n->radio.next += n->radio.cfg.events - n->radio.next % n->radio.cfg.events;
    }
    return cis_radio_schedule(&n->radio, now, first_us, second_us);
}

cis_result cis_node_tick(cis_node *n, uint64_t now)
{
    cis_result rc = cis_control_tick(&n->control, now);
    cis_radio_result radio = cis_radio_tick(&n->radio, now);
    if (iso_expire(&n->rx, now)) n->port.notice(n->port.ctx, CIS_DATA_DROPPED);
    if (rc==CIS_CTL_OK && radio==RADIO_TIMEOUT) return CIS_CTL_TIMEOUT;
    return rc;
}

iso_result cis_node_packet(cis_node *n, uint64_t token, uint64_t now,
                           const uint8_t header[4], const uint8_t *bytes, size_t len,
                           bool header_ok, bool data_ok, uint8_t reply[4], bool *ready)
{
    if (!ready) return ISO_INVALID;
    *ready = false;
    if (!reply || !header) return ISO_INVALID;
    const cis_radio_event *e = &n->radio.active;
    if (n->control.state != CIS_ACTIVE || !n->radio.pending || e->first_tx ||
        e->token!=token || now<e->start_us || now>=e->second_us) return ISO_INVALID;
    if (!header_ok) return ISO_CRC;
    cis_a3 decoded;
    if (!cis_a3_decode(header, &decoded)) return ISO_INVALID;
    if (decoded.empty) {
        if (len || decoded.length || decoded.type) return ISO_INVALID;
        cis_a3 empty_ack = {.empty=true, .receive_seq=(uint8_t)((decoded.send_seq+1u)&31u)};
        *ready = cis_a3_encode(&empty_ack, reply);
        return *ready ? ISO_OK : ISO_INVALID;
    }
    uint64_t anchor = e->start_us - (uint64_t)e->index*n->radio.cfg.event_slots*n->radio.cfg.slot_us;
    iso_result rc = cis_rx_pdu(&n->rx, &n->format, e->cycle, anchor, header,
                               bytes, len, true, data_ok, reply);
    *ready = rc==ISO_OK || rc==ISO_DUPLICATE || rc==ISO_CRC ||
             rc==ISO_BUSY || rc==ISO_GAP;
    return rc;
}

iso_result cis_node_feedback(cis_node *n, uint64_t token, uint64_t now,
                             const uint8_t reply[4], bool header_ok)
{
    const cis_radio_event *e = &n->radio.active;
    if (!reply || n->control.state != CIS_ACTIVE || !n->radio.pending || !e->first_tx ||
        token!=e->token || now<e->second_us || now>e->end_us) return ISO_INVALID;
    cis_a3 ack;
    if (!header_ok) return ISO_CRC;
    if (!cis_a3_decode(reply, &ack) || !ack.empty || ack.length || ack.type) return ISO_INVALID;
    iso_result rc;
    if (n->sent_empty) rc = ack.receive_seq == 1u ? ISO_OK : ISO_RETRY;
    else rc = cis_tx_feedback(&n->tx, e->cycle, reply, true);
    if (rc == ISO_OK && !n->first_more && !ack.more) {
        n->group_closed = true;
        n->closed_cycle = e->cycle;
    }
    return rc;
}

iso_result cis_node_deliver(cis_node *n, uint64_t now, uint8_t *out, size_t cap, size_t *used)
{
    if (n->control.state != CIS_ACTIVE) return ISO_INVALID;
    iso_result rc = iso_deliver(&n->rx, now, out, cap, used);
    if (rc == ISO_OK && !n->delivered) {
        n->delivered = true;
        n->port.notice(n->port.ctx, CIS_FIRST_SDU);
    }
    return rc;
}

iso_result cis_node_service(cis_node *n, const cis_irq_port *port)
{
    if (!n || !port || !port->peek || !port->reply || !port->ack) return ISO_INVALID;
    cis_irq item;
    if (!port->peek(port->ctx, &item)) return ISO_WAIT;
    iso_result rc=ISO_INVALID;
    if (item.kind==CIS_IRQ_FIRST_RX) {
        uint8_t feedback[4];
        bool ready;
        rc=cis_node_packet(n,item.token,item.when_us,item.a3,item.bytes,item.length,
                           item.header_ok,item.data_ok,feedback,&ready);
        if (ready && !port->reply(port->ctx,item.token,feedback)) rc=ISO_BUSY;
    } else if (item.kind==CIS_IRQ_FEEDBACK_RX) {
        rc=cis_node_feedback(n,item.token,item.when_us,item.a3,item.header_ok);
    } else if (item.kind==CIS_IRQ_END) {
        rc=cis_radio_finish(&n->radio,item.token)==RADIO_OK ? ISO_OK : ISO_INVALID;
    }
    port->ack(port->ctx,&item);
    return rc;
}
