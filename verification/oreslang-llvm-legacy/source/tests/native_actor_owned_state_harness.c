#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_read(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_add(OresActorI64Heap*,uint64_t,uint64_t,int64_t);
extern int64_t oreslang_actor_Counter_total_value(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_plus(OresActorI64Heap*,uint64_t,uint64_t,int64_t);
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"ACTOR_OWNED_STATE_FAILURE line %d\n",__LINE__); return 2; } } while(0)
int main(void) {
 OresActorI64Heap* heap=ores_actor_i64_heap_open(29,3,3);
 OresActorTurn a=ores_actor_i64_heap_enter(heap,29,3);
 oreslang_actor_Counter_init(heap,a.domain,a.epoch);
 CHECK(oreslang_actor_Counter_read(heap,a.domain,a.epoch)==42);
 CHECK(oreslang_actor_Counter_total_value(heap,a.domain,a.epoch)==0);
 CHECK(oreslang_actor_Counter_plus(heap,a.domain,a.epoch,1)==43);
 CHECK(oreslang_actor_Counter_add(heap,a.domain,a.epoch,1)==43);
 CHECK(oreslang_actor_Counter_total_value(heap,a.domain,a.epoch)==43);
 ores_actor_i64_heap_leave(heap,a);
 OresActorTurn b=ores_actor_i64_heap_enter(heap,29,3);
 CHECK(b.epoch!=a.epoch);
 CHECK(oreslang_actor_Counter_total_value(heap,b.domain,b.epoch)==43);
 CHECK(oreslang_actor_Counter_add(heap,b.domain,b.epoch,4)==89);
 CHECK(oreslang_actor_Counter_total_value(heap,b.domain,b.epoch)==89);
 CHECK(oreslang_actor_Counter_read(heap,b.domain,b.epoch)==42);
 ores_actor_i64_heap_leave(heap,b);
 ores_actor_i64_heap_retire(heap);
 CHECK(ores_actor_i64_heap_try_destroy(heap)==1);
 puts("ACTOR_OWNED_STATE_PASS");
 return 43;
}
