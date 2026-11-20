#include "cis_node.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERIFY(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL integrated line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

/* Each endpoint owns its radio, parent-link mailbox and upper-layer notices.
   Only timestamps and on-air bytes cross the two-node test boundary. */
typedef struct {
    cis_node node;
    sle_sch radio;
    uint8_t mailbox[CIS_CTRL_MAX_FRAME], header[4], payload[2047];
    size_t mail_len, payload_len;
    bool armed;
    cis_irq irq;
    bool irq_pending, reply_ready;
    uint8_t reply[4];
    cis_radio_event desc;
    unsigned notices[CIS_FIRST_SDU+1];
} terminal;

static bool mail(void *ctx, const uint8_t *p, size_t len)
{
    terminal *t = ctx;
    if (t->mail_len || len > sizeof(t->mailbox)) return false;
    memcpy(t->mailbox, p, len);
    t->mail_len = len;
    return true;
}
static bool publish_event(void *ctx, const cis_radio_event *d,
                          const uint8_t *header, const uint8_t *data, size_t size)
{
    terminal *t = ctx;
    VERIFY(!t->armed && size <= sizeof(t->payload));
    VERIFY((header != NULL) == d->first_tx);
    t->armed = true;
    t->desc = *d;
    t->payload_len = size;
    if (header) memcpy(t->header, header, sizeof(t->header));
    if (size) memcpy(t->payload, data, size);
    return true;
}
static void abort_event(void *ctx, uint64_t token)
{
    terminal *t = ctx;
    VERIFY(t->armed && token==t->desc.token);
    t->armed = false;
}
static void notice(void *ctx, cis_notice what)
{
    terminal *t = ctx;
    t->notices[what]++;
}
static bool peek_irq(void *ctx, cis_irq *item)
{
    terminal *t=ctx;
    if (!t->irq_pending) return false;
    *item=t->irq;
    return true;
}
static bool post_reply(void *ctx, uint64_t token, const uint8_t a3[4])
{
    terminal *t=ctx;
    VERIFY(t->armed && t->desc.token==token);
    memcpy(t->reply,a3,4);
    t->reply_ready=true;
    return true;
}
static void ack_irq(void *ctx, const cis_irq *item)
{
    terminal *t=ctx;
    VERIFY(t->irq_pending && t->irq.token==item->token);
    t->irq_pending=false;
}
static iso_result service_irq(terminal *t, cis_irq item)
{
    VERIFY(!t->irq_pending);
    t->irq=item;
    t->irq_pending=true;
    t->reply_ready=false;
    cis_irq_port port={t,peek_irq,post_reply,ack_irq};
    iso_result rc=cis_node_service(&t->node,&port);
    VERIFY(!t->irq_pending);
    return rc;
}
static void init_terminal(terminal *t, bool is_g)
{
    uint8_t map[10];
    memset(map, 0xff, sizeof(map));
    map[9]=0x0f;
    cis_parent parent = {.epoch_us=0, .slot=0, .period_slots=8, .slot_us=125};
    cis_node_port port = {t, mail, publish_event, abort_event, notice};
    VERIFY(cis_node_init(&t->node, port, is_g, parent, 8000, &t->radio, map, true));
}
static void exchange(terminal *src, terminal *dst, uint16_t expected, uint64_t now)
{
    uint16_t wire_type = (uint16_t)(src->mailbox[0] | ((uint16_t)src->mailbox[1]<<8));
    VERIFY(src->mail_len>=3 && wire_type==expected && src->mailbox[2]+3u==src->mail_len);
    VERIFY(cis_control_rx(&dst->node.control, src->mailbox, src->mail_len, now)==CIS_CTL_OK);
    printf("CTRL us=%lu type=0x%04x payload_bytes=%u\n", (unsigned long)now,
           (unsigned)wire_type, (unsigned)src->mailbox[2]);
    src->mail_len=0;
}
static cis_params profile(void)
{
    cis_params p = {.set_id=1, .group_id=1, .events=4, .slot_us=125,
        .group_slots=16, .event_slots=4, .intra_us=50, .inter_us=25,
        .anchor_delay_us=1800, .reference_delay_us=250};
    p.dir[0]=(cis_direction){.link_id=0x123456, .sdu_period_us=2000,
        .crc_seed=0x123abc, .max_sdu=8, .max_pdu=4, .max_offset_slots=2,
        .discard_cycles=1, .new_packets=2, .feedback=26};
    p.dir[1]=(cis_direction){.link_id=0x432176, .sdu_period_us=2000,
        .crc_seed=0x432cba, .max_sdu=4, .max_pdu=4, .max_offset_slots=1,
        .discard_cycles=1, .new_packets=1, .feedback=7};
    return p;
}
static void end_event(terminal *t)
{
    VERIFY(t->armed);
    t->armed=false;
    VERIFY(service_irq(t,(cis_irq){.kind=CIS_IRQ_END,.token=t->desc.token,
                                   .when_us=t->desc.end_us})==ISO_OK);
}
static void open_pair(terminal *g, terminal *t, uint64_t start)
{
    VERIFY(cis_node_schedule(&g->node,start,200,100)==RADIO_OK);
    VERIFY(cis_node_schedule(&t->node,start,200,100)==RADIO_OK);
    VERIFY(g->armed && t->armed && g->desc.first_tx && !t->desc.first_tx);
    VERIFY(g->desc.start_us==start && t->desc.start_us==start);
    VERIFY(g->desc.second_us==start+250 && t->desc.second_us==start+250);
    VERIFY(g->desc.channel==t->desc.channel && g->desc.link_id[0]==t->desc.link_id[0]);
    if (start>=6000) {
        const cis_radio_event *e=&g->desc;
        printf("CIS_LINK_EVENT token=%lu first_us=%lu second_us=%lu end_us=%lu slot=%u channel=%u first_tx=%u first_air_us=%u\n",
               (unsigned long)e->token,(unsigned long)e->start_us,(unsigned long)e->second_us,
               (unsigned long)e->end_us,e->slot,e->channel,(unsigned)e->first_tx,e->first_air_us);
    }
}

