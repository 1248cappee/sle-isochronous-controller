#include "sle_adv.h"
#include <string.h>
bool bis_aux_encode(uint8_t channel,uint16_t offset,bool ten_slots,uint8_t aux[4])
{
    if(!aux || channel>78 || !offset || offset>4095) return false;
    aux[0]=channel; aux[1]=(uint8_t)offset; aux[2]=(uint8_t)(offset>>8);
    aux[3]=ten_slots?128:0;
    return true;
}
size_t bis_adv_encode(const bis_config *cfg,const uint8_t *m,size_t n,uint8_t *tlv,size_t cap)
{
    if(!tlv || !m || n<15 || n>255 || n!=14u+m[13] || cap<n+4+BIS_LINK_BYTES)
        return 0;
    uint8_t link[BIS_LINK_BYTES];
    if(!bis_link_encode(cfg,link)) return 0;
    tlv[0]=5; tlv[1]=(uint8_t)n;
    memcpy(tlv+2,m,n);
    tlv[2+n]=6; tlv[3+n]=BIS_LINK_BYTES;
    memcpy(tlv+4+n,link,BIS_LINK_BYTES);
    return 4+n+BIS_LINK_BYTES;
}
size_t bis_advertise_at(const bis_controller *c,uint64_t us,const uint8_t *m,size_t n,
                        uint8_t *out,size_t cap)
{
    if(c->state!=BIS_TX || us>=c->stop_at) return 0;
    bis_config cfg=c->cfg;
    uint64_t group=c->anchor;
    uint64_t period=cfg.group_slots*BIS_SLOT_US;
    if(us>=group){
        uint64_t cycles=(us-group)/period+1;
        if(cycles>(UINT64_MAX-group)/period) return 0;
        group+=cycles*period;
        cfg.base_slot=(cfg.base_slot+(uint32_t)cycles*cfg.group_slots)&BIS_SLOT_MASK;
    }
    if(group>=c->stop_at || group-us>0xffffff) return 0;
    cfg.offset_us=(uint32_t)(group-us);
    return bis_adv_encode(&cfg,m,n,out,cap);
}
bis_result bis_disconnect_pdu(bis_controller *c,const uint8_t *p,size_t n,
                              uint64_t now,uint8_t *reason)
{
    if(!p || n!=5) return BIS_INVALID;
    uint32_t slot=p[1]|((uint32_t)p[2]<<8)|((uint32_t)p[3]<<16)|((uint32_t)p[4]<<24);
    bis_result rc=bis_disconnect(c,slot,now);
    if(reason && (rc==BIS_OK || rc==BIS_STOPPED)) *reason=p[0];
    return rc;
}
