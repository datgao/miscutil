/*
	This file is part of miscutil.
	Copyright (C) 2016-2018, Robert L. Thompson

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Highlight standard error, inspired by https://github.com/charliesome/errexec */

#define _GNU_SOURCE
#include <sys/uio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof (a) / sizeof *(a))
#endif

enum ansi_color {
	COLOR_BLACK,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_YELLOW,
	COLOR_BLUE,
	COLOR_MAGENTA,
	COLOR_CYAN,
	COLOR_WHITE,
};

#ifndef COLOR_DEFAULT
#define COLOR_DEFAULT	COLOR_RED
#endif

enum stdidx {
	STDIN,
	STDOUT,
	STDERR,
	STDNUM
};

enum {
	PIPEOUT,
	PIPEERR,
	PIPENUM
};

enum {
	PIPERD,
	PIPEWR,
	PIPEOPS
};


struct wrap_glob_st {
	const char	*name;				// our name from cmdline
	pid_t		pid;				// child process
	sig_atomic_t	quit;
	int		status;
	int		fdpipe[PIPENUM][PIPEOPS];
	const int	fdstd[STDNUM];
};


static struct wrap_glob_st wrap_glob = {
	.name = NULL,
	.pid = -1,
	.quit = 0,
	.status = EXIT_SUCCESS,
	.fdpipe = { [PIPEOUT] = { [PIPERD] = -1, [PIPEWR] -1 }, [PIPEERR] = { [PIPERD] = -1, [PIPEWR] = -1 } },
	.fdstd = { [STDIN] = STDIN_FILENO, [STDOUT] = STDOUT_FILENO, [STDERR] = STDERR_FILENO },
};


static bool write_iov(int fd, struct iovec *iov, unsigned niov)
{
	bool ret = false;
	ssize_t len;

	do {
		len = writev(fd, iov, niov);
		if (len < 0) {
			if (errno != EINTR) { perror("writev()"); goto fail; }
			continue;
		}
		do {
			if ((size_t)len < iov->iov_len) {
				iov->iov_base += len;
				iov->iov_len -= len;
				break;
			}
			len -= iov->iov_len;
			iov++;
			niov--;
		} while (niov > 0);
	} while (niov > 0);

	ret = true;
fail:
	return ret;
}


//#if defined(SPLICE_F_NONBLOCK) && !defined(AVOID_SPLICE)

#if 0

