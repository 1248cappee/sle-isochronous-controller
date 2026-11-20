#ifndef SLE_ADV_H
#define SLE_ADV_H
#include "sle_bis.h"
/* The external advertising service wraps aux/TLVs in the common broadcast
   header, timestamps frame start and supplies CRC-qualified RX indications. */
bool bis_aux_encode(uint8_t channel,uint16_t offset,bool ten_slots,uint8_t aux[4]);
size_t bis_adv_encode(const bis_config *,const uint8_t *management,size_t management_len,
                      uint8_t *tlv,size_t capacity);
/* Re-announce a running TX stream from a new extension frame timestamp. */
size_t bis_advertise_at(const bis_controller *,uint64_t ext_start_us,
                        const uint8_t *,size_t,uint8_t *,size_t);
/* 0x002A payload after control dispatch. Returns reason only on acceptance. */
bis_result bis_disconnect_pdu(bis_controller *,const uint8_t *,size_t,uint64_t,uint8_t *);
#endif