static void delivery_after_parameter_switch(void)
{
    iso_rx rx={0};
    iso_config profile={.linked=true,.max_sdu=2,.max_pdu=2,.group_period_us=1000,
        .sdu_period_us=1000,.anchor_delay_us=1500,.discard_cycles=1};
    uint8_t data[2]={0x45,0x12}, wire[16], out[2];
    size_t len=0;
    VERIFY(iso_encode(&profile,0,0,0,data,2,wire,sizeof(wire),&len)==ISO_OK);
    VERIFY(iso_ingest(&rx,&profile,0,1000,0,wire,len)==ISO_OK);
    VERIFY(iso_rx_reconfigure(&rx)==0);
    data[0]=0x99;
    VERIFY(iso_encode(&profile,0,0,0,data,2,wire,sizeof(wire),&len)==ISO_OK);
    VERIFY(iso_ingest(&rx,&profile,0,2000,0,wire,len)==ISO_OK);
    VERIFY(iso_deliver(&rx,2499,out,sizeof(out),&len)==ISO_WAIT);
    VERIFY(iso_deliver(&rx,2500,out,sizeof(out),&len)==ISO_OK && out[0]==0x45 && len==2);
    VERIFY(iso_deliver(&rx,3500,out,sizeof(out),&len)==ISO_OK && out[0]==0x99 && len==2);
    puts("PASS completed SDUs keep their original deadlines across a new configuration epoch");
}

