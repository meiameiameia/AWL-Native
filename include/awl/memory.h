#ifndef AWL_MEMORY_H
#define AWL_MEMORY_H

#include "awl/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Memory management — replacement for Dolphin OS memory functions */

/* Initialize the memory system */
void  awl_memory_init(void);
void  awl_memory_shutdown(void);

/* Allocate memory from the main heap */
void* awl_malloc(u32 size);
void  awl_free(void* ptr);

/* Allocate aligned memory */
void* awl_memalign(u32 alignment, u32 size);
void  awl_aligned_free(void* ptr);

/* Get total free memory */
u32   awl_memory_free(void);

/* Replacement for OSGetArenaLo / OSGetArenaHi */
void* awl_arena_lo(void);
void* awl_arena_hi(void);
void  awl_set_arena_lo(void* lo);
void  awl_set_arena_hi(void* hi);

#ifdef __cplusplus
}
#endif

#endif /* AWL_MEMORY_H */
