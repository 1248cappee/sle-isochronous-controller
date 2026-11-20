#include "sle_isoal.h"
#include <string.h>

unsigned iso_fragments(const iso_config *c)
{
    return c && c->max_pdu ? (c->max_sdu + c->max_pdu - 1u) / c->max_pdu : 0;
}

unsigned iso_new_packets(const iso_config *c)
{
    if (!c || !c->sdu_period_us) return 0;
    uint64_t n = (uint64_t)iso_fragments(c) * (c->group_period_us / c->sdu_period_us);
    return n <= UINT32_MAX ? (unsigned)n : 0;
}

static size_t prefix(const iso_config *c)
{
    return (c->linked ? 5u : 3u) + (c->multiplexed ? 2u : 0u);
}

bool iso_config_valid(const iso_config *c)
{
    if (!c || !c->max_sdu || c->max_sdu > ISO_SDU_LIMIT || !c->max_pdu ||
        c->max_pdu > 2047 || !c->sdu_period_us || !c->group_period_us ||
        c->group_period_us % c->sdu_period_us || !c->discard_cycles)
        return false;
    unsigned n = iso_new_packets(c);
    size_t largest = c->max_sdu < c->max_pdu ? c->max_sdu : c->max_pdu;
    return n && n <= (c->linked ? 15u : 16u) && prefix(c) + largest <= 2047 &&
           (!c->linked || (uint32_t)c->discard_cycles * n <= 32u);
}

static unsigned segment_type(unsigned part, unsigned count)
{
    if (count == 1) return 0;
    if (part == 0) return 1;
    return part+1 == count ? 3 : 2;
}

iso_result iso_encode(const iso_config *c,uint64_t group,uint8_t index,uint8_t part,
                      const uint8_t *sdu,size_t len,uint8_t *w,size_t cap,size_t *used)
{
    if (!iso_config_valid(c) || !used || len > c->max_sdu || (len && !sdu) ||
        index >= c->group_period_us/c->sdu_period_us || part >= iso_fragments(c))
        return ISO_INVALID;
    if (!len) { *used=0; return ISO_OK; } /* Sec. 6.5.3.2: empty SDU on air. */
    size_t offset = (size_t)part * c->max_pdu;
    size_t n = offset < len ? len-offset : 0;
    if (n > c->max_pdu) n = c->max_pdu;
    if (!w || cap < prefix(c)+n) return ISO_CAPACITY;
    *used=prefix(c)+n;
    if (c->multiplexed) { *w++=0xfe; *w++=0xff; }
    if (c->linked) {
        *w++=(uint8_t)(index*iso_fragments(c)+part);
        *w++=(uint8_t)group;
    }
    unsigned word = (unsigned)n<<3 | segment_type(part,iso_fragments(c))<<1;
    *w++=(uint8_t)word; *w++=(uint8_t)(word>>8); *w++=index;
    if (n) memcpy(w,sdu+offset,n);
    return ISO_OK;
}

iso_result iso_decode(const iso_config *c,const uint8_t *w,size_t len,iso_fragment *out)
{
    if (!iso_config_valid(c) || !out || !w || len<prefix(c)) return ISO_INVALID;
    size_t n=len-prefix(c);
    if (n>c->max_pdu) return ISO_INVALID;
    if (c->multiplexed) {
        if (w[0]!=0xfe || w[1]!=0xff) return ISO_INVALID;
        w+=2;
    }
    iso_fragment f={0};
    if (c->linked) {
        f.pdu_index=*w++; f.group_low=*w++;
        if (f.pdu_index>=iso_new_packets(c)) return ISO_INVALID;
    }
    unsigned word=w[0]|((unsigned)w[1]<<8);
    if ((word&0xc001u) || (word>>3)!=n || w[2]>=c->group_period_us/c->sdu_period_us)
        return ISO_INVALID;
    f.segment=(uint8_t)((word>>1)&3); f.sdu_index=w[2];
    f.data=w+3; f.length=n;
    *out=f;
    return ISO_OK;
}

