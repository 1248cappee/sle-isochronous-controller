#include "cis_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"control FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)

typedef struct {
    cis_control control;
    uint8_t outgoing[CIS_CTRL_MAX_FRAME];
    size_t len;
    unsigned acquisitions, releases, commits;
    bool deny, fail_send;
} endpoint;
static bool tx(void *user,const uint8_t *data,size_t n)
{
    endpoint *e=user;
    if (e->fail_send) return false;
    CHECK(n<=sizeof(e->outgoing));
    memcpy(e->outgoing,data,n); e->len=n;
    return true;
}
static bool admissible(void *ctx,const cis_params *p)
{
    (void)ctx;
    return !p->dir[0].aperiodic && !p->dir[1].aperiodic;
}
static bool acquire(void *ctx,const cis_params *p)
{
    endpoint *e=ctx; (void)p;
    if (e->deny) return false;
    CHECK(e->acquisitions==e->releases);
    e->acquisitions++;
    return true;
}
static void release_link(void *ctx)
{
    endpoint *e=ctx;
    CHECK(e->acquisitions==e->releases+1); e->releases++;
}
static void apply(void *ctx,const cis_params *p,uint64_t anchor,uint32_t slot)
{
    endpoint *e=ctx;
    CHECK(e->control.state==CIS_ACTIVE && p==&e->control.active);
    CHECK(anchor==e->control.anchor_us && slot==e->control.anchor_slot);
    e->commits++;
}
static void init(endpoint *e,bool is_g)
{
    memset(e,0,sizeof(*e));
    cis_port port={e,tx,admissible,acquire,apply,release_link};
    CHECK(cis_control_init(&e->control,port,is_g,(cis_parent){1000,CIS_SLOT_MASK-15,4,125},2500));
}
static cis_result relay(endpoint *from,endpoint *to,uint64_t now)
{
    CHECK(from->len!=0);
    size_t n=from->len; from->len=0;
    return cis_control_rx(&to->control,from->outgoing,n,now);
}
static cis_params sample(void)
{
    cis_params p={.set_id=7,.group_id=3,.slot_us=125,.events=2,.group_slots=8,
                  .event_slots=4,.intra_us=25,.inter_us=50,.anchor_delay_us=750,.reference_delay_us=500};
    p.dir[0]=(cis_direction){.link_id=0x010203,.max_offset_slots=1,.crc_seed=0x123456,
        .pilot=1,.max_sdu=8,.sdu_period_us=1000,.max_pdu=4,.new_packets=2,.discard_cycles=3,.feedback=26};
    p.dir[1]=p.dir[0];
    p.dir[1].link_id=0x040506; p.dir[1].pilot=2; p.dir[1].crc_seed=0x89abcdef;
    p.dir[1].crc32=true; p.dir[1].discard_cycles=2; p.dir[1].feedback=7;
    return p;
}
static size_t from_hex(const char *s,uint8_t *out)
{
    size_t n=0;
    while (*s) {
        char *end=NULL;
        unsigned long value=strtoul(s,&end,16);
        CHECK(end!=s && value<=255 && n<CIS_CTRL_MAX_FRAME);
        out[n++]=(uint8_t)value;
        s=end;
    }
    return n;
}
static void wire_golden(void)
{
    /* Independently packed field-order fixtures including the 3-byte wrapper. */
    const char *gold[]={
        "22 00 38 07 03 10 00 00 00 08 00 04 00 19 00 32 00 02 ee 02 00 f4 01 00 24 03 02 01 06 05 04 00 90 08 80 00 e8 03 80 3e 00 04 20 40 80 00 22 56 34 12 00 ef cd ab 89 03 02 6a 07",
        "23 00 34 07 03 08 00 04 00 19 00 32 00 02 ee 02 00 f4 01 00 25 03 02 01 06 05 04 00 90 08 80 00 e8 03 80 3e 00 04 20 40 80 00 22 56 34 12 00 ef cd ab 89 02 01 6a 07",
        "24 00 34 07 03 08 00 04 00 19 00 32 00 02 ee 02 00 f4 01 00 05 03 02 01 06 05 04 00 90 08 80 00 e8 03 80 3e 00 04 20 00 00 00 11 56 34 12 00 ef cd ab 89 02 01 6a 07",
        "25 00 03 05 07 03","26 00 09 05 07 03 10 00 00 00 08 00",
        "1e 00 04 03 02 01 11","03 00 03 22 00 0f"
    };
    const uint16_t codes[]={CIS_CREATE,CIS_PARAM_REQ,CIS_PARAM_RSP,CIS_UPDATE_REQ,CIS_UPDATE_IND,
                            CIS_DISCONNECT,CIS_REJECT};
    for (unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++) {
        cis_signal sig={.type=codes[i],.slot=16,.tag=5,.params=sample(),.offset_slots=8,
                        .link_id=0x010203,.reason=codes[i]==CIS_REJECT?15:17,.rejected_type=CIS_CREATE};
        uint8_t actual[CIS_CTRL_MAX_FRAME], expected[CIS_CTRL_MAX_FRAME];
        size_t len=from_hex(gold[i],expected);
        CHECK(cis_wire_encode(&sig,actual,sizeof(actual))==len && !memcmp(expected,actual,len));
        cis_signal parsed;
        CHECK(cis_wire_decode(expected,len,&parsed));
        CHECK(cis_wire_encode(&parsed,actual,sizeof(actual))==len && !memcmp(expected,actual,len));
        CHECK(!cis_wire_decode(expected,len-1,&parsed));
        if (codes[i]<=CIS_PARAM_RSP && codes[i]>=CIS_CREATE) {
            CHECK(!parsed.params.t_sends_first && parsed.params.dir[0].max_offset_slots==1);
            CHECK(parsed.params.dir[0].discard_cycles==3 && parsed.params.dir[1].crc_seed==0x89abcdef);
            expected[len-1]|=0x80;
            CHECK(!cis_wire_decode(expected,len,&parsed));
        }
    }
    uint8_t packet[CIS_CTRL_MAX_FRAME];
    cis_signal edge={.type=CIS_PARAM_RSP,.tag=15,.params=sample()}, parsed;
    edge.params.dir[0].discard_cycles=256;
    edge.params.dir[0].new_packets=16;
    edge.params.dir[0].max_offset_slots=512;
    CHECK(cis_wire_encode(&edge,packet,sizeof(packet))==55);
    CHECK(cis_wire_decode(packet,55,&parsed) && parsed.params.dir[0].discard_cycles==256 &&
          parsed.params.dir[0].new_packets==16 && parsed.params.dir[0].max_offset_slots==512);
    edge.type=CIS_PARAM_REQ; CHECK(!cis_wire_encode(&edge,packet,sizeof(packet)));
    edge.type=CIS_CREATE; CHECK(!cis_wire_encode(&edge,packet,sizeof(packet)));
    puts("PASS control golden frames 0022/23/24/25/26/001E/0003, distinct biases, role bit, malformed TLV");
}
static void control_roundtrip(void)
{
    endpoint g,t; init(&g,true); init(&t,false);
    cis_params p=sample();
    CHECK(cis_create(&g.control,&p,17,1000)==CIS_CTL_INVALID);
    CHECK(cis_create(&g.control,&p,16,1000)==CIS_CTL_OK);
    CHECK(relay(&g,&t,1010)==CIS_CTL_OK && !t.len); /* implicit at instant */
    CHECK(cis_control_tick(&g.control,4999)==CIS_CTL_OK && g.control.state==CIS_ARMED);
    CHECK(cis_control_tick(&g.control,5000)==CIS_CTL_OK && g.control.state==CIS_ACTIVE);
    CHECK(cis_control_tick(&t.control,5000)==CIS_CTL_OK && t.control.anchor_slot==16);

    cis_params candidate=p;
    candidate.intra_us=50;
    CHECK(cis_propose(&g.control,5,&candidate,5010)==CIS_CTL_OK);
    CHECK(relay(&g,&t,5020)==CIS_CTL_OK && !g.control.known_tags);
    CHECK(relay(&t,&g,5030)==CIS_CTL_OK);
    CHECK(cis_request_update(&t.control,5,5040)==CIS_CTL_OK);
    CHECK(relay(&t,&g,5050)==CIS_CTL_OK && g.control.update_pending);
    CHECK(relay(&g,&t,5060)==CIS_CTL_OK && t.control.update_pending);
    CHECK(g.control.effective_us==8000 && t.control.effective_us==8000);
    CHECK(cis_control_tick(&g.control,7999)==CIS_CTL_OK && g.control.active.intra_us==25);
    CHECK(cis_group_permitted(&g.control,7000) && !cis_group_permitted(&g.control,8000));
    CHECK(cis_control_tick(&g.control,8000)==CIS_CTL_OK && g.control.active.intra_us==50);
    CHECK(cis_control_tick(&t.control,8000)==CIS_CTL_OK && t.control.anchor_us==8000);
    CHECK(cis_update(&g.control,5,48,8,8000)==CIS_CTL_OK);
    CHECK(relay(&g,&t,8000)==CIS_CTL_OK);
    CHECK(cis_control_tick(&g.control,10000)==CIS_CTL_OK && g.commits==3);
    CHECK(cis_control_tick(&t.control,10000)==CIS_CTL_OK && t.commits==3);

    CHECK(cis_disconnect(&t.control,17,11000)==CIS_CTL_OK);
    CHECK(relay(&t,&g,11010)==CIS_CTL_OK && g.control.state==CIS_OFF);
    CHECK(cis_disconnect_ack(&t.control)==CIS_CTL_OK && t.control.state==CIS_OFF);
    CHECK(g.releases==1 && t.releases==1);
    CHECK(cis_disconnect_ack(&t.control)==CIS_CTL_STALE);
    puts("PASS G/T create at wrapped parent instant, exchange, both update paths, atomic switch, disconnect/ACK");
}
static void failure_paths(void)
{
    endpoint g,t; init(&g,true); init(&t,false); cis_params p=sample();
    g.fail_send=true;
    CHECK(cis_create(&g.control,&p,16,1000)==CIS_CTL_IO && g.releases==1 && g.control.state==CIS_OFF);
    g.fail_send=false; t.deny=true;
    CHECK(cis_create(&g.control,&p,16,1000)==CIS_CTL_OK);
    CHECK(relay(&g,&t,1000)==CIS_CTL_REJECTED);
    CHECK(relay(&t,&g,1000)==CIS_CTL_REJECTED && g.control.reason==9 && g.control.state==CIS_OFF);
    CHECK(cis_create(&g.control,&p,16,1000)==CIS_CTL_OK);
    CHECK(relay(&g,&t,5001)==CIS_CTL_REJECTED);
    cis_signal late;
    CHECK(cis_wire_decode(t.outgoing,t.len,&late) && late.reason==21);
    /* The reliable parent failed to deliver the request before its instant.
       A terminal post-instant refusal must also stop an already active G. */
    CHECK(cis_control_tick(&g.control,5001)==CIS_CTL_OK && g.control.state==CIS_ACTIVE);
    CHECK(relay(&t,&g,5002)==CIS_CTL_REJECTED && !g.control.held);
    init(&g,true); init(&t,false);
    p.slot_us=50; p.group_slots=20; p.event_slots=10; p.t_sends_first=true;
    CHECK(cis_create(&g.control,&p,16,1000)==CIS_CTL_OK);
    CHECK(relay(&g,&t,1000)==CIS_CTL_OK);
    CHECK(cis_control_tick(&t.control,5000)==CIS_CTL_OK && t.control.anchor_slot==0 && t.control.active.t_sends_first);
    CHECK(cis_control_tick(&g.control,5000)==CIS_CTL_OK);
    CHECK(cis_propose(&g.control,6,&p,5100)==CIS_CTL_OK);
    CHECK(relay(&g,&t,5110)==CIS_CTL_OK);
    CHECK(cis_control_tick(&g.control,7600)==CIS_CTL_TIMEOUT);
    CHECK(relay(&t,&g,7601)==CIS_CTL_STALE && g.control.state==CIS_ACTIVE);
    CHECK(cis_disconnect(&g.control,17,8000)==CIS_CTL_OK);
    CHECK(cis_control_tick(&g.control,10500)==CIS_CTL_TIMEOUT && g.control.state==CIS_OFF);
    cis_parent_down(&t.control); cis_parent_down(&t.control); cis_parent_down(&g.control);
    CHECK(g.acquisitions==g.releases && t.acquisitions==t.releases);
    puts("PASS transport failure, resource rejection, non-125us zero epoch, timeout, stale response, parent loss");
}
void test_control(void)
{
    wire_golden(); control_roundtrip(); failure_paths();
}
