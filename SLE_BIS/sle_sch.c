#include "sle_sch.h"

bool sle_reserve(sle_sch *s,const void *owner,uint64_t token,uint64_t start,uint32_t duration)
{
    if(!s || !owner || !duration || UINT64_MAX-start<duration) return false;
    uint64_t end=start+duration;
    sle_window *free_window=NULL;
    for(unsigned i=0;i<SLE_SCH_CAPACITY;i++){
        sle_window *w=&s->windows[i];
        if(!w->owner){
            free_window=w;
            continue;
        }
        if(w->owner==owner && w->token==token) return false;
        if(start<w->end_us && w->start_us<end) return false;
    }
    if(!free_window) return false;
    *free_window=(sle_window){owner,token,start,end};
    return true;
}
void sle_release(sle_sch *s,const void *owner,uint64_t token)
{
    for(unsigned i=0;i<SLE_SCH_CAPACITY;i++){
        sle_window *w=&s->windows[i];
        if(w->owner==owner && w->token==token) w->owner=NULL;
    }
}
