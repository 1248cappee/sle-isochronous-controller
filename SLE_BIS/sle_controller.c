#include "sle_bis.h"
#include <string.h>

static bool running(const bis_controller *c)
{
    return c->state == BIS_TX || c->state == BIS_RX;
}
bool bis_init(bis_controller *c, bis_port port, uint32_t duration, uint32_t limit)
{
    if (!c || !duration || !limit || !port.reserve || !port.release ||
        !port.submit || !port.cancel || !port.pdu || !port.management) return false;
    *c=(bis_controller){.port=port,.duration=duration,.loss_limit=limit,.stop_at=UINT64_MAX};
    return true;
}
static bis_result activate(bis_controller *c,const bis_config *cfg,uint64_t adv,bis_state role)
{
    if (!bis_config_valid(cfg) || cfg->new_packets>cfg->events ||
        adv>UINT64_MAX-cfg->offset_us ||
        (cfg->events>1 && c->duration>cfg->event_slots*BIS_SLOT_US) ||
        (uint64_t)(cfg->events-1)*cfg->event_slots*BIS_SLOT_US+c->duration
            > (uint64_t)cfg->group_slots*BIS_SLOT_US) return BIS_INVALID;
    c->cfg=*cfg;
    c->anchor=adv+cfg->offset_us;
    c->state=role;
    c->next=0;
    c->misses=0;
    c->stop_at=UINT64_MAX;
    return BIS_OK;
}
bis_result bis_create(bis_controller *c,const bis_config *cfg,uint64_t adv)
{
    if (c->state!=BIS_OFF) return BIS_BUSY;
    return activate(c,cfg,adv,BIS_TX);
}
bis_result bis_scan(bis_controller *c,uint64_t deadline)
{
    if(c->state!=BIS_OFF) return BIS_BUSY;
    c->deadline=deadline;
    c->state=BIS_SCAN;
    return BIS_OK;
}
bis_result bis_basic(bis_controller *c,const uint8_t aux[4],uint64_t us)
{
    if(c->state!=BIS_SCAN && c->state!=BIS_WAIT_EXT) return BIS_IDLE;
    if(us>=c->deadline) return bis_tick(c,us);
    if(!aux || aux[0]>78 || (aux[2]>>4)!=0 || (aux[3]&3)!=0) return BIS_INVALID;
    uint32_t delta=(aux[1]+((aux[2]&15u)<<8))*BIS_SLOT_US;
    if(aux[3]&128) delta*=10;
    if(!delta || UINT64_MAX-us<delta) return BIS_INVALID;
    c->ext_at=us+delta;
    c->ext_channel=aux[0];
    c->state=BIS_WAIT_EXT;
    return BIS_OK;
}
bis_result bis_extended(bis_controller *c,const uint8_t *p,size_t len,
                        uint8_t channel,uint64_t us)
{
    if(c->state!=BIS_WAIT_EXT) return BIS_IDLE;
    if(us>=c->deadline) return bis_tick(c,us);
    if(channel!=c->ext_channel || us!=c->ext_at) return BIS_FILTERED;
    if(!p) return BIS_INVALID;
    bis_config cfg={0};
    bool link=false;
    const uint8_t *mgmt=NULL;
    size_t mgmt_len=0;
    while(len){
        if(len<2 || p[1]>len-2) return BIS_INVALID;
        size_t n=p[1];
        if(p[0]==6){
            if(link || !bis_link_decode(p+2,n,&cfg)) return BIS_INVALID;
            link=true;
        }else if(p[0]==5){
            if(mgmt || n<15 || p[2+13]==0 || n!=14u+p[2+13]) return BIS_INVALID;
            mgmt=p+2; mgmt_len=n;
        }
        p+=2+n; len-=2+n;
    }
    if(!link || !mgmt) return BIS_INVALID;
    bis_result rc=activate(c,&cfg,us,BIS_RX);
    if(rc!=BIS_OK) return rc;
    if(!c->port.management(c->port.ctx,mgmt,mgmt_len,us)){
        c->state=BIS_WAIT_EXT;
        return BIS_IO;
    }
    return BIS_OK;
}
static void retire(bis_controller *c,bool cancel)
{
    if(!c->pending) return;
    if(cancel) c->port.cancel(c->port.ctx,c->active.token);
    c->port.release(c->port.ctx,c->active.token);
    c->pending=false;
}
void bis_stop(bis_controller *c)
{
    retire(c,true);
    c->state=BIS_OFF;
    c->stop_at=UINT64_MAX;
}
static bis_result miss(bis_controller *c,uint64_t count,bis_result why)
{
    if(c->state==BIS_RX){
        if(count>=c->loss_limit-c->misses){
            bis_stop(c);
            return BIS_LOST;
        }
        c->misses+=(uint32_t)count;
    }
    return why;
}
bis_result bis_tick(bis_controller *c,uint64_t now)
{
    if((c->state==BIS_SCAN || c->state==BIS_WAIT_EXT) && now>=c->deadline){
        bis_stop(c); return BIS_TIMEOUT;
    }
    if(running(c) && now>=c->stop_at){
        bis_stop(c); return BIS_STOPPED;
    }
    if(c->pending && now>=c->active.time_us+c->duration){
        retire(c,true);
        if(c->heard){ c->misses=0; return BIS_TIMEOUT; }
        return miss(c,1,BIS_TIMEOUT);
    }
    return BIS_OK;
}
bis_result bis_disconnect(bis_controller *c,uint32_t slot,uint64_t now)
{
    if(!running(c) || slot>BIS_SLOT_MASK) return BIS_INVALID;
    /* Compare on the 30-bit air time line; monotonic local time may have an
       arbitrary epoch. Indications must be within half the slot modulus. */
    uint32_t ahead=(slot-c->cfg.base_slot)&BIS_SLOT_MASK;
    uint64_t first=c->anchor;
    if(now>=first){
        uint64_t elapsed=(now-first)/BIS_SLOT_US;
        first+=elapsed*BIS_SLOT_US;
        ahead=(ahead-(uint32_t)elapsed)&BIS_SLOT_MASK;
    }
    if(ahead > BIS_SLOT_MASK/2u || UINT64_MAX-first<ahead*(uint64_t)BIS_SLOT_US)
        return BIS_INVALID;
    c->stop_at=first+ahead*(uint64_t)BIS_SLOT_US;
    if(c->pending && c->active.time_us>=c->stop_at) retire(c,true);
    return bis_tick(c,now);
}
bis_result bis_schedule(bis_controller *c,uint64_t now,const bis_a6 *tx,const uint8_t *bytes)
{
    bis_result rc=bis_tick(c,now);
    if(rc!=BIS_OK) return rc;
    if(!running(c)) return BIS_IDLE;
    if(c->pending) return BIS_BUSY;
    const bis_config *cfg=&c->cfg;
    uint64_t period=(uint64_t)cfg->group_slots*BIS_SLOT_US;
    uint64_t spacing=(uint64_t)cfg->event_slots*BIS_SLOT_US;
    if(now>c->anchor){
        uint64_t delta=now-c->anchor;
        uint64_t index=(delta%period+spacing-1)/spacing;
        if(index>cfg->events) index=cfg->events;
        uint64_t first=(delta/period)*cfg->events+index;
        if(first>c->next){
            uint64_t skipped=first-c->next;
            c->next=first;
            return miss(c,skipped,BIS_MISSED);
        }
    }
    uint64_t cycle=c->next/cfg->events;
    uint8_t index=(uint8_t)(c->next%cfg->events);
    uint64_t offset=index*spacing;
    if(cycle>(UINT64_MAX-c->anchor)/period) return BIS_INVALID;
    uint64_t base=c->anchor+cycle*period;
    if(UINT64_MAX-base<offset+c->duration || c->serial==UINT64_MAX) return BIS_INVALID;
    uint64_t target=base+offset;
    if(target>=c->stop_at) return BIS_IDLE;
    bool sending=c->state==BIS_TX;
    if(sending && (!tx || tx->seq>31 || tx->group!=1 || tx->type!=0 ||
        tx->length>cfg->max_pdu || (tx->length && !bytes))) return BIS_INVALID;
    uint32_t slot=(cfg->base_slot+(uint32_t)(cycle*cfg->group_slots)+index*cfg->event_slots)&BIS_SLOT_MASK;
    bis_event e={.token=++c->serial,.time_us=target,.cycle=cycle,.slot=slot,
        .link_id=cfg->link_base^1u,.crc_seed=(cfg->crc_base^1u)&(cfg->crc32?UINT32_MAX:0xffffffu),
        .duration_us=c->duration,.index=index,.channel=bis_channel(cfg,(uint16_t)slot),
        .pilot=cfg->pilot,.max_pdu=cfg->max_pdu,.tx=sending,.crc32=cfg->crc32};
    ++c->next;
    if(!c->port.reserve(c->port.ctx,&e)) return miss(c,1,BIS_CONFLICT);
    if(!c->port.submit(c->port.ctx,&e,sending?tx:NULL,sending?bytes:NULL)){
        c->port.release(c->port.ctx,e.token);
        return miss(c,1,BIS_IO);
    }
    c->active=e; c->pending=true; c->seen=0; c->heard=false;
    return BIS_OK;
}
bis_result bis_receive(bis_controller *c,uint64_t token,bool hcrc,bool dcrc,
                       bis_a6 a,const uint8_t *payload,size_t len)
{
    if(c->state!=BIS_RX || !c->pending || c->active.token!=token) return BIS_STALE;
    if(!hcrc || !dcrc) return BIS_CRC;
    if(a.group!=1 || a.type!=0) return BIS_FILTERED;
    if(a.seq>31 || len!=a.length || len>c->cfg.max_pdu || (len && !payload))
        return BIS_INVALID;
    c->heard=true;
    uint32_t mask=UINT32_C(1)<<a.seq;
    if(c->seen&mask) return BIS_DUPLICATE;
    if(!c->port.pdu(c->port.ctx,&c->active,&a,payload)) return BIS_IO;
    c->seen|=mask;
    return BIS_OK;
}
bis_result bis_done(bis_controller *c,uint64_t token)
{
    if(!c->pending || token!=c->active.token) return BIS_STALE;
    retire(c,false);
    if(c->state==BIS_RX && !c->heard) return miss(c,1,BIS_MISSED);
    c->misses=0;
    return BIS_OK;
}
