#include "sle_adv.h"
#include "sle_sch.h"
#include "sle_fpga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#define CHECK(x) do { if(!(x)){ fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)

typedef struct {
    bis_controller ctl;
    sle_sch sch;
    bis_fpga fp;
    bis_event desc;
    bis_a6 a6;
    uint8_t payload[BIS_MAX_PDU], sdu[8], fragment_mask;
    uint64_t assembling_cycle, delivery_us;
    unsigned pdus, sdus, mgmt_calls, cancels, acks;
    bool sdu_ready;
    bool fail_stage, staged, armed, irq_valid, reject_pdu;
    bis_irq irq;
} node;
static bool reserve(void *p,const bis_event *e){
    node *n=p; return sle_reserve(&n->sch,n,e->token,e->time_us,e->duration_us);
}
static void release(void *p,uint64_t token){
    node *n=p; sle_release(&n->sch,n,token);
}
static bool stage(void *p,const bis_event *e,const bis_a6 *a,const uint8_t *data){
    node *n=p; CHECK(!n->armed && !n->staged);
    if(n->fail_stage) return false;
    n->staged=true; n->desc=*e;
    if(a){
        n->a6=*a;
        if(a->length) memcpy(n->payload,data,a->length);
    }
    return true;
}
static void publish(void *p,uint64_t token){
    node *n=p; CHECK(n->staged && n->desc.token==token);
    n->staged=false; n->armed=true;
}
static void abort_dma(void *p,uint64_t token){
    node *n=p; CHECK(n->armed && n->desc.token==token);
    n->armed=false; n->cancels++;
}
static bool peek(void *p,bis_irq *q){
    node *n=p;
    if(!n->irq_valid) return false;
    *q=n->irq; return true;
}
static void ack(void *p,const bis_irq *q){
    node *n=p; CHECK(n->irq_valid && q->token==n->irq.token);
    n->irq_valid=false; n->acks++;
}
static bool submit(void *p,const bis_event *e,const bis_a6 *a,const uint8_t *data){
    return bis_fpga_submit(&((node *)p)->fp,e,a,data);
}
static void cancel(void *p,uint64_t token){ bis_fpga_cancel(&((node *)p)->fp,token); }
/* A-side test double for exactly one eight-byte SDU in two scheduled PDUs. */
static bool pdu(void *p,const bis_event *e,const bis_a6 *a,const uint8_t *data){
    node *n=p;
    if(n->reject_pdu || !data || a->length!=4 || e->index>=2) return false;
    if(n->assembling_cycle!=e->cycle) n->fragment_mask=0;
    n->assembling_cycle=e->cycle;
    memcpy(n->sdu+4*e->index,data,4);
    n->pdus++; n->fragment_mask|=(uint8_t)(1u<<e->index);
    if(n->fragment_mask==3){
        n->sdu_ready=true;
        n->delivery_us=e->time_us-e->index*n->ctl.cfg.event_slots*BIS_SLOT_US+
                       n->ctl.cfg.anchor_delay_us;
    }
    return true;
}
static bool management(void *p,const uint8_t *m,size_t len,uint64_t us){
    node *n=p; CHECK(us==1250);
    if(len!=15 || m[13]!=1 || !m[9] || m[14]>78) return false;
    n->mgmt_calls++; return true;
}
static bool deliver_sdu(node *n,uint64_t now,uint8_t out[8]){
    if(!n->sdu_ready || now<n->delivery_us) return false;
    memcpy(out,n->sdu,8);
    n->sdus++;
    n->sdu_ready=false;
    return true;
}
static void init(node *n){
    memset(n,0,sizeof(*n));
    n->fp=(bis_fpga){n,stage,publish,abort_dma,peek,ack};
    bis_port port={n,reserve,release,submit,cancel,pdu,management};
    CHECK(bis_init(&n->ctl,port,200,3));
}
static bis_config config(void){
    bis_config c={.base_slot=BIS_SLOT_MASK-1,.offset_us=375,.set_id=7,.group_slots=8,
        .event_slots=2,.events=2,.anchor_delay_us=750,.link_base=0x123456,
        .crc_base=0xabcdef,.max_sdu=8,.max_pdu=4,.sdu_period_us=1000,.new_packets=2};
    memset(c.map,255,9); c.map[9]=15; return c;
}
static size_t advertise(const bis_controller *tx,uint8_t *tlv){
    uint8_t mgmt[15]={0};
    mgmt[4]=125; mgmt[6]=2; mgmt[9]=8; mgmt[13]=1; mgmt[14]=24;
    return bis_advertise_at(tx,1250,mgmt,sizeof(mgmt),tlv,100);
}
static void discover(node *r,const uint8_t *tlv,size_t n){
    uint8_t aux[4];
    CHECK(n==65 && bis_aux_encode(17,2,false,aux));
    CHECK(bis_scan(&r->ctl,2500)==BIS_OK);
    CHECK(bis_basic(&r->ctl,aux,1000)==BIS_OK);
    CHECK(bis_extended(&r->ctl,tlv,n,16,1250)==BIS_FILTERED);
    CHECK(bis_extended(&r->ctl,tlv,n,17,1249)==BIS_FILTERED);
    CHECK(bis_extended(&r->ctl,tlv,n-1,17,1250)==BIS_INVALID);
    CHECK(bis_extended(&r->ctl,tlv+17,n-17,17,1250)==BIS_INVALID);
    CHECK(bis_extended(&r->ctl,tlv,n,17,1250)==BIS_OK);
    CHECK(r->ctl.anchor==1625 && r->ctl.state==BIS_RX && r->mgmt_calls==1);
}
static bis_result irq(node *n,bis_irq q){
    n->irq=q; n->irq_valid=true;
    if(q.kind==BIS_IRQ_END && n->desc.token==q.token) n->armed=false;
    return bis_fpga_service(&n->fp,&n->ctl);
}
static bis_result end(node *n){
    return irq(n,(bis_irq){.kind=BIS_IRQ_END,.token=n->desc.token});
}
static bis_result air(node *t,node *r,bool crc){
    CHECK(t->armed && r->armed && t->desc.tx && !r->desc.tx);
    CHECK(t->desc.time_us==r->desc.time_us && t->desc.channel==r->desc.channel);
    CHECK(t->desc.link_id==r->desc.link_id && t->desc.crc_seed==r->desc.crc_seed);
    uint8_t packed[4]; bis_a6 decoded;
    CHECK(bis_a6_encode(&t->a6,packed) && bis_a6_decode(packed,&decoded));
    return irq(r,(bis_irq){BIS_IRQ_PACKET,r->desc.token,true,crc,decoded,t->payload,decoded.length});
}
static void data_test(void){
    node t,r; init(&t); init(&r); bis_config c=config();
    uint8_t broadcast[100];
    CHECK(bis_create(&t.ctl,&c,1250)==BIS_OK);
    size_t length=advertise(&t.ctl,broadcast);
    discover(&r,broadcast,length);
    const uint8_t sdu[]={0x19,0x11,0x00,0xfe,0x00,0xea,0x70,0xff};
    const uint8_t channels[]={55,48,34,19};
    const uint64_t times[]={1625,1875,2625,2875};
    const uint32_t slots[]={BIS_SLOT_MASK-1,0,6,8};
    for(unsigned i=0;i<4;i++){
        bis_a6 a={.group=1,.seq=31,.last=true,.length=4};
        CHECK(bis_schedule(&t.ctl,times[i],&a,sdu+4*(i%2))==BIS_OK);
        CHECK(bis_schedule(&r.ctl,times[i],NULL,NULL)==BIS_OK);
        CHECK(t.desc.channel==channels[i] && t.desc.time_us==times[i] && t.desc.slot==slots[i]);
        CHECK(t.desc.link_id==0x123457 && t.desc.crc_seed==0xabcdee);
        CHECK(air(&t,&r,false)==BIS_CRC);
        CHECK(r.pdus==i);
        r.reject_pdu=true; CHECK(air(&t,&r,true)==BIS_IO);
        r.reject_pdu=false; CHECK(air(&t,&r,true)==BIS_OK);
        CHECK(air(&t,&r,true)==BIS_DUPLICATE);
        CHECK(end(&t)==BIS_OK && end(&r)==BIS_OK);
        if(i%2){
            uint8_t delivered[8];
            uint64_t due=i==1?2375:3375;
            CHECK(!deliver_sdu(&r,due-1,delivered));
            CHECK(deliver_sdu(&r,due,delivered) && !memcmp(sdu,delivered,8));
            CHECK(!deliver_sdu(&r,due,delivered));
        }
        printf("EVENT token=%lu us=%lu slot=%u phy_channel=%u accepted_pdus=%u\n",
               (unsigned long)r.desc.token,(unsigned long)r.desc.time_us,r.desc.slot,r.desc.channel,r.pdus);
    }
    CHECK(r.sdus==2 && r.delivery_us==3375 && !memcmp(sdu,r.sdu,8));
    uint8_t late[100],mgmt[15]={0}; mgmt[13]=1; mgmt[9]=8;
    CHECK(bis_advertise_at(&t.ctl,3000,mgmt,15,late,100)==65);
    bis_config next;
    CHECK(bis_link_decode(late+19,46,&next) && next.offset_us==625 && next.base_slot==14);
    bis_stop(&r.ctl); bis_stop(&t.ctl);
    puts("PASS discovery, wrap, hopping, A6, CRC retry, dedup, SDU comparison");
}
static void exception_test(void){
    node r,t; init(&r); init(&t); bis_config c=config();
    uint8_t broadcast[100];
    CHECK(bis_create(&t.ctl,&c,1250)==BIS_OK);
    size_t length=advertise(&t.ctl,broadcast);
    discover(&r,broadcast,length);
    int scanner;
    CHECK(sle_reserve(&r.sch,&scanner,1,1620,100));
    CHECK(bis_schedule(&r.ctl,1620,NULL,NULL)==BIS_CONFLICT);
    sle_release(&r.sch,&scanner,1);
    CHECK(bis_schedule(&r.ctl,1800,NULL,NULL)==BIS_OK && r.desc.time_us==1875);
    uint64_t old=r.desc.token;
    bis_stop(&r.ctl);
    CHECK(r.cancels==1 && bis_scan(&r.ctl,2000)==BIS_OK);
    CHECK(bis_tick(&r.ctl,2000)==BIS_TIMEOUT);
    r.mgmt_calls=0; discover(&r,broadcast,length);
    CHECK(bis_schedule(&r.ctl,1600,NULL,NULL)==BIS_OK);
    CHECK(irq(&r,(bis_irq){.kind=BIS_IRQ_END,.token=old})==BIS_STALE && r.armed);
    bis_a6 other={.group=2,.length=0};
    CHECK(bis_receive(&r.ctl,r.desc.token,true,true,other,NULL,0)==BIS_FILTERED);
    CHECK(bis_receive(&r.ctl,r.desc.token,false,true,other,NULL,0)==BIS_CRC);
    other.group=1; other.length=5;
    CHECK(bis_receive(&r.ctl,r.desc.token,true,true,other,NULL,0)==BIS_INVALID);
    uint8_t disconnect[]={42,0,0,0,0},reason=0;
    CHECK(bis_disconnect_pdu(&r.ctl,disconnect,5,1625,&reason)==BIS_OK && reason==42);
    CHECK(bis_done(&r.ctl,old)==BIS_STALE);
    CHECK(bis_tick(&r.ctl,1825)==BIS_TIMEOUT);
    CHECK(bis_tick(&r.ctl,1874)==BIS_OK);
    CHECK(bis_tick(&r.ctl,1875)==BIS_STOPPED && r.ctl.state==BIS_OFF);
    r.mgmt_calls=0; discover(&r,broadcast,length);
    r.fail_stage=true;
    CHECK(bis_schedule(&r.ctl,1625,NULL,NULL)==BIS_IO);
    CHECK(sle_reserve(&r.sch,&scanner,1,1625,200));
    sle_release(&r.sch,&scanner,1);
    r.fail_stage=false;
    CHECK(bis_schedule(&r.ctl,1876,NULL,NULL)==BIS_MISSED);
    CHECK(bis_schedule(&r.ctl,1876,NULL,NULL)==BIS_OK && r.desc.time_us==2625);
    CHECK(end(&r)==BIS_LOST && r.ctl.state==BIS_OFF);
    CHECK(bis_create(&r.ctl,&c,1250)==BIS_OK);
    CHECK(bis_create(&r.ctl,&c,1250)==BIS_BUSY);
    bis_stop(&r.ctl); c.max_pdu=0;
    CHECK(bis_create(&r.ctl,&c,1250)==BIS_INVALID && r.ctl.state==BIS_OFF);
    puts("PASS conflict, anchor recovery, missed IRQ, late IRQ, disconnect, timeout, loss, rollback");
}
static void vectors(void){
    bis_config c=config(), copy;
    uint8_t raw[46];
    const uint8_t expected[46]={
        1,0xfe,0xff,0xff,0x3f,0x77,1,0,7,1,0,8,0,2,0,2,
        0xee,2,0,0,0,0,0x56,0x34,0x12,0,8,0x80,0x3e,0,4,8,
        0xef,0xcd,0xab,0,255,255,255,255,255,255,255,255,255,15};
    CHECK(bis_link_encode(&c,raw) && !memcmp(raw,expected,46));
    CHECK(bis_link_decode(expected,46,&copy) && copy.base_slot==c.base_slot);
    raw[0]=3; CHECK(!bis_link_decode(raw,46,&copy));
    memset(c.map,0,10); c.map[0]=2; c.map[9]=8;
    CHECK(bis_channel(&c,65534)==1 && bis_channel(&c,0)==75);
    uint8_t a[4]; const uint8_t expected_a[]={0xfc,4,8,0};
    bis_a6 meta={.group=1,.seq=31,.last=true,.length=4};
    CHECK(bis_a6_encode(&meta,a) && !memcmp(a,expected_a,4));
    sle_sch s={0};
    for(unsigned i=0;i<SLE_SCH_CAPACITY;i++) CHECK(sle_reserve(&s,&s,i,i*125,125));
    CHECK(!sle_reserve(&s,&s,32,3000,125));
    for(unsigned i=0;i<SLE_SCH_CAPACITY;i++) sle_release(&s,&s,i);
    CHECK(sle_reserve(&s,&s,32,3000,125));
    puts("PASS golden bytes, remapping, shared SCH capacity and release");
}
int main(void){vectors(); data_test(); exception_test(); puts("ALL TESTS PASSED"); return 0;}
