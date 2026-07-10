/*-------------------------------------------------------------------------
 *
 * repl.h
 *		Per-backend Janet REPL served over a Unix-domain socket.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPATCH_REPL_H
#define PGPATCH_REPL_H

/*
 * Start the REPL listener for this backend (idempotent).  Must be called on
 * the main thread (it registers an on_proc_exit cleanup and reads MyProcPid).
 */
extern void pgpatch_repl_start(void);

/* The Unix-domain socket path for this backend, or "" if not started. */
extern const char *pgpatch_repl_socket_path(void);

#endif							/* PGPATCH_REPL_H */
