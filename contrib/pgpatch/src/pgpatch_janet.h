/*-------------------------------------------------------------------------
 *
 * pgpatch_janet.h
 *		Include janet.h with the Postgres/Janet macro conflicts resolved.
 *
 * Postgres and Janet both define Min/Max/Abs.  Translation units that need
 * Janet include this (after postgres.h) instead of janet.h directly, so the
 * Postgres definitions are dropped and the Janet header compiles cleanly.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPATCH_JANET_H
#define PGPATCH_JANET_H

#ifdef Max
#undef Max
#endif
#ifdef Min
#undef Min
#endif
#ifdef Abs
#undef Abs
#endif

#include "janet.h"

#endif							/* PGPATCH_JANET_H */
