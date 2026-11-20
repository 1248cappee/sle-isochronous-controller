#include "cis_control.h"
#include <limits.h>
#include <string.h>

/* Reasons from section 7.3.2.5. Missing PCs, busy queues and late instants
   are reported on the existing parent-link control transport. */
enum { BAD_LINK=2, NO_RESOURCE=9, NOT_ALLOWED=11, BAD_PARAM=15, LATE=21, BUSY=25 };
static bool slot_size(uint8_t us)
{
    return us && us<=125 && us%25==0;
}
bool cis_control_init(cis_control *c,cis_port port,bool is_g,cis_parent parent,uint32_t timeout)
{
    if (!c || !port.send || !port.acquire || !port.admit || !port.release ||
        !port.apply || !slot_size(parent.slot_us) || parent.slot>CIS_SLOT_MASK ||
        !parent.period_slots || !timeout) return false;
    *c=(cis_control){.port=port,.is_g=is_g,.parent=parent,.parent_up=true,.timeout_us=timeout};
    return true;
}
static void close_link(cis_control *c)
{
    if (c->held) c->port.release(c->port.ctx);
    c->held=false;
    c->state=CIS_OFF;
    c->known_tags=0;
    c->waiting=0;
    c->update_pending=false;
}
void cis_parent_down(cis_control *c)
{
    close_link(c);
    c->parent_up=false;
}
static bool profile(const cis_control *c,const cis_params *p)
{
    if (!slot_size(p->slot_us) || !p->events || !p->group_slots || !p->event_slots)
        return false;
    uint32_t period=(uint32_t)p->event_slots*p->slot_us;
    uint32_t group=(uint32_t)p->group_slots*p->slot_us;
    uint32_t window=(p->dir[0].max_offset_slots+p->dir[1].max_offset_slots)*125u;
    if (!window || window>period || (uint64_t)(p->events-1)*period+window>group)
        return false;
    for (unsigned j=0;j<2;j++) {
        const cis_direction *d=&p->dir[j];
        if (d->frame || d->bandwidth || !d->sdu_period_us || !d->max_pdu ||
            !d->max_sdu || d->feedback!=(j==0 ? 26 : 7)) return false;
    }
    return c->port.admit(c->port.ctx,p);
}
static cis_result transmit(cis_control *c,const cis_signal *v)
{
    uint8_t wire[CIS_CTRL_MAX_FRAME];
    size_t n=cis_wire_encode(v,wire,sizeof(wire));
    if (!n) return CIS_CTL_INVALID;
    return c->port.send(c->port.ctx,wire,n) ? CIS_CTL_OK : CIS_CTL_IO;
}
static cis_result refuse(cis_control *c,uint16_t type,uint8_t reason)
{
    cis_signal sig={.type=CIS_REJECT,.rejected_type=type,.reason=reason};
    cis_result r=transmit(c,&sig);
    return r==CIS_CTL_OK ? CIS_CTL_REJECTED : r;
}
static void await_reply(cis_control *c,uint16_t type,uint64_t now)
{
    c->waiting=type;
    c->deadline=UINT64_MAX-now<c->timeout_us ? UINT64_MAX : now+c->timeout_us;
}
/* Map a 30-bit on-air scheduling slot to local microseconds. The supplied
   epoch is an actual group boundary; unwrap relative to "now" rather than
   assuming the first parent slot after boot is smaller than the instant. */
