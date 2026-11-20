#ifndef SLE_SCH_H
#define SLE_SCH_H
#include "sle_bis.h"
/* Reference backend for a single radio. All clients use one table,
   serialized at task level. Target deployments may bind the existing SCH
   through bis_port instead. Windows are half-open, first grant wins. */
#define SLE_SCH_CAPACITY 8
typedef struct {
    const void *owner;
    uint64_t token, start_us, end_us;
} sle_window;
typedef struct { sle_window windows[SLE_SCH_CAPACITY]; } sle_sch;
bool sle_reserve(sle_sch *,const void *owner,uint64_t token,uint64_t start,uint32_t duration);
void sle_release(sle_sch *,const void *owner,uint64_t token);
#endif
