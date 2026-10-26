/**
 * @file fimingw.h
 * @author Daniel Starke
 * @date 2026-10-25
 * @version 2026-10-25
 *
 * Included in front of every C++ source of FreeImage when building it with MinGW.
 *
 * OpenEXR calls `posix_memalign` for SSE2 builds with GCC, which MinGW does
 * not provide. Its only caller (`SimdAlignedBuffer64`) handles unaligned
 * memory by allocating more and releases it with `free`, so `malloc` is a
 * valid replacement.
 */
#ifndef __FIMINGW_H__
#define __FIMINGW_H__

#include <errno.h>
#include <stdlib.h>


static inline int posix_memalign(void ** ptr, size_t alignment, size_t size) {
	(void)alignment;
	*ptr = malloc(size);
	return (*ptr != NULL) ? 0 : ENOMEM;
}


#endif /* __FIMINGW_H__ */
