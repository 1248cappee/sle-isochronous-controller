#ifndef CIS_DATA_H
#define CIS_DATA_H
#include "sle_isoal.h"
typedef struct {
    uint8_t type, send_seq, receive_seq;
    bool empty, more, async_scheduled;
    uint16_t length;
} cis_a3;
typedef struct {
    iso_config config;
    uint64_t origin_group, submitted_group;
    bool active, submitted;
    uint8_t sdu_index, part, sent_seq;
    size_t sdu_length;
    uint8_t sdu[ISO_SDU_LIMIT];
} cis_tx;
bool cis_a3_encode(const cis_a3 *,uint8_t out[4]);
bool cis_a3_decode(const uint8_t in[4],cis_a3 *);
iso_result cis_tx_load(cis_tx *,const iso_config *,uint64_t group,uint8_t index,const uint8_t *,size_t);
iso_result cis_tx_pdu(cis_tx *,uint64_t group,uint8_t a3[4],uint8_t *wire,size_t cap,size_t *used);
/* On ISO_CRC from a failed header, no reply is produced. With a valid header
   and bad data, ack is a NACK. Radio activity tokens are checked by the driver. */
iso_result cis_rx_pdu(iso_rx *,const iso_config *,uint64_t group,uint64_t anchor_us,
                      const uint8_t a3[4],const uint8_t *wire,size_t used,
                      bool header_crc_ok,bool data_crc_ok,uint8_t ack[4]);
iso_result cis_tx_feedback(cis_tx *,uint64_t group,const uint8_t ack[4],bool crc_ok);
#endif
