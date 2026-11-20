#include "cis_data.h"
#include <string.h>

bool cis_a3_encode(const cis_a3 *a,uint8_t out[4])
{
    if (!a || !out || a->type>3 || a->send_seq>31 || a->receive_seq>31 ||
        a->length>2047) return false;
    uint32_t v=a->type | (uint32_t)a->empty<<2 | (uint32_t)a->send_seq<<3 |
        (uint32_t)a->receive_seq<<8 | (uint32_t)a->more<<13 |
        (uint32_t)(!a->async_scheduled)<<14 | (uint32_t)a->length<<17;
    for (unsigned i=0;i<4;i++) out[i]=(uint8_t)(v>>(8*i));
    return true;
}

bool cis_a3_decode(const uint8_t in[4],cis_a3 *out)
{
    if (!in || !out) return false;
    uint32_t v=0;
    for (unsigned i=0;i<4;i++) v|=(uint32_t)in[i]<<(8*i);
    if (v&UINT32_C(0xf0018000)) return false;
    *out=(cis_a3){.type=v&3,.empty=(v>>2)&1,.send_seq=(v>>3)&31,
        .receive_seq=(v>>8)&31,.more=(v>>13)&1,.async_scheduled=!(v&(1u<<14)),
        .length=(uint16_t)(v>>17)};
    return true;
}

/* Sec. 6.5.2.4, table 14. The local sequence base stays at zero during
   the first discard_cycles groups, then advances by new_packets per group. */
static uint64_t oldest_group(const iso_config *cfg,uint64_t current)
{
    return current>=cfg->discard_cycles ? current-cfg->discard_cycles+1 : 0;
}
iso_result cis_tx_load(cis_tx *t,const iso_config *cfg,uint64_t group,uint8_t index,
                       const uint8_t *data,size_t len)
{
    if (!t || !iso_config_valid(cfg) || !cfg->linked || index>=cfg->group_period_us/cfg->sdu_period_us ||
        len>cfg->max_sdu || (len && !data)) return ISO_INVALID;
    if (t->active) return ISO_BUSY;
    t->active=true; t->submitted=false; t->part=0;
    t->config=*cfg; t->origin_group=group; t->sdu_index=index; t->sdu_length=len;
    if (len) memcpy(t->sdu,data,len);
    return ISO_OK;
}

iso_result cis_tx_pdu(cis_tx *t,uint64_t group,uint8_t a3[4],uint8_t *wire,size_t cap,size_t *used)
{
    if (!t || !a3 || !used) return ISO_INVALID;
    if (!t->active) return ISO_WAIT;
    if (group<t->origin_group || (t->submitted && group<t->submitted_group)) return ISO_INVALID;
    uint64_t base=oldest_group(&t->config,group);
    if (t->origin_group<base) {
        t->active=t->submitted=false; return ISO_EXPIRED;
    }
    unsigned part_count=iso_fragments(&t->config);
    unsigned ordinal=t->sdu_index*part_count+t->part;
    unsigned seq=(unsigned)(t->origin_group-base)*iso_new_packets(&t->config)+ordinal;
    iso_result rc=iso_encode(&t->config,t->origin_group,t->sdu_index,t->part,
                             t->sdu,t->sdu_length,wire,cap,used);
    if (rc!=ISO_OK) return rc;
    cis_a3 header={.send_seq=(uint8_t)seq,.more=ordinal+1<iso_new_packets(&t->config),
                   .length=(uint16_t)*used};
    if (!cis_a3_encode(&header,a3)) return ISO_INVALID;
    t->sent_seq=(uint8_t)seq; t->submitted_group=group; t->submitted=true;
    return ISO_OK;
}

iso_result cis_rx_pdu(iso_rx *r,const iso_config *cfg,uint64_t group,uint64_t anchor_us,
                      const uint8_t a3[4],const uint8_t *wire,size_t used,
                      bool header_ok,bool data_ok,uint8_t ack[4])
{
    if (!r || !ack || !iso_config_valid(cfg) || !cfg->linked) return ISO_INVALID;
    if (!header_ok) return ISO_CRC;
    cis_a3 header;
    if (!cis_a3_decode(a3,&header) || header.empty || header.type!=0 ||
        header.length!=used) return ISO_INVALID;
    cis_a3 reply={.empty=true,.receive_seq=header.send_seq};
    cis_a3_encode(&reply,ack);
    if (!data_ok) return ISO_CRC;
    unsigned total=iso_new_packets(cfg);
    uint64_t base=oldest_group(cfg,group);
    unsigned delta=header.send_seq/total;
    if (delta>group-base) return ISO_INVALID;
    uint64_t origin=base+delta;
    uint64_t back=(group-origin)*cfg->group_period_us;
    if (anchor_us<back) return ISO_INVALID;
    iso_result rc=iso_ingest(r,cfg,origin,anchor_us-back,header.send_seq%total,wire,used);
    if (rc==ISO_OK || rc==ISO_DUPLICATE) {
        reply.receive_seq=(header.send_seq+1u)&31u;
        cis_a3_encode(&reply,ack);
    }
    return rc;
}

iso_result cis_tx_feedback(cis_tx *t,uint64_t group,const uint8_t ack[4],bool crc_ok)
{
    if (!t || !t->active || !t->submitted) return ISO_WAIT;
    if (group!=t->submitted_group) return ISO_INVALID;
    if (!crc_ok) return ISO_CRC;
    cis_a3 response;
    if (!cis_a3_decode(ack,&response) || !response.empty || response.length || response.type)
        return ISO_INVALID;
    if (response.receive_seq==t->sent_seq) return ISO_RETRY;
    if (response.receive_seq!=((t->sent_seq+1u)&31u)) return ISO_INVALID;
    t->submitted=false;
    if (++t->part==iso_fragments(&t->config)) t->active=false;
    return ISO_OK;
}
