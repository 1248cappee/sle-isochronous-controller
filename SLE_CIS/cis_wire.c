#include "cis_control.h"
#include <string.h>

/* Fields are listed once in transmission order. A scratch record keeps
   caller storage unchanged when lengths, reserved bits or values are invalid. */
typedef struct {
    uint8_t *bytes;
    unsigned bit;
    bool read, ok;
} bit_stream;

static uint32_t field(bit_stream *s, unsigned width, uint32_t value)
{
    if (!s->read && width < 32 && value >= (UINT32_C(1) << width)) s->ok=false;
    if (s->read) value=0;
    for (unsigned i=0; i<width; ++i, ++s->bit) {
        unsigned byte=s->bit/8, bit=s->bit%8;
        if (s->read) value |= (uint32_t)((s->bytes[byte] >> bit) & 1u) << i;
        else s->bytes[byte] |= (uint8_t)(((value >> i) & 1u) << bit);
    }
    return value;
}
static void reserved(bit_stream *s, unsigned width)
{
    if (field(s,width,0)) s->ok=false;
}
static uint32_t biased(bit_stream *s,unsigned width,uint32_t value,unsigned bias)
{
    return field(s,width,value-bias)+bias;
}
static unsigned body_size(uint16_t type)
{
    switch (type) {
    case CIS_CREATE: return 56;
    case CIS_PARAM_REQ:
    case CIS_PARAM_RSP: return 52;
    case CIS_UPDATE_REQ:
    case CIS_REJECT: return 3;
    case CIS_UPDATE_IND: return 9;
    case CIS_DISCONNECT: return 4;
    default: return 0;
    }
}
static void params_layout(bit_stream *s,cis_signal *v)
{
    cis_params *p=&v->params;
    bool create=v->type==CIS_CREATE, response=v->type==CIS_PARAM_RSP;
    p->set_id=field(s,8,p->set_id);
    p->group_id=field(s,8,p->group_id);
    if (create) {
        v->slot=field(s,32,v->slot);
        if (v->slot>CIS_SLOT_MASK) s->ok=false;
    }
    p->group_slots=field(s,16,p->group_slots);
    p->event_slots=field(s,16,p->event_slots);
    p->intra_us=field(s,16,p->intra_us);
    p->inter_us=field(s,16,p->inter_us);
    p->events=field(s,8,p->events);
    p->anchor_delay_us=field(s,24,p->anchor_delay_us);
    p->reference_delay_us=field(s,24,p->reference_delay_us);
    if (create) {
        if (!s->read && (!p->slot_us || p->slot_us%25 || p->slot_us>125)) s->ok=false;
        unsigned code=field(s,3,(unsigned)(p->slot_us/25)-1);
        if (code>4) s->ok=false;
        p->slot_us=(uint8_t)((code+1)*25);
        reserved(s,2);
    } else {
        v->tag=field(s,4,v->tag);
        reserved(s,1);
    }
    /* This field describes the receiver of the control message. G receives
       the response, whereas T receives the creation and parameter request. */
    bool receiver_second=field(s,1,response ? p->t_sends_first : !p->t_sends_first);
    p->t_sends_first=response ? receiver_second : !receiver_second;
    /* Directional fields alternate on the wire rather than two whole records. */
#define PAIR(name,width) do { for (int j=0;j<2;j++) p->dir[j].name=field(s,width,p->dir[j].name); } while(0)
    PAIR(aperiodic,1);
    PAIR(link_id,24);
    PAIR(frame,4);
    PAIR(bandwidth,2);
    PAIR(pilot,2);
    PAIR(max_sdu,12);
    PAIR(sdu_period_us,20);
    PAIR(max_pdu,11);
    for (int j=0;j<2;j++) {
        cis_direction *d=&p->dir[j];
        d->max_offset_slots=biased(s,9,d->max_offset_slots,response);
        if (!d->max_offset_slots) s->ok=false;
    }
    for (int j=0;j<2;j++) {
        cis_direction *d=&p->dir[j];
        d->new_packets=biased(s,4,d->new_packets,response);
        if (!d->new_packets) s->ok=false;
    }
    PAIR(crc_seed,32);
    for (int j=0;j<2;j++) {
        cis_direction *d=&p->dir[j];
        d->discard_cycles=biased(s,8,d->discard_cycles,!create);
        if (!d->discard_cycles) s->ok=false;
    }
    PAIR(crc32,1);
#undef PAIR
    p->dir[0].feedback=field(s,6,p->dir[0].feedback);
    p->dir[1].feedback=field(s,3,p->dir[1].feedback);
    reserved(s,5);
}
static bool layout(uint8_t *body, cis_signal *v, bool read)
{
    bit_stream s={body,0,read,true};
    switch (v->type) {
    case CIS_CREATE:
    case CIS_PARAM_REQ:
    case CIS_PARAM_RSP:
        params_layout(&s,v);
        break;
    case CIS_UPDATE_REQ:
    case CIS_UPDATE_IND:
        v->tag=field(&s,4,v->tag);
        reserved(&s,4);
        v->params.set_id=field(&s,8,v->params.set_id);
        v->params.group_id=field(&s,8,v->params.group_id);
        if (v->type==CIS_UPDATE_IND) {
            v->slot=field(&s,32,v->slot);
            v->offset_slots=field(&s,16,v->offset_slots);
            if (v->slot>CIS_SLOT_MASK) s.ok=false;
        }
        break;
    case CIS_REJECT:
        v->rejected_type=field(&s,16,v->rejected_type);
        v->reason=field(&s,8,v->reason);
        break;
    case CIS_DISCONNECT:
        v->link_id=field(&s,24,v->link_id);
        v->reason=field(&s,8,v->reason);
        break;
    default: return false;
    }
    return s.ok && s.bit==8*body_size(v->type);
}
size_t cis_wire_encode(const cis_signal *v,uint8_t *out,size_t cap)
{
    if (!v || !out) return 0;
    unsigned n=body_size(v->type);
    if (!n || cap<n+3) return 0;
    uint8_t tmp[CIS_CTRL_MAX_FRAME]={0};
    cis_signal copy=*v;
    if (!layout(tmp+3,&copy,false)) return 0;
    tmp[0]=(uint8_t)v->type;
    tmp[1]=(uint8_t)(v->type>>8);
    tmp[2]=(uint8_t)n;
    memcpy(out,tmp,n+3);
    return n+3;
}
bool cis_wire_decode(const uint8_t *src,size_t len,cis_signal *out)
{
    if (!src || !out || len<3) return false;
    cis_signal decoded={0};
    decoded.type=(uint16_t)(src[0] | ((uint16_t)src[1]<<8));
    unsigned n=body_size(decoded.type);
    if (!n || len!=n+3 || src[2]!=n) return false;
    uint8_t body[CIS_CTRL_MAX_FRAME];
    memcpy(body,src+3,n);
    if (!layout(body,&decoded,true)) return false;
    *out=decoded;
    return true;
}