static bool write_ansi(int *fdp, bool *is_on, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	ssize_t len;
	struct iovec iov;

	if (*is_on != want) {
		*is_on = want;
		iov.iov_base = (void *)ansi;
		iov.iov_len = size;
		if (!write_iov(fda, &iov, 1)) goto fail;
	}
	if (fdp != NULL) {
		len = splice(*fdp, NULL, fdb, NULL, 65536, SPLICE_F_NONBLOCK);
		if (len < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { perror("splice()"); goto fail; }
		if (len == 0) { close(*fdp); *fdp = -1; }
	}

	ret = true;
fail:
	return ret;
}

#endif

//#else

static ssize_t readfd(int *fdp, char *buf, size_t size)
{
	ssize_t len, ret = 0;

	len = read(*fdp, buf, size);
	if (len < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { perror("read()"); goto out; }
		goto skip;
	}
	if (len == 0) { close(*fdp); *fdp = -1; }

out:
	ret = len;
skip:
	return ret;
}

#if 0

static bool write_ansi(int *fdp, bool *is_on, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	ssize_t len = 0;
	char buf[4096];
	struct iovec iov[2];
	unsigned niov = 0;

	if (*is_on != want) {
		*is_on = want;
		iov[niov].iov_base = (void *)ansi;
		iov[niov].iov_len = size;
		niov++;
		if (fda != fdb) {
			if (!write_iov(fda, iov, niov)) goto fail;
			niov = 0;
		}
	}
	if (fdp != NULL) {
		len = readfd(fdp, buf, sizeof buf);
		if (len < 0) goto fail;
		if (len > 0) {
			iov[niov].iov_base = (void *)buf;
			iov[niov].iov_len = len;
			niov++;
		}
	}
	if (niov > 0 && !write_iov(fdb, iov, niov)) goto fail;

	ret = true;
fail:
	return ret;
}

#endif


static bool write_ansi(int *fdp, bool *is_on, bool want, int fda, const char *ansi, size_t size, int fdb, bool *try_splice)
{
	bool ret = false;
	ssize_t len = 0;
	char buf[4096];
	struct iovec iov[2];
	unsigned niov = 0;

	if (*is_on != want) {
		*is_on = want;
		iov[niov].iov_base = (void *)ansi;
		iov[niov].iov_len = size;
		niov++;
	}

	if (fdp != NULL) {

#if defined(SPLICE_F_NONBLOCK) && !defined(AVOID_SPLICE)
		if (fdb < 0) *try_splice = false;

		if (*try_splice) {
			if (niov != 0 && !write_iov(fda, iov, niov)) goto fail;
			niov = 0;

			len = splice(*fdp, NULL, fdb, NULL, 65536, SPLICE_F_NONBLOCK);
			if (len == 0) { close(*fdp); *fdp = -1; }
			if (len < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				//perror("splice()\x07");
				*try_splice = false;	// fdb has O_APPEND - GNU screen?
			}
		}
#else
		*try_splice = false;
#endif

		if (!*try_splice) {
			if (niov != 0 && fda != fdb) {
				if (!write_iov(fda, iov, niov)) goto fail;
				niov = 0;
			}
			len = readfd(fdp, buf, sizeof buf);
			if (len < 0) goto fail;
			if (len > 0) {
				iov[niov].iov_base = (void *)buf;
				iov[niov].iov_len = len;
				niov++;
			}
		}

#if defined(SPLICE_F_NONBLOCK) && !defined(AVOID_SPLICE)
		//*try_splice = true;
#endif
	}
	if (fdb >= 0 && niov > 0 && !write_iov(fdb, iov, niov)) goto fail;

	ret = true;
fail:
	return ret;
}


static bool write_color(int *fdp, const char *code, bool *is_on, bool want, int fda, int fdb, bool *try_splice)
{
	bool ret = false;
	char highlight[8];
	static const char restore[] = "\x1b[0m";
	const char *ansi = want ? highlight : restore;
	size_t pick = want ? sizeof highlight : sizeof restore;

	snprintf(highlight, sizeof highlight, "\x1b[%s;1m", code);
	if (!write_ansi(fdp, is_on, want, fda, ansi, pick - 1, fdb, try_splice)) goto fail;

	ret = true;
fail:
	return ret;
}


static bool write_stuff(int fdpipe[PIPENUM][PIPEOPS], fd_set *fdset, const char *code, bool *is_on, const int fdstd[STDNUM], bool *try_splice)
{
	bool ret, state;
	unsigned i;
	int fdb, *fdp;

	for (i = 0, ret = true, state = *is_on; i < 2 && ret; i++, state = !state) {
		if (state) {
			fdp = &fdpipe[PIPEERR][PIPERD];
			fdb = fdstd[STDERR];
		} else {
			fdp = &fdpipe[PIPEOUT][PIPERD];
			fdb = fdstd[STDOUT];
		}

		if (fdp != NULL && (*fdp < 0 || !FD_ISSET(*fdp, fdset))) fdp = NULL;

		ret = write_color(fdp, code, is_on, state, fdstd[STDERR], fdb, try_splice);
	}

	return ret;
}


static void exec_child(char *const argv[], int fdpipe[PIPENUM][PIPEOPS], const int fdstd[STDNUM])
{
	int flags, fd = -1;
	FILE *fp;

	close(fdpipe[PIPEOUT][PIPERD]);
	close(fdpipe[PIPEERR][PIPERD]);
	fd = dup(fdstd[STDERR]);

	if (dup2(fdpipe[PIPEOUT][PIPEWR], fdstd[STDOUT]) < 0
	|| dup2(fdpipe[PIPEERR][PIPEWR], fdstd[STDERR]) < 0) {
		perror("dup2()");
		return;
	}

	close(fdpipe[PIPEOUT][PIPEWR]);
	close(fdpipe[PIPEERR][PIPEWR]);

	if (fd >= 0) {
		flags = fcntl(fd, F_GETFD);
		if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
	(void)execvp(argv[0], argv);
	if (fd < 0) return;

	fp = fdopen(fd, "a");
	if (fp == NULL) return;
	fprintf(fp, "execvp() failed\n");
	fclose(fp);
}


static void fdset_add(int *fdpmax, fd_set *fdset, int fd)
{
	if (fd < 0) return;
	FD_SET(fd, fdset);
	if (*fdpmax < fd) *fdpmax = fd;
}


static bool fd_nonblock(int fd, bool nonblock)
{
	bool ret = false;
	int flags, mask = O_NONBLOCK, bit = nonblock ? mask : 0;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0) { perror("fcntl(F_GETFL)"); goto fail; }

	flags ^= mask;

	if ((flags & mask) == bit && fcntl(fd, F_SETFL, flags) != 0) { perror("fcntl(F_SETFL)"); goto fail; }

	ret = true;
fail:
	return ret;
}


static bool color_output(struct wrap_glob_st *glob)
{
	bool ret = false, is_on = false, try_splice = true;
	int fd, i, nfds = -1;
	const int *fdstd = glob->fdstd;
	fd_set fdset;
	char code[3];
	const char **pstr, *ptr, *name = glob->name;
	size_t len, namelen;
	static const char *palette[] = {	[COLOR_BLACK] = "black", [COLOR_RED] = "red",
						[COLOR_GREEN] = "green", [COLOR_YELLOW] = "yellow",
						[COLOR_BLUE] = "blue", [COLOR_MAGENTA] = "magenta",
						[COLOR_CYAN] = "cyan", [COLOR_WHITE] = "white" };
	enum ansi_color entries = ARRAY_SIZE(palette), color = entries;

	for (i = 0; i < STDNUM; i++) {
		fd = fdstd[i];
		if (i == STDIN) { close(fd); continue; }
		if (!fd_nonblock(fd, false)) goto fail;
	}

	for (i = 0; i < PIPENUM; i++) {
		if (!fd_nonblock(glob->fdpipe[i][PIPERD], true)) goto fail;
		close(glob->fdpipe[i][PIPEWR]);
	}

	if (name != NULL) {
		namelen = strlen(name);
		for (color = 0, pstr = palette, ptr = name + namelen; color < entries; color++, pstr++) {
			if (namelen >= (len = strlen(*pstr)) && strcmp(*pstr, ptr - len) == 0) break;
		}
	}
	if (color >= entries) color = COLOR_DEFAULT;
	snprintf(code, sizeof code, "3%u", color);

	while (glob->quit == 0) {
		nfds = -1;
		FD_ZERO(&fdset);
		fdset_add(&nfds, &fdset, glob->fdpipe[PIPEOUT][PIPERD]);
		fdset_add(&nfds, &fdset, glob->fdpipe[PIPEERR][PIPERD]);
		if (nfds < 0) break;
		nfds = select(nfds + 1, &fdset, NULL, NULL, NULL);
		if (nfds < 0) { if (errno == EINTR) continue; perror("select()"); goto fail; }
		if (nfds == 0) continue;
		if (!write_stuff(glob->fdpipe, &fdset, code, &is_on, fdstd, &try_splice)) goto fail;
	}

	ret = true;
fail:
	if (!write_color(NULL, code, &is_on, false, fdstd[STDERR], -1, &try_splice)) ret = EXIT_FAILURE;
	return ret;
}


static int wrap_sig_mask(int how, const sigset_t *set, sigset_t *oldset)
{
	int ret;

#ifdef WRAP_MT_PTHREAD		// not defined as process has main thread only and compile/link w/o -pthread
	static const char *const str = "pthread_sigmask()";
	ret = pthread_sigmask(how, set, oldset);
	errno = ret;
	if (ret != 0) ret = -1;
#else
	static const char *const str = "sigprocmask()";
	ret = sigprocmask(how, set, oldset);
#endif
	if (ret != 0) perror(str);
	return ret;
}


static int wrap_sig_setup(int signum, void (*sigfn)(int), int sign_block)
{
	int ret = -1, how = (sign_block < 0) ? SIG_BLOCK : SIG_UNBLOCK;
	struct sigaction sa;
	sigset_t sigset;

	if (sign_block != 0) {
		if (sigemptyset(&sigset) != 0) { perror("sigemptyset()"); goto fail; }
		if (sigaddset(&sigset, signum) != 0) { perror("sigaddset()"); goto fail; }
		if (wrap_sig_mask(how, &sigset, NULL) != 0) goto fail;
	}

	if (sigfn != SIG_ERR) {		// SIG_DFL, SIG_IGN, or custom - allow NULL in case defined equal
		sa.sa_flags = 0;
		sa.sa_handler = sigfn;
		if (sigaction(signum, &sa, NULL) != 0) { perror("sigaction()"); goto fail; }
	}

	ret = 0;
fail:
	return ret;
}


static void sigchld_handler(int signum)
{
	int status, errnum = errno;
	pid_t pid;

	(void)signum;

	do {
		pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
		if (pid == 0) break;
		if (pid == wrap_glob.pid) {
			if (WIFSTOPPED(status) || WIFCONTINUED(status)) break;

			if (WIFEXITED(status)) {
				wrap_glob.status = WEXITSTATUS(status);
			} else if (WIFSIGNALED(status)) {
				wrap_glob.status = 128 + WTERMSIG(status);
			}
		}

		if (pid < 0) {
			if (errno == EINTR) break;
			perror("waitpid()");
			// fall through
		}

		wrap_glob.quit = 1;		// mismatch or exited - anything better?

	} while (false);	// idiomatic goto

	errno = errnum;
}


static void sigterm_handler(int signum)
{
	(void)signum;

	wrap_glob.quit = 1;
}


int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE, argn = argc - 1, adj = 0;
	char *const *arga = argv + 1;

	wrap_glob.name = argv[0];
	if (argn > 0 && **arga == '\0') adj++;

	if (argn <= adj) {
		fprintf(stderr, "%s command [ [ '' ] args ]\n", wrap_glob.name);
		goto fail;
	}

	if (isatty(wrap_glob.fdstd[STDERR]) != 1) {
		(void)execvp(arga[adj], arga + adj);
		fprintf(stderr, "execvp() failed\n");
		goto fail;
	}

	if (pipe(wrap_glob.fdpipe[PIPEOUT]) != 0 || pipe(wrap_glob.fdpipe[PIPEERR]) != 0) {
		perror("pipe()");
		goto fail;
	}

	wrap_glob.pid = fork();
	if (wrap_glob.pid < 0) { perror("fork()"); goto fail; }

	if (wrap_glob.pid == 0) { exec_child(arga + adj, wrap_glob.fdpipe, wrap_glob.fdstd); goto fail; }

	if (wrap_sig_setup(SIGPIPE, SIG_IGN, -1) != 0
	|| wrap_sig_setup(SIGCHLD, &sigchld_handler, 1) != 0
	|| wrap_sig_setup(SIGTERM, &sigterm_handler, 1) != 0
	|| wrap_sig_setup(SIGINT, &sigterm_handler, 1) != 0
	|| wrap_sig_setup(SIGQUIT, &sigterm_handler, 1) != 0
	|| wrap_sig_setup(SIGHUP, &sigterm_handler, 0) != 0
	|| !color_output(&wrap_glob)) goto fail;

	ret = wrap_glob.status;
fail:
	return ret;
}