static bool future_boundary(uint64_t epoch,uint32_t base,uint8_t tick,uint16_t period,
                            uint32_t slot,uint64_t now,uint64_t *result)
{
    if (now<epoch || slot>CIS_SLOT_MASK) return false;
    uint64_t elapsed=(now-epoch)/tick;
    uint32_t here=(base+(uint32_t)elapsed)&CIS_SLOT_MASK;
    uint32_t ahead=(slot-here)&CIS_SLOT_MASK;
    if (ahead>(CIS_SLOT_MASK>>1) || elapsed>UINT64_MAX-ahead) return false;
    uint64_t index=elapsed+ahead;
    if (index%period || index>(UINT64_MAX-epoch)/tick) return false;
    *result=epoch+index*tick;
    return *result>=now && *result-now>=(uint32_t)period*tick;
}
static bool create_time(const cis_control *c,uint32_t slot,uint64_t now,uint64_t *at)
{
    const cis_parent *p=&c->parent;
    return future_boundary(p->epoch_us,p->slot,p->slot_us,p->period_slots,slot,now,at);
}
static void arm(cis_control *c,const cis_signal *v,uint64_t at)
{
    c->pending=v->params;
    c->effective_us=at;
    c->pending_slot=(v->params.slot_us==125) ? v->slot : 0;
    c->state=CIS_ARMED;
    c->reason=0;
}
cis_result cis_create(cis_control *c,const cis_params *p,uint32_t slot,uint64_t now)
{
    if (!c || !p || !c->is_g || !c->parent_up) return CIS_CTL_INVALID;
    if (c->state!=CIS_OFF) return CIS_CTL_BUSY;
    uint64_t at;
    if (!profile(c,p) || !create_time(c,slot,now,&at)) return CIS_CTL_INVALID;
    if (!c->port.acquire(c->port.ctx,p)) return CIS_CTL_BUSY;
    c->held=true;
    cis_signal v={.type=CIS_CREATE,.slot=slot,.params=*p};
    cis_result r=transmit(c,&v);
    if (r!=CIS_CTL_OK) { close_link(c); return r; }
    arm(c,&v,at);
    return CIS_CTL_OK;
}
cis_result cis_control_tick(cis_control *c,uint64_t now)
{
    if (!c || !c->parent_up) return CIS_CTL_INVALID;
    if (c->waiting && now>=c->deadline) {
        if (c->state==CIS_CLOSING) close_link(c);
        else c->waiting=0;
        return CIS_CTL_TIMEOUT;
    }
    if ((c->state==CIS_ARMED || c->update_pending) && now>=c->effective_us) {
        c->active=c->pending;
        c->anchor_us=c->effective_us;
        c->anchor_slot=c->pending_slot;
        c->update_pending=false;
        c->state=CIS_ACTIVE;
        c->port.apply(c->port.ctx,&c->active,c->anchor_us,c->anchor_slot);
    }
    return CIS_CTL_OK;
}
static bool same_group(const cis_params *a,const cis_params *b)
{
    return a->set_id==b->set_id && a->group_id==b->group_id;
}
cis_result cis_propose(cis_control *c,uint8_t tag,const cis_params *candidate,uint64_t now)
{
    if (!c || !candidate || tag>=16 || !c->is_g || c->state!=CIS_ACTIVE ||
        !same_group(candidate,&c->active) || candidate->slot_us!=c->active.slot_us ||
        !profile(c,candidate)) return CIS_CTL_INVALID;
    if (c->waiting || c->update_pending) return CIS_CTL_BUSY;
    cis_signal v={.type=CIS_PARAM_REQ,.params=*candidate,.tag=tag};
    cis_result r=transmit(c,&v);
    if (r==CIS_CTL_OK) {
        c->known_tags &= ~(1u<<tag);
        c->tag=tag;
        await_reply(c,CIS_PARAM_REQ,now);
    }
    return r;
}
static cis_signal tagged(const cis_control *c,uint16_t type,uint8_t tag)
{
    cis_signal v={.type=type,.tag=tag};
    v.params.set_id=c->active.set_id;
    v.params.group_id=c->active.group_id;
    return v;
}
static bool known(const cis_control *c,uint8_t tag)
{
    return tag<16 && (c->known_tags & (1u<<tag));
}
cis_result cis_request_update(cis_control *c,uint8_t tag,uint64_t now)
{
    if (!c || c->is_g || c->state!=CIS_ACTIVE || !known(c,tag)) return CIS_CTL_INVALID;
    if (c->waiting || c->update_pending) return CIS_CTL_BUSY;
    cis_signal v=tagged(c,CIS_UPDATE_REQ,tag);
    cis_result r=transmit(c,&v);
    if (r==CIS_CTL_OK) {
        c->tag=tag;
        await_reply(c,CIS_UPDATE_REQ,now);
    }
    return r;
}
static bool update_time(const cis_control *c,const cis_signal *v,uint64_t now,uint64_t *at)
{
    const cis_params *p=&c->active;
    if (!v->offset_slots || !future_boundary(c->anchor_us,c->anchor_slot,p->slot_us,
                          p->group_slots,v->slot,now,at)) return false;
    uint64_t delay=(uint64_t)v->offset_slots*p->slot_us;
    if (UINT64_MAX-*at<delay) return false;
    /* Permit a changed group period, but never start before the final old window ends. */
    uint64_t old_end=(uint64_t)(p->events-1)*p->event_slots*p->slot_us+
                     (p->dir[0].max_offset_slots+p->dir[1].max_offset_slots)*125u;
    if (delay<old_end) return false;
    *at+=delay;
    return true;
}
static void stage_update(cis_control *c,const cis_signal *v,uint64_t at)
{
    c->pending=c->candidates[v->tag];
    c->effective_us=at;
    c->last_old_group_us=at-(uint64_t)v->offset_slots*c->active.slot_us;
    c->pending_slot=c->active.slot_us==125 ? (v->slot+v->offset_slots)&CIS_SLOT_MASK : 0;
    c->update_pending=true;
    c->tag=v->tag;
}
bool cis_group_permitted(const cis_control *c,uint64_t start)
{
    return c && c->state==CIS_ACTIVE && start>=c->anchor_us &&
           (!c->update_pending || start<=c->last_old_group_us);
}
cis_result cis_update(cis_control *c,uint8_t tag,uint32_t slot,uint16_t offset,uint64_t now)
{
    if (!c || !c->is_g || c->state!=CIS_ACTIVE || !known(c,tag)) return CIS_CTL_INVALID;
    if (c->waiting || c->update_pending) return CIS_CTL_BUSY;
    cis_signal v=tagged(c,CIS_UPDATE_IND,tag);
    v.slot=slot; v.offset_slots=offset;
    uint64_t at;
    if (!update_time(c,&v,now,&at)) return CIS_CTL_INVALID;
    cis_result r=transmit(c,&v);
    if (r==CIS_CTL_OK) stage_update(c,&v,at);
    return r;
}
cis_result cis_disconnect(cis_control *c,uint8_t reason,uint64_t now)
{
    if (!c || !c->parent_up || c->state!=CIS_ACTIVE) return CIS_CTL_INVALID;
    cis_signal v={.type=CIS_DISCONNECT,.link_id=c->active.dir[0].link_id,.reason=reason};
    cis_result r=transmit(c,&v);
    if (r==CIS_CTL_OK) {
        c->state=CIS_CLOSING; c->update_pending=false; c->reason=reason;
        await_reply(c,CIS_DISCONNECT,now);
    }
    return r;
}
cis_result cis_disconnect_ack(cis_control *c)
{
    if (!c || c->state!=CIS_CLOSING) return CIS_CTL_STALE;
    close_link(c); return CIS_CTL_OK;
}
static cis_result receive_reject(cis_control *c,const cis_signal *v)
{
    if (!v->reason) return CIS_CTL_STALE;
    if (v->rejected_type==CIS_CREATE && c->is_g &&
        (c->state==CIS_ARMED || c->state==CIS_ACTIVE)) close_link(c);
    else if (v->rejected_type==CIS_UPDATE_IND && c->is_g && c->update_pending) c->update_pending=false;
    else if (c->waiting && c->waiting==v->rejected_type) {
        if (c->state==CIS_CLOSING) c->state=CIS_ACTIVE;
        c->waiting=0;
    } else return CIS_CTL_STALE;
    c->reason=v->reason;
    return CIS_CTL_REJECTED;
}
cis_result cis_control_rx(cis_control *c,const uint8_t *bytes,size_t n,uint64_t now)
{
    if (!c || !c->parent_up || !bytes) return CIS_CTL_INVALID;
    /* Expired transactions cannot be revived by a late response, including
       when the receive notification arrives before the next periodic tick. */
    if (c->waiting && now>=c->deadline) (void)cis_control_tick(c,now);
    cis_signal v;
    if (!cis_wire_decode(bytes,n,&v)) {
        if (n>=2 && bytes[1]==0 && (bytes[0]==CIS_DISCONNECT ||
            (bytes[0]>=CIS_CREATE && bytes[0]<=CIS_UPDATE_IND)))
            return refuse(c,bytes[0],BAD_PARAM);
        return CIS_CTL_INVALID;
    }
    if (v.type==CIS_REJECT) return receive_reject(c,&v);
    if ((c->state==CIS_ARMED || c->update_pending) && now>=c->effective_us)
        (void)cis_control_tick(c,now);
    uint64_t at;
    if (v.type==CIS_CREATE) {
        if (c->is_g || c->state!=CIS_OFF) return refuse(c,v.type,NOT_ALLOWED);
        if (!create_time(c,v.slot,now,&at)) return refuse(c,v.type,LATE);
        if (!profile(c,&v.params)) return refuse(c,v.type,BAD_PARAM);
        if (!c->port.acquire(c->port.ctx,&v.params)) return refuse(c,v.type,NO_RESOURCE);
        c->held=true; arm(c,&v,at);
        return CIS_CTL_OK;
    }
    if (v.type==CIS_DISCONNECT) {
        const cis_params *p=c->state==CIS_ARMED ? &c->pending : &c->active;
        if (c->state==CIS_OFF || (v.link_id!=p->dir[0].link_id && v.link_id!=p->dir[1].link_id))
            return refuse(c,v.type,BAD_LINK);
        c->reason=v.reason;
        close_link(c);
        return CIS_CTL_OK; /* parent-link service supplies link-layer ACK */
    }
    if (c->state!=CIS_ACTIVE || !same_group(&c->active,&v.params))
        return refuse(c,v.type,BAD_LINK);
    if (v.type==CIS_PARAM_REQ || v.type==CIS_PARAM_RSP) {
        v.params.slot_us=c->active.slot_us;
        if (v.type==CIS_PARAM_RSP && (!c->is_g || c->waiting!=CIS_PARAM_REQ || v.tag!=c->tag))
            return CIS_CTL_STALE;
        if (v.type==CIS_PARAM_REQ && (c->is_g || c->waiting || c->update_pending))
            return refuse(c,v.type,BUSY);
        if (!profile(c,&v.params)) return refuse(c,v.type,BAD_PARAM);
        if (v.type==CIS_PARAM_REQ) {
            v.type=CIS_PARAM_RSP;
            cis_result r=transmit(c,&v);
            if (r!=CIS_CTL_OK) return r;
        }
        c->candidates[v.tag]=v.params;
        c->known_tags|=(uint16_t)(1u<<v.tag);
        c->waiting=0;
        return CIS_CTL_OK;
    }
    if (!known(c,v.tag)) return refuse(c,v.type,BAD_PARAM);
    if (v.type==CIS_UPDATE_REQ) {
        if (!c->is_g || c->waiting || c->update_pending) return refuse(c,v.type,BUSY);
        /* Select a reference group with a full old period of lead time. */
        if (now<c->anchor_us) return refuse(c,v.type,LATE);
        uint64_t period=(uint64_t)c->active.group_slots*c->active.slot_us;
        uint64_t cycles=(now-c->anchor_us)/period+2;
        uint32_t ref=(c->anchor_slot+(uint32_t)cycles*c->active.group_slots)&CIS_SLOT_MASK;
        return cis_update(c,v.tag,ref,c->active.group_slots,now);
    }
    if (c->is_g || c->update_pending ||
        (c->waiting && (c->waiting!=CIS_UPDATE_REQ || v.tag!=c->tag)))
        return refuse(c,v.type,BUSY);
    if (!update_time(c,&v,now,&at)) return refuse(c,v.type,LATE);
    stage_update(c,&v,at);
    c->waiting=0;
    return CIS_CTL_OK;
}
