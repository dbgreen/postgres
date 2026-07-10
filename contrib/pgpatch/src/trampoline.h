/*-------------------------------------------------------------------------
 *
 * trampoline.h
 *		Architecture-specific inline code patching for pgpatch.
 *
 * These routines overwrite the prologue of a live function with a jump to a
 * replacement ("hook"), and restore the original bytes on unpatch.  They are
 * pure: no Janet, no Postgres — just memory protection and instruction-cache
 * maintenance.  The current implementation targets macOS on arm64.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPATCH_TRAMPOLINE_H
#define PGPATCH_TRAMPOLINE_H

#include <stdbool.h>
#include <stdint.h>

/* Bytes of the target prologue we overwrite (arm64 absolute-jump stub). */
#define PGPATCH_STUB_LEN 16

/*
 * Overwrite `target`'s first PGPATCH_STUB_LEN bytes with a jump to `hook`,
 * first copying the original bytes into `saved` (must be >= PGPATCH_STUB_LEN).
 * Returns true on success.
 */
extern bool pgpatch_arch_patch(void *target, void *hook, uint8_t *saved);

/*
 * Restore the bytes previously captured in `saved` to `target`.
 */
extern bool pgpatch_arch_unpatch(void *target, const uint8_t *saved);

#endif							/* PGPATCH_TRAMPOLINE_H */