iso_result iso_ingest(iso_rx *r,const iso_config *c,uint64_t group,uint64_t anchor,
                      unsigned ordinal,const uint8_t *wire,size_t len)
{
    if (!r || !iso_config_valid(c)) return ISO_INVALID;
    unsigned count=iso_fragments(c), total=iso_new_packets(c);
    if (ordinal>=total || group>(UINT64_MAX-ordinal)/total) return ISO_INVALID;
    unsigned index=ordinal/count, part=ordinal%count;
    iso_fragment frame={0};
    if (len) {
        if (iso_decode(c,wire,len,&frame)!=ISO_OK || frame.sdu_index!=index ||
            frame.segment!=segment_type(part,count) ||
            (c->linked && (frame.pdu_index!=ordinal || frame.group_low!=(uint8_t)group)))
            return ISO_INVALID;
    }
    uint64_t payload=group*total+ordinal;
    if (r->accepted && payload<=r->last_payload) return ISO_DUPLICATE;
    uint64_t delay = c->anchor_delay_us + (uint64_t)(c->discard_cycles-1)*c->group_period_us +
                     (uint64_t)index*c->sdu_period_us;
    if (anchor>UINT64_MAX-delay) return ISO_INVALID;
    uint64_t due=anchor+delay;
    iso_rx_slot *slot=NULL,*free_slot=NULL;
    for (unsigned i=0;i<ISO_RX_SLOTS;i++) {
        iso_rx_slot *s=&r->pending[i];
        if (!s->used) free_slot=s;
        else if (!s->retired_epoch && s->group==group && s->index==index) slot=s;
    }
    if (!slot && part) return ISO_GAP;
    if (!slot) {
        if (!free_slot) return ISO_BUSY;
        slot=free_slot;
        slot->group=group; slot->due=due; slot->index=(uint8_t)index;
        slot->length=0; slot->next_fragment=0; slot->complete=false; slot->used=true;
        slot->retired_epoch=false;
    }
    if (slot->due!=due || part!=slot->next_fragment) return ISO_GAP;
    if (frame.length > (size_t)c->max_sdu-slot->length) return ISO_INVALID;
    if (frame.length) memcpy(slot->data+slot->length,frame.data,frame.length);
    slot->length+=(uint16_t)frame.length;
    slot->complete=(++slot->next_fragment==count);
    r->accepted=true; r->last_payload=payload;
    return ISO_OK;
}

size_t iso_rx_reconfigure(iso_rx *r)
{
    size_t discarded=0;
    for (unsigned i=0;i<ISO_RX_SLOTS;++i) {
        iso_rx_slot *slot=&r->pending[i];
        if (!slot->used) continue;
        if (slot->complete) slot->retired_epoch=true;
        else { slot->used=false; ++discarded; }
    }
    r->accepted=false;
    return discarded;
}

size_t iso_expire(iso_rx *r,uint64_t now)
{
    size_t count=0;
    if (r) for (unsigned i=0;i<ISO_RX_SLOTS;i++) {
        iso_rx_slot *s=&r->pending[i];
        if (s->used && !s->complete && now>=s->due) { s->used=false; count++; }
    }
    return count;
}

iso_result iso_deliver(iso_rx *r,uint64_t now,uint8_t *out,size_t cap,size_t *used)
{
    if (!r || !used) return ISO_INVALID;
    iso_rx_slot *first=NULL;
    for (unsigned i=0;i<ISO_RX_SLOTS;i++) {
        iso_rx_slot *s=&r->pending[i];
        if (s->used && s->complete && (!first || s->due<first->due)) first=s;
    }
    if (!first || first->due>now) return ISO_WAIT;
    if (first->length>cap || (first->length && !out)) return ISO_CAPACITY;
    if (first->length) memcpy(out,first->data,first->length);
    *used=first->length; first->used=false;
    return ISO_OK;
}
