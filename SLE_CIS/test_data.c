#include "cis_data.h"
#include "../SLE_BIS/sle_adv.h"
#include "../SLE_BIS/sle_sch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static iso_config profile(void)
{
    return (iso_config){.linked=true,.max_sdu=8,.max_pdu=4,.group_period_us=1000,
                        .sdu_period_us=1000,.anchor_delay_us=750,.discard_cycles=2};
}
static void wire_vectors(void)
{
    iso_config c=profile();
    c.multiplexed=true;
    const uint8_t sdu[]={1,2,3,4,5,6,7,8};
    const uint8_t expected[]={0xfe,0xff,1,0xab,0x26,0,0,5,6,7,8};
    uint8_t bytes[32]; size_t used=0; iso_fragment f;
    assert(iso_encode(&c,0x1ab,0,1,sdu,8,bytes,sizeof(bytes),&used)==ISO_OK);
    assert(used==sizeof(expected) && !memcmp(bytes,expected,used));
    assert(iso_decode(&c,expected,sizeof(expected),&f)==ISO_OK && f.group_low==0xab &&
           f.pdu_index==1 && f.segment==3 && f.length==4);
    bytes[2]=0x10; assert(iso_decode(&c,bytes,used,&f)==ISO_INVALID);
    bytes[2]=1; bytes[4]|=1; assert(iso_decode(&c,bytes,used,&f)==ISO_INVALID);
    assert(iso_decode(&c,expected,sizeof(expected)-1,&f)==ISO_INVALID);
    cis_a3 a={.type=0,.send_seq=31,.receive_seq=17,.more=true,.length=0x123};
    const uint8_t gold_a3[]={0xf8,0x71,0x46,0x02};
    assert(cis_a3_encode(&a,bytes) && !memcmp(bytes,gold_a3,4));
    assert(cis_a3_decode(gold_a3,&a) && !a.async_scheduled && a.length==0x123);
    bytes[3]|=0x80; assert(!cis_a3_decode(bytes,&a));
    puts("PASS A3 golden header and periodic PDU 0xFFFE mux / unicast wire vectors");
}
static void retry_across_groups(void)
{
    iso_config c=profile();
    /* Origin group 255; at group 256 A3 sequence base has advanced by 2.
       The inner group field still identifies first TX group 255. */
    cis_tx t={0}; iso_rx r={0}; const uint8_t data[]={0,1,0,0xff,4,5,6,7};
    uint8_t h[4],ack[4],w[32],out[8]; size_t n,used;
    assert(cis_tx_load(&t,&c,255,0,data,8)==ISO_OK);
    assert(cis_tx_load(&t,&c,255,0,data,8)==ISO_BUSY);
    assert(cis_tx_pdu(&t,255,h,w,2,&n)==ISO_CAPACITY);
    assert(cis_tx_pdu(&t,255,h,w,sizeof(w),&n)==ISO_OK && ((h[0]>>3)&31)==2);
    assert(cis_rx_pdu(&r,&c,255,10000,h,w,n,false,true,ack)==ISO_CRC);
    assert(cis_rx_pdu(&r,&c,255,10000,h,w,n,true,false,ack)==ISO_CRC);
    assert(cis_tx_feedback(&t,255,ack,true)==ISO_RETRY);
    assert(cis_rx_pdu(&r,&c,255,10000,h,w,n,true,true,ack)==ISO_OK);
    assert(cis_tx_feedback(&t,255,ack,false)==ISO_CRC); /* ACK lost */
    assert(cis_tx_pdu(&t,256,h,w,sizeof(w),&n)==ISO_OK && (h[0]>>3)==0 && w[1]==255);
    assert(cis_rx_pdu(&r,&c,256,11000,h,w,n,true,true,ack)==ISO_DUPLICATE);
    assert(cis_tx_feedback(&t,256,ack,true)==ISO_OK);
    assert(cis_tx_pdu(&t,256,h,w,sizeof(w),&n)==ISO_OK && (h[0]>>3)==1);
    assert(cis_rx_pdu(&r,&c,256,11000,h,w,n,true,true,ack)==ISO_OK);
    assert(cis_tx_feedback(&t,256,ack,true)==ISO_OK && !t.active);
    assert(iso_deliver(&r,11749,out,sizeof(out),&used)==ISO_WAIT);
    assert(iso_deliver(&r,11750,out,3,&used)==ISO_CAPACITY);
    assert(iso_deliver(&r,11750,out,sizeof(out),&used)==ISO_OK && used==8 && !memcmp(out,data,8));
    assert(iso_deliver(&r,11750,out,sizeof(out),&used)==ISO_WAIT);
    puts("PASS unicast retry, missing ACK, cross-group numbering, duplicate ACK and timed SDU");
}
static void iso_limits(void)
{
    iso_config c=profile();
    c.group_period_us=2000;
    const uint8_t data[]={1,2,3,4,5,6,7,8};
    uint8_t w[16],out[8]; size_t n,used;
    iso_rx r={0};
    /* Two SDUs from group 0. Second is a zero-length service unit. */
    for (unsigned ord=0;ord<4;ord++) {
        assert(iso_encode(&c,0,(uint8_t)(ord/2),(uint8_t)(ord%2),data,ord<2?8:0,w,sizeof(w),&n)==ISO_OK);
        assert(iso_ingest(&r,&c,0,20000,ord,w,n)==ISO_OK);
    }
    assert(iso_deliver(&r,22750,out,sizeof(out),&used)==ISO_OK && used==8);
    assert(iso_deliver(&r,23749,NULL,0,&used)==ISO_WAIT);
    assert(iso_deliver(&r,23750,NULL,0,&used)==ISO_OK && !used);
    assert(iso_encode(&c,1,0,0,data,8,w,sizeof(w),&n)==ISO_OK);
    assert(iso_ingest(&r,&c,1,22000,0,w,n)==ISO_OK);
    assert(iso_expire(&r,24749)==0 && iso_expire(&r,24750)==1);
    assert(iso_encode(&c,1,0,1,data,8,w,sizeof(w),&n)==ISO_OK);
    assert(iso_ingest(&r,&c,1,22000,1,w,n)==ISO_GAP);
    cis_tx t={0}; uint8_t h[4];
    assert(cis_tx_load(&t,&c,1,0,data,sizeof(data))==ISO_OK);
    assert(cis_tx_pdu(&t,3,h,w,sizeof(w),&n)==ISO_EXPIRED && !t.active);
    c.sdu_period_us=999; assert(!iso_config_valid(&c));
    puts("PASS periodic multi-SDU timing, zero-length SDU, fragmented SDU expiry and missing first");
}
static void pending_capacity_and_modulo(void)
{
    /* Four SDUs per group. With eight discard cycles the last SDU of
       group 7 has local A3 sequence 31; accepting it is signaled by 0. */
    iso_config c=profile();
    c.max_sdu=c.max_pdu=1; c.sdu_period_us=250; c.discard_cycles=8;
    const uint8_t byte=0x80;
    uint8_t h[4],w[8],reply[4],out[1];
    size_t len,used;
    cis_tx t={0}; iso_rx r={0};
    assert(cis_tx_load(&t,&c,7,3,&byte,1)==ISO_OK);
    assert(cis_tx_pdu(&t,7,h,w,sizeof(w),&len)==ISO_OK && (h[0]>>3)==31);
    assert(cis_rx_pdu(&r,&c,7,7000,h,w,len,true,true,reply)==ISO_OK);
    cis_a3 fb;
    assert(cis_a3_decode(reply,&fb) && fb.receive_seq==0);
    assert(cis_tx_feedback(&t,7,reply,true)==ISO_OK);
    memset(&r,0,sizeof(r));
    for (unsigned ord=0;ord<4;ord++) {
        assert(iso_encode(&c,0,(uint8_t)ord,0,&byte,1,w,sizeof(w),&len)==ISO_OK);
        assert(iso_ingest(&r,&c,0,0,ord,w,len)==ISO_OK);
    }
    assert(iso_encode(&c,1,0,0,&byte,1,w,sizeof(w),&len)==ISO_OK);
    assert(iso_ingest(&r,&c,1,1000,0,w,len)==ISO_BUSY);
    assert(iso_deliver(&r,7750,out,sizeof(out),&used)==ISO_OK && out[0]==byte);
    assert(iso_ingest(&r,&c,1,1000,0,w,len)==ISO_OK);
    puts("PASS A3 ACK 31-to-0 wrap and ISOAL backpressure/retry");
}

