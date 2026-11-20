#include "sle_bis.h"
#include <string.h>

/* LSB first within fields. Accesses never depend on host ABI or alignment. */
static uint32_t bits(const uint8_t *p, unsigned start, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i)
        v |= (uint32_t)((p[(start+i)/8] >> ((start+i)%8)) & 1u) << i;
    return v;
}
static void put(uint8_t *p, unsigned start, unsigned width, uint32_t v)
{
    for (unsigned i = 0; i < width; ++i)
        p[(start+i)/8] |= (uint8_t)(((v >> i) & 1u) << ((start+i)%8));
}
bool bis_config_valid(const bis_config *c)
{
    if (!c || c->base_slot > BIS_SLOT_MASK || c->offset_us > 0xffffff ||
        c->link_base > 0xffffff || c->anchor_delay_us > 0xffffff ||
        c->reference_delay_us > 0xffffff || !c->group_slots || !c->event_slots ||
        !c->events || !c->max_sdu || c->max_sdu > 4095 || !c->max_pdu ||
        c->max_pdu > BIS_MAX_PDU || !c->sdu_period_us || c->sdu_period_us > 0xfffff ||
        c->pilot > 3 || c->new_packets < 1 || c->new_packets > 16 ||
        (uint32_t)(c->events-1)*c->event_slots >= c->group_slots) return false;
    uint32_t period = c->group_slots * BIS_SLOT_US;
    uint32_t fragments = (c->max_sdu + c->max_pdu-1u)/c->max_pdu;
    if (period % c->sdu_period_us || (uint64_t)fragments * (period/c->sdu_period_us) != c->new_packets)
        return false;
    for (unsigned channel=0; channel<76; ++channel)
        if (c->map[channel/8] & (1u << (channel%8))) return true;
    return false;
}
bool bis_link_encode(const bis_config *c, uint8_t p[BIS_LINK_BYTES])
{
    if (!p || !bis_config_valid(c)) return false;
    memset(p,0,BIS_LINK_BYTES);
    p[0]=1; put(p,8,32,c->base_slot); put(p,40,24,c->offset_us);
    p[8]=c->set_id; p[9]=1;
    put(p,88,16,c->group_slots); put(p,104,16,c->event_slots); p[15]=c->events;
    put(p,128,24,c->anchor_delay_us); put(p,152,24,c->reference_delay_us);
    put(p,176,24,c->link_base); put(p,206,2,c->pilot);
    put(p,208,12,c->max_sdu); put(p,220,20,c->sdu_period_us);
    put(p,240,11,c->max_pdu); put(p,251,4,c->new_packets-1);
    put(p,255,1,c->crc32); put(p,256,32,c->crc_base); memcpy(p+36,c->map,10);
    return true;
}
bool bis_link_decode(const uint8_t *p,size_t len,bis_config *out)
{
    if (!out || !p || len!=BIS_LINK_BYTES || p[0]!=1 || p[9]!=1 ||
        (p[25]&63)!=0) return false;
    bis_config c={0};
    c.base_slot=bits(p,8,32); c.offset_us=bits(p,40,24); c.set_id=p[8];
    c.group_slots=bits(p,88,16); c.event_slots=bits(p,104,16); c.events=p[15];
    c.anchor_delay_us=bits(p,128,24); c.reference_delay_us=bits(p,152,24);
    c.link_base=bits(p,176,24); c.pilot=p[25]>>6;
    c.max_sdu=bits(p,208,12); c.sdu_period_us=bits(p,220,20);
    c.max_pdu=bits(p,240,11); c.new_packets=1+bits(p,251,4);
    c.crc32=bits(p,255,1); c.crc_base=bits(p,256,32); memcpy(c.map,p+36,10);
    if (!bis_config_valid(&c)) return false;
    *out=c;
    return true;
}
static unsigned reverse8(unsigned x)
{
    unsigned r=0;
    for(unsigned i=0;i<8;i++) r=(r<<1)|((x>>i)&1);
    return r;
}
uint8_t bis_channel(const bis_config *c,uint16_t slot)
{
    uint32_t id=c->link_base ^ 1u;
    uint16_t key=(uint16_t)(id^(id>>16)), r=slot^key;
    for(unsigned i=0;i<3;i++){
        unsigned rev=(reverse8(r>>8)<<8)|reverse8(r);
        r=(uint16_t)(17u*rev+key);
    }
    r^=key;
    uint8_t ch=r%76;
    if(c->map[ch/8]&(1u<<(ch%8))) return ch;
    uint8_t available[76], count=0;
    for(ch=0;ch<76;ch++)
        if(c->map[ch/8]&(1u<<(ch%8))) available[count++]=ch;
    return count ? available[((uint32_t)r*count)>>16] : UINT8_MAX;
}
bool bis_a6_encode(const bis_a6 *a,uint8_t p[4])
{
    if (!a || !p || a->type>3 || a->seq>31 || a->group>7 || a->length>2047) return false;
    memset(p,0,4);
    put(p,0,2,a->type); put(p,2,5,a->seq); put(p,7,3,a->group);
    put(p,10,1,a->last); put(p,11,1,a->skip_management); put(p,17,11,a->length);
    return true;
}
bool bis_a6_decode(const uint8_t p[4],bis_a6 *a)
{
    if (!p || !a || bits(p,12,5) || bits(p,28,4)) return false;
    *a=(bis_a6){bits(p,0,2),bits(p,2,5),bits(p,7,3),bits(p,10,1),bits(p,11,1),bits(p,17,11)};
    return true;
}
