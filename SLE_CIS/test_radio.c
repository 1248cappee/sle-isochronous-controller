#include "cis_radio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERIFY(x) do { if (!(x)) { fprintf(stderr, "radio line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    bool fail, busy;
    unsigned drains;
    cis_radio_event e;
} queue;

static bool post(void *ctx, const cis_radio_event *event)
{
    queue *q = ctx;
    VERIFY(!q->busy);
    if (q->fail) return false;
    q->e = *event;
    q->busy = true;
    return true;
}
static void drain(void *ctx, uint64_t token)
{
    queue *q = ctx;
    VERIFY(q->busy && token == q->e.token);
    q->busy = false;
    ++q->drains;
}
static void complete(cis_radio *r, queue *q)
{
    VERIFY(q->busy);
    q->busy = false;
    VERIFY(cis_radio_finish(r, q->e.token) == RADIO_OK);
}
static void capture(cis_radio *r, queue *q, unsigned start, unsigned slot,
                    unsigned channel, bool tx)
{
    VERIFY(cis_radio_schedule(r, start-10, 100, 120) == RADIO_OK);
    const cis_radio_event *e = &q->e;
    VERIFY(e->start_us == start && e->second_us == start+125 && e->end_us == start+245);
    VERIFY(e->slot == slot && e->channel == channel && e->first_tx == tx);
    VERIFY(e->link_id[0] == 0x123456 && e->link_id[1] == 0x987654);
    VERIFY(e->crc_seed[0] == 0x345678 && e->crc_seed[1] == 0x98765432);
    printf("CIS_EVENT token=%lu first_us=%lu second_us=%lu end_us=%lu slot=%u channel=%u first_tx=%u first_air_us=%u\n",
           (unsigned long)e->token, (unsigned long)e->start_us,
           (unsigned long)e->second_us, (unsigned long)e->end_us,
           (unsigned)e->slot, (unsigned)e->channel, (unsigned)e->first_tx,
           (unsigned)e->first_air_us);
    complete(r, q);
}

typedef struct {
    sle_sch *sch;
    uint64_t submitted;
} broadcast_queue;
static bool b_reserve(void *ctx, const bis_event *e)
{
    broadcast_queue *q = ctx;
    return sle_reserve(q->sch, q, e->token, e->time_us, e->duration_us);
}
static void b_release(void *ctx, uint64_t token)
{
    broadcast_queue *q = ctx;
    sle_release(q->sch, q, token);
}
static bool b_post(void *ctx, const bis_event *e, const bis_a6 *header, const uint8_t *bytes)
{
    broadcast_queue *q = ctx;
    VERIFY(e->tx && header && bytes && header->length == 2);
    q->submitted = e->token;
    return true;
}
static void b_cancel(void *ctx, uint64_t token)
{
    broadcast_queue *q = ctx;
    VERIFY(q->submitted == token);
    q->submitted = 0;
}
static bool b_pdu(void *ctx, const bis_event *e, const bis_a6 *h, const uint8_t *p)
{
    (void)ctx; (void)e; (void)h; (void)p;
    return false; /* TX-only fixture */
}
static bool b_management(void *ctx, const uint8_t *p, size_t n, uint64_t timestamp)
{
    (void)ctx; (void)p; (void)n; (void)timestamp;
    return false;
}
static void mixed_links(cis_radio *r, queue *q, const cis_params *cfg)
{
    broadcast_queue bq = {.sch = r->sch};
    bis_port bp = {&bq, b_reserve, b_release, b_post, b_cancel, b_pdu, b_management};
    bis_controller broadcaster;
    bis_config b = {.group_slots=8, .event_slots=4, .events=2, .max_sdu=2, .max_pdu=2,
                    .sdu_period_us=1000, .new_packets=1, .link_base=0x654321};
    memset(b.map, 255, sizeof(b.map));
    VERIFY(bis_init(&broadcaster, bp, 100, 3));
    VERIFY(bis_create(&broadcaster, &b, 1000)==BIS_OK);
    VERIFY(cis_radio_apply(r, cfg, 1000, 0));
    const bis_a6 data = {.group=1, .length=2};
    const uint8_t payload[]={0x31, 0x32};

    VERIFY(bis_schedule(&broadcaster, 900, &data, payload)==BIS_OK);
    VERIFY(cis_radio_schedule(r, 900, 100, 120)==RADIO_CONFLICT);
    VERIFY(bis_done(&broadcaster,bq.submitted)==BIS_OK);

    VERIFY(cis_radio_schedule(r, 1450, 100, 120)==RADIO_OK && q->e.start_us==1500);
    VERIFY(bis_schedule(&broadcaster, 1450, &data, payload)==BIS_CONFLICT);
    complete(r, q);
    VERIFY(bis_schedule(&broadcaster, 1900, &data, payload)==BIS_OK);
    VERIFY(broadcaster.active.time_us==2000);
    bis_stop(&broadcaster);
    VERIFY(!bq.submitted);
    cis_radio_stop(r);
    for (size_t i=0; i<SLE_SCH_CAPACITY; ++i) VERIFY(!r->sch->windows[i].owner);
    puts("PASS real BIS/CIS on the same radio: B wins, A wins, both release");
}