/* Two real BIS controllers, with a stub radio and this module's ISOAL in
   their public APIs. The pre-existing BIS max_pdu limit includes wire bytes;
   use 4 service bytes + 3 header bytes under an 8-byte advertisement. */
typedef struct {
    sle_sch sch;
    iso_config cfg;
    iso_rx iso;
    bis_event event;
    bis_a6 header;
    uint8_t wire[32];
} b_stub;
static bool b_reserve(void *ctx,const bis_event *e)
{
    b_stub *b=ctx;
    return sle_reserve(&b->sch,b,e->token,e->time_us,e->duration_us);
}
static void b_release(void *ctx,uint64_t token)
{
    b_stub *b=ctx; sle_release(&b->sch,b,token);
}
static bool b_submit(void *ctx,const bis_event *e,const bis_a6 *a,const uint8_t *p)
{
    b_stub *b=ctx; b->event=*e;
    if (a) {
        if (a->length>sizeof(b->wire)) return false;
        b->header=*a; memcpy(b->wire,p,a->length);
    }
    return true;
}
static void b_cancel(void *ctx,uint64_t token) { (void)ctx; (void)token; }
static bool b_receive(void *ctx,const bis_event *e,const bis_a6 *a,const uint8_t *p)
{
    b_stub *b=ctx;
    return iso_ingest(&b->iso,&b->cfg,e->cycle,e->time_us,e->index,p,a->length)==ISO_OK;
}
static bool b_management(void *ctx,const uint8_t *m,size_t n,uint64_t ts)
{
    (void)ctx; (void)ts;
    return n==15 && m[13]==1;
}
static bis_port port(b_stub *b)
{
    return (bis_port){b,b_reserve,b_release,b_submit,b_cancel,b_receive,b_management};
}
static void broadcast_integration(void)
{
    b_stub a={0},b={0};
    iso_config c=profile(); c.linked=false; c.max_pdu=8; c.discard_cycles=1;
    a.cfg=b.cfg=c;
    bis_controller tx,rx;
    bis_config adv={.base_slot=100,.offset_us=500,.set_id=9,.group_slots=8,.event_slots=4,
        .events=1,.anchor_delay_us=750,.link_base=0x123456,.crc_base=0xabcdef,
        .max_sdu=8,.max_pdu=8,.new_packets=1,.sdu_period_us=1000,.map={255}};
    assert(bis_init(&tx,port(&a),150,2) && bis_init(&rx,port(&b),150,2));
    assert(bis_create(&tx,&adv,1000)==BIS_OK);
    uint8_t aux[4],tlv[100],m[15]={0}; m[13]=1;
    size_t tl=bis_advertise_at(&tx,1000,m,sizeof(m),tlv,sizeof(tlv));
    assert(tl>0 && bis_aux_encode(7,1,false,aux));
    assert(bis_scan(&rx,4000)==BIS_OK && bis_basic(&rx,aux,875)==BIS_OK);
    assert(bis_extended(&rx,tlv,tl,7,1000)==BIS_OK);
    const uint8_t data[]={0x11,0x00,0xfe,0xff},gold[]={32,0,0,0x11,0x00,0xfe,0xff};
    uint8_t wire[16],out[4],air[4]; size_t n,used;
    assert(iso_encode(&a.cfg,0,0,0,data,sizeof(data),wire,sizeof(wire),&n)==ISO_OK);
    assert(n==sizeof(gold) && memcmp(wire,gold,n)==0);
    bis_a6 pkt={.seq=0,.group=1,.last=true,.length=(uint16_t)n};
    assert(bis_schedule(&tx,1490,&pkt,wire)==BIS_OK && bis_schedule(&rx,1490,NULL,NULL)==BIS_OK);
    assert(a.event.channel==b.event.channel && a.event.time_us==1500 && a.event.time_us==b.event.time_us);
    assert(bis_a6_encode(&a.header,air) && bis_a6_decode(air,&pkt));
    assert(bis_receive(&rx,b.event.token,true,true,pkt,a.wire,n)==BIS_OK);
    assert(bis_receive(&rx,b.event.token,true,true,pkt,a.wire,n)==BIS_DUPLICATE);
    assert(bis_done(&rx,b.event.token)==BIS_OK && bis_done(&tx,a.event.token)==BIS_OK);
    assert(iso_deliver(&b.iso,2249,out,sizeof(out),&used)==ISO_WAIT);
    assert(iso_deliver(&b.iso,2250,out,sizeof(out),&used)==ISO_OK && used==sizeof(data));
    assert(!memcmp(out,data,used));
    bis_stop(&tx); bis_stop(&rx);
    puts("PASS shared ISOAL real BIS advertise, sync, A6 TX/RX and timed SDU delivery");
}

void test_data(void)
{
    wire_vectors();
    retry_across_groups();
    iso_limits();
    pending_capacity_and_modulo();
    broadcast_integration();
}