void test_end_to_end(void)
{
    delivery_after_parameter_switch();
    terminal g={0}, t={0};
    init_terminal(&g,true); init_terminal(&t,false);
    cis_params cfg=profile();
    cfg.dir[1].aperiodic=true;
    VERIFY(cis_create(&g.node.control,&cfg,32,0)==CIS_CTL_INVALID && !g.mail_len);
    cfg.dir[1].aperiodic=false;
    VERIFY(cis_create(&g.node.control,&cfg,32,0)==CIS_CTL_OK);
    exchange(&g,&t,CIS_CREATE,1);
    VERIFY(t.mail_len==0 && g.node.control.state==CIS_ARMED);
    VERIFY(cis_node_tick(&g.node,3999)==CIS_CTL_OK && !g.node.radio.enabled);
    VERIFY(cis_node_tick(&t.node,3999)==CIS_CTL_OK && !t.node.radio.enabled);
    VERIFY(cis_node_tick(&g.node,4000)==CIS_CTL_OK && cis_node_tick(&t.node,4000)==CIS_CTL_OK);
    VERIFY(g.notices[CIS_LINK_UP]==1 && t.notices[CIS_LINK_UP]==1);

    uint8_t sdu[]={0,0x7f,0xff,0x19,0x5a,0,0x11,0xa5};
    VERIFY(cis_node_queue_at(&g.node,3749,sdu,sizeof(sdu))==ISO_EXPIRED);
    VERIFY(cis_node_queue_at(&g.node,3900,sdu,sizeof(sdu))==ISO_OK);
    uint8_t first_attempt[32];
    size_t first_length=0;
    for (unsigned i=0;i<3;i++) {
        uint64_t start=4000+500*i;
        open_pair(&g,&t,start);
        VERIFY(g.payload_len==11 && g.payload[0]==0xfe && g.payload[1]==0xff);
        if (i==0) {
            first_length=g.payload_len;
            memcpy(first_attempt,g.payload,first_length);
        } else if (i==1)
            VERIFY(first_length==g.payload_len && !memcmp(first_attempt,g.payload,first_length));
        cis_irq packet={.kind=CIS_IRQ_FIRST_RX,.token=t.desc.token,.when_us=start+200,
            .bytes=g.payload,.length=g.payload_len,.header_ok=true,.data_ok=i!=0};
        memcpy(packet.a3,g.header,4);
        VERIFY(service_irq(&t,packet)==(i==0?ISO_CRC:ISO_OK) && t.reply_ready);
        cis_irq feedback={.kind=CIS_IRQ_FEEDBACK_RX,.token=g.desc.token,.when_us=start+350,.header_ok=true};
        memcpy(feedback.a3,t.reply,4);
        VERIFY(service_irq(&g,feedback)==(i==0?ISO_RETRY:ISO_OK));
        printf("DATA first_us=%lu second_us=%lu channel=%u result=%s\n",
               (unsigned long)start,(unsigned long)g.desc.second_us,
               (unsigned)g.desc.channel,i==0?"NACK":"ACK");
        end_event(&t); end_event(&g);
    }
    /* T still listens at the last opportunity because the final ACK itself
       could have been lost. G has closed its group after receiving it. */
    VERIFY(cis_node_schedule(&t.node,5500,200,100)==RADIO_OK);
    end_event(&t);
    uint8_t delivered[8];
    size_t used=0;
    VERIFY(cis_node_deliver(&t.node,5799,delivered,sizeof(delivered),&used)==ISO_WAIT);
    VERIFY(cis_node_deliver(&t.node,5800,delivered,sizeof(delivered),&used)==ISO_OK);
    VERIFY(used==8 && !memcmp(sdu,delivered,used) && t.notices[CIS_FIRST_SDU]==1);
    VERIFY(cis_node_deliver(&t.node,5800,delivered,sizeof(delivered),&used)==ISO_WAIT);

    cis_params revised=cfg;
    revised.event_slots=3;
    revised.dir[0].link_id=0x654321;
    VERIFY(cis_propose(&g.node.control,3,&revised,5800)==CIS_CTL_OK);
    exchange(&g,&t,CIS_PARAM_REQ,5801);
    exchange(&t,&g,CIS_PARAM_RSP,5802);
    VERIFY(cis_update(&g.node.control,3,64,16,5803)==CIS_CTL_OK);
    exchange(&g,&t,CIS_UPDATE_IND,5804);
    open_pair(&g,&t,6000);
    VERIFY(cis_node_queue(&g.node,0,0,sdu,sizeof(sdu))==ISO_EXPIRED);
    VERIFY(g.desc.link_id[0]==cfg.dir[0].link_id);
    end_event(&g); end_event(&t);
    VERIFY(cis_node_tick(&g.node,9999)==CIS_CTL_OK && cis_node_tick(&t.node,9999)==CIS_CTL_OK);
    VERIFY(g.notices[CIS_LINK_UPDATED]==0 && t.notices[CIS_LINK_UPDATED]==0);
    open_pair(&g,&t,10000);
    VERIFY(g.desc.link_id[0]==revised.dir[0].link_id && t.notices[CIS_LINK_UPDATED]==1);
    end_event(&g); end_event(&t);
    open_pair(&g,&t,10375);
    VERIFY(g.desc.index==1 && t.desc.index==1);

    VERIFY(cis_disconnect(&t.node.control,17,10376)==CIS_CTL_OK);
    exchange(&t,&g,CIS_DISCONNECT,10377);
    VERIFY(cis_disconnect_ack(&t.node.control)==CIS_CTL_OK);
    VERIFY(!g.armed && !t.armed && !g.node.reserved && !t.node.reserved);
    VERIFY(g.node.control.state==CIS_OFF && t.node.control.state==CIS_OFF);
    VERIFY(cis_radio_finish(&t.node.radio,t.desc.token)==RADIO_STALE);
    VERIFY(g.notices[CIS_LINK_DOWN]==1 && t.notices[CIS_LINK_DOWN]==1);
    puts("PASS G/T integrated: create 4000us, 2 fragments/1 retry, SDU 5800us, atomic update 10000us, T disconnect and drain");
}