void test_radio(void)
{
    cis_radio r;
    queue q = {0};
    sle_sch sch = {0};
    uint8_t map[10] = {0};
    cis_radio_port port = { &q, post, drain };
    VERIFY(!cis_radio_init(&r, &sch, port, map, true));
    memset(map, 255, sizeof(map));
    VERIFY(cis_radio_init(&r, &sch, port, map, true));
    cis_params c = { .events=2, .slot_us=125, .group_slots=16, .event_slots=4,
                     .intra_us=25, .inter_us=10 };
    c.dir[0].link_id=0x123456; c.dir[1].link_id=0x987654;
    c.dir[0].max_offset_slots=2; c.dir[1].max_offset_slots=2;
    c.dir[0].crc_seed=0x12345678; c.dir[1].crc32=true; c.dir[1].crc_seed=0x98765432;
    VERIFY(cis_radio_apply(&r, &c, 1000, CIS_SLOT_MASK-3u));
    capture(&r, &q, 1000, CIS_SLOT_MASK-3u, 53, true);
    capture(&r, &q, 1500, 0, 41, true);

    c.slot_us=50; c.group_slots=24; c.event_slots=8; c.t_sends_first=true;
    VERIFY(cis_radio_apply(&r, &c, 3000, 0));
    capture(&r, &q, 3000, 0, 41, false);
    capture(&r, &q, 3400, 8, 32, false);

    // Reserve a competing activity in the TURNAROUND GAP of cycle 1. Since the
    // same radio must switch direction, both phases plus gap are indivisible.
    int bis_owner;
    VERIFY(sle_reserve(&sch, &bis_owner, 5, 4310, 5));
    VERIFY(cis_radio_schedule(&r, 4000, 100, 120) == RADIO_CONFLICT);
    sle_release(&sch, &bis_owner, 5);
    VERIFY(cis_radio_schedule(&r, 4001, 100, 120) == RADIO_OK && q.e.start_us==4600);
    VERIFY(cis_radio_finish(&r, q.e.token-1) == RADIO_STALE && q.busy);
    VERIFY(cis_radio_tick(&r, 4844) == RADIO_OK);
    VERIFY(cis_radio_tick(&r, 4845) == RADIO_TIMEOUT && !q.busy && q.drains==1);
    uint64_t stale=q.e.token;
    VERIFY(sle_reserve(&sch, &bis_owner, stale, 4600, 245));
    sle_release(&sch, &bis_owner, stale);
    q.fail=true;
    VERIFY(cis_radio_schedule(&r, 5000, 100, 120) == RADIO_IO);
    VERIFY(sle_reserve(&sch, &bis_owner, 91, 5400, 245));
    sle_release(&sch, &bis_owner, 91);
    q.fail=false;
    VERIFY(cis_radio_schedule(&r, 5801, 100, 120) == RADIO_MISSED);
    VERIFY(cis_radio_schedule(&r, 5801, 100, 120) == RADIO_OK && q.e.start_us==6600);
    cis_radio_stop(&r);
    VERIFY(!q.busy && q.drains==2 && cis_radio_finish(&r, stale)==RADIO_STALE);
    // Re-map a disabled primary channel onto a sparse, ordered map.
    memset(map, 0, sizeof(map)); map[0]=2; map[9]=8;
    VERIFY(cis_radio_init(&r, &sch, port, map, false));
    VERIFY(cis_radio_apply(&r, &c, 10000, 0));
    VERIFY(cis_radio_schedule(&r, 10000, 100, 120)==RADIO_OK);
    VERIFY(q.e.channel==75 && q.e.first_tx);
    complete(&r, &q);
    VERIFY(cis_radio_schedule(&r, 10001, 251, 120)==RADIO_INVALID);
    cis_radio_stop(&r);
    c.slot_us=125; c.group_slots=16; c.event_slots=4;
    mixed_links(&r,&q,&c);
    puts("PASS CIS paired event scheduling, wrap, SCH conflict, IRQ recovery, remapping");
}
