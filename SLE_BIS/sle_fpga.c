#include "sle_fpga.h"
bool bis_fpga_submit(bis_fpga *fp,const bis_event *e,const bis_a6 *a,const uint8_t *data)
{
    if(!fp->stage(fp->ctx,e,a,data)) return false;
    fp->flush_and_publish(fp->ctx,e->token);
    return true;
}
void bis_fpga_cancel(bis_fpga *fp,uint64_t token)
{
    fp->abort_and_drain(fp->ctx,token);
}
bis_result bis_fpga_service(bis_fpga *fp,bis_controller *c)
{
    bis_irq irq;
    if(!fp->peek_acquire(fp->ctx,&irq)) return BIS_IDLE;
    bis_result rc=BIS_INVALID;
    if(irq.kind==BIS_IRQ_PACKET)
        rc=bis_receive(c,irq.token,irq.header_ok,irq.data_ok,irq.a6,irq.bytes,irq.length);
    else if(irq.kind==BIS_IRQ_END)
        rc=bis_done(c,irq.token);
    fp->ack(fp->ctx,&irq);
    return rc;
}
