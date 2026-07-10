/*-------------------------------------------------------------------------
 *
 * repl.c
 *		Per-backend Janet REPL served over a Unix-domain socket.
 *
 * A dedicated pthread owns a listening socket at /tmp/pgpatch/<pid>.sock and
 * runs a line-oriented Janet REPL with continuation support.  The thread never
 * calls Postgres internals: it only uses libc, pthreads, sockets, and the
 * thread-safe Janet entry points in janet_embed.c (which serialize VM access
 * with the backend's main thread via a mutex).
 *
 * Only the socket-file cleanup runs on the main thread, via on_proc_exit.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/ipc.h"

#include "pgpatch.h"
#include "repl.h"

#define PGPATCH_SOCK_DIR "/tmp/pgpatch"

static char pgpatch_sock_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
static pthread_t pgpatch_repl_thread;
static bool pgpatch_repl_started = false;

/* ---- small socket helpers (thread context; no Postgres calls) ---- */

static bool
send_all(int fd, const char *buf, size_t len)
{
	while (len > 0)
	{
		ssize_t n = send(fd, buf, len, 0);

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		buf += n;
		len -= (size_t) n;
	}
	return true;
}

static bool
send_str(int fd, const char *s)
{
	return send_all(fd, s, strlen(s));
}

static bool
whitespace_only(const char *s)
{
	for (; *s; s++)
		if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
			return false;
	return true;
}

/*
 * Serve one connected client until it disconnects.  Accumulates input until it
 * forms a complete Janet expression, evaluates it, and writes the result.
 */
static void
serve_client(int cfd)
{
	const char *prompt = "pgpatch> ";
	const char *cont = "    ...> ";
	size_t		cap = 1024;
	size_t		len = 0;
	char	   *acc = malloc(cap);
	char		buf[1024];

	if (acc == NULL)
		return;

	send_str(cfd, "; pgpatch Janet REPL. Definitions persist in this backend.\n");
	send_str(cfd, prompt);

	for (;;)
	{
		ssize_t		n = recv(cfd, buf, sizeof(buf), 0);
		bool		saw_newline;

		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			break;				/* disconnect or error */
		}

		/* Grow the accumulator as needed and append this chunk. */
		if (len + (size_t) n + 1 > cap)
		{
			char	   *tmp;

			while (len + (size_t) n + 1 > cap)
				cap *= 2;
			tmp = realloc(acc, cap);
			if (tmp == NULL)
				break;
			acc = tmp;
		}
		memcpy(acc + len, buf, (size_t) n);
		len += (size_t) n;
		acc[len] = '\0';

		saw_newline = (memchr(buf, '\n', (size_t) n) != NULL);
		if (!saw_newline)
			continue;			/* keep reading the current line */

		if (whitespace_only(acc))
		{
			len = 0;
			acc[0] = '\0';
			send_str(cfd, prompt);
		}
		else if (pgpatch_input_complete(acc))
		{
			bool		is_error = false;
			char	   *res = pgpatch_janet_eval_locked(acc, &is_error);

			if (is_error)
				send_str(cfd, "error: ");
			send_str(cfd, res);
			send_str(cfd, "\n");
			free(res);

			len = 0;
			acc[0] = '\0';
			send_str(cfd, prompt);
		}
		else
		{
			/* Incomplete form: prompt for continuation. */
			send_str(cfd, cont);
		}
	}

	free(acc);
	close(cfd);
}

/*
 * REPL thread entry point.  Owns the listening socket for the backend's life.
 */
static void *
repl_thread_main(void *arg)
{
	int			lfd;
	struct sockaddr_un addr;
	sigset_t	block_all;

	(void) arg;

	/* Keep all signals on the main thread. */
	sigfillset(&block_all);
	pthread_sigmask(SIG_SETMASK, &block_all, NULL);

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, pgpatch_sock_path, sizeof(addr.sun_path));

	unlink(pgpatch_sock_path);	/* clear any stale socket */
	if (bind(lfd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
		listen(lfd, 4) < 0)
	{
		close(lfd);
		return NULL;
	}

	for (;;)
	{
		int			cfd = accept(lfd, NULL, NULL);

		if (cfd < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		serve_client(cfd);		/* one client at a time */
	}

	close(lfd);
	return NULL;
}

/*
 * on_proc_exit cleanup (main thread): remove the socket file.
 */
static void
repl_cleanup(int code, Datum arg)
{
	(void) code;
	(void) arg;
	if (pgpatch_sock_path[0] != '\0')
		unlink(pgpatch_sock_path);
}

void
pgpatch_repl_start(void)
{
	pthread_attr_t attr;

	if (pgpatch_repl_started)
		return;

	/* mkdir the socket directory (ignore "already exists"). */
	if (mkdir(PGPATCH_SOCK_DIR, 0700) < 0 && errno != EEXIST)
		return;

	snprintf(pgpatch_sock_path, sizeof(pgpatch_sock_path),
			 "%s/%d.sock", PGPATCH_SOCK_DIR, (int) MyProcPid);

	on_proc_exit(repl_cleanup, (Datum) 0);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&pgpatch_repl_thread, &attr, repl_thread_main, NULL) == 0)
		pgpatch_repl_started = true;
	pthread_attr_destroy(&attr);
}

const char *
pgpatch_repl_socket_path(void)
{
	return pgpatch_sock_path;
}
