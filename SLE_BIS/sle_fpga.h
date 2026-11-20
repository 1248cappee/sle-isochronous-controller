#ifndef SLE_FPGA_H
#define SLE_FPGA_H
#include "sle_bis.h"
/* BSP functions implement the actual target register layout and DMA barriers.
   stage prepares an unpublished descriptor and copies payload. publish
   transfers ownership atomically. stage failure must leave queue unchanged. */
typedef enum { BIS_IRQ_PACKET, BIS_IRQ_END } bis_irq_kind;
typedef struct {
    bis_irq_kind kind;
    uint64_t token;
    bool header_ok, data_ok;
    bis_a6 a6;
    const uint8_t *bytes;
    size_t length;
} bis_irq;
typedef struct {
    void *ctx;
    bool (*stage)(void *,const bis_event *,const bis_a6 *,const uint8_t *);
    void (*flush_and_publish)(void *,uint64_t);
    void (*abort_and_drain)(void *,uint64_t);
    bool (*peek_acquire)(void *,bis_irq *);
    void (*ack)(void *,const bis_irq *);
} bis_fpga;
bool bis_fpga_submit(bis_fpga *,const bis_event *,const bis_a6 *,const uint8_t *);
void bis_fpga_cancel(bis_fpga *,uint64_t);
bis_result bis_fpga_service(bis_fpga *,bis_controller *);
#endif
