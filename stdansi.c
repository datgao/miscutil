/*
	This file is part of miscutil.
	Copyright (C) 2016, Robert L. Thompson

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
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static bool writeiov(int fd, struct iovec *iov, unsigned niov)
{
	bool ret = false;
	ssize_t len;

	do {
		len = writev(fd, iov, niov);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("writev()");
			goto fail;
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

#if defined(SPLICE_F_NONBLOCK) && !defined(AVOID_SPLICE)
static bool writeansi(int *fdp, bool *is_on, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	ssize_t len;
	struct iovec iov;

	if (*is_on != want) {
		*is_on = want;
		iov.iov_base = (void *)ansi;
		iov.iov_len = size;
		if (!writeiov(fda, &iov, 1))
			goto fail;
	}
	if (fdp != NULL) {
		len = splice(*fdp, NULL, fdb, NULL, 65536, SPLICE_F_NONBLOCK);
		if (len < 0 && errno != EINTR && errno != EAGAIN) {
			perror("splice()");
			goto fail;
		}
		if (len == 0) {
			close(*fdp);
			*fdp = -1;
		}
	}

	ret = true;
fail:
	return ret;
}
#else
static size_t readfd(int *fdp, char *buf, size_t size)
{
	size_t ret = 0;
	ssize_t len;
	int fd = *fdp;

	len = read(fd, buf, size);
	if (len < 0) {
		if (errno == EINTR)
			goto out;
		perror("read()");
		goto out;
	}
	if (len == 0) {
		close(fd);
		*fdp = -1;
		goto out;
	}

	ret = len;
out:
	return ret;
}

static bool writeansi(int *fdp, bool *is_on, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	size_t len = 0;
	char buf[4096];
	struct iovec iov[2];
	unsigned niov = 0;

	if (*is_on != want) {
		*is_on = want;
		iov[niov].iov_base = (void *)ansi;
		iov[niov].iov_len = size;
		niov++;
		if (fda != fdb) {
			if (!writeiov(fda, iov, niov))
				goto fail;
			niov = 0;
		}
	}
	if (fdp != NULL) {
		len = readfd(fdp, buf, sizeof buf);
		if (len > 0) {
			iov[niov].iov_base = (void *)buf;
			iov[niov].iov_len = len;
			niov++;
		}
	}
	if (niov > 0 && !writeiov(fdb, iov, niov))
		goto fail;

	ret = true;
fail:
	return ret;
}
#endif

static bool writecolor(int *fdp, const char *code, bool *is_on, bool want, int fda, int fdb)
{
	bool ret = false;
	char highlight[8];
	static const char restore[] = "\x1b[0m";
	const char *ansi = want ? highlight : restore;
	size_t pick = want ? sizeof highlight : sizeof restore;

	snprintf(highlight, sizeof highlight, "\x1b[%s;1m", code);
	if (!writeansi(fdp, is_on, want, fda, ansi, pick - 1, fdb))
		goto fail;

	ret = true;
fail:
	return ret;
}

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

static bool writestuff(int fdpipe[PIPENUM][PIPEOPS], fd_set *fdset, const char *code, bool *is_on, const int fdstd[STDNUM])
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
		if (fdp != NULL && (*fdp < 0 || !FD_ISSET(*fdp, fdset)))
			fdp = NULL;
		ret = writecolor(fdp, code, is_on, state, fdstd[STDERR], fdb);
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
		if (flags >= 0)
			(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
	(void)execvp(argv[0], argv);
	if (fd < 0)
		return;
	fp = fdopen(fd, "a");
	if (fp == NULL)
		return;
	fprintf(fp, "execvp() failed\n");
	fclose(fp);
}

static void fdset_add(int *fdpmax, fd_set *fdset, int fd)
{
	if (fd >= 0) {
		FD_SET(fd, fdset);
		if (*fdpmax < fd)
			*fdpmax = fd;
	}
}

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

#define ARRAY_LEN(a)	(sizeof(a)/sizeof(*(a)))

static bool color_output(int fdpipe[PIPENUM][PIPEOPS], const int fdstd[STDNUM], const char *name)
{
	bool ret = false, is_on = false;
	int nfds = -1;
	fd_set fdset;
	char code[3];
	const char *ptr, **pstr;
	size_t len, namelen;
	static const char *palette[] = {	[COLOR_BLACK] = "black", [COLOR_RED] = "red",
						[COLOR_GREEN] = "green", [COLOR_YELLOW] = "yellow",
						[COLOR_BLUE] = "blue", [COLOR_MAGENTA] = "magenta",
						[COLOR_CYAN] = "cyan", [COLOR_WHITE] = "white" };
	enum ansi_color entries = ARRAY_LEN(palette), color = entries;

	close(fdstd[STDIN]);
	close(fdpipe[PIPEOUT][PIPEWR]);
	close(fdpipe[PIPEERR][PIPEWR]);

	if (name != NULL) {
		namelen = strlen(name);
		for (color = 0, pstr = palette, ptr = name + namelen; color < entries; color++, pstr++)
			if (namelen >= (len = strlen(*pstr)) && strcmp(*pstr, ptr - len) == 0)
				break;
	}
	if (color >= entries)
		color = COLOR_DEFAULT;
	snprintf(code, sizeof code, "3%u", color);

	while (true) {
		nfds = -1;
		FD_ZERO(&fdset);
		fdset_add(&nfds, &fdset, fdpipe[PIPEOUT][PIPERD]);
		fdset_add(&nfds, &fdset, fdpipe[PIPEERR][PIPERD]);
		if (nfds < 0)
			break;
		nfds = select(nfds + 1, &fdset, NULL, NULL, NULL);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			perror("select()");
			goto fail;
		}
		if (nfds == 0)
			continue;
		if (!writestuff(fdpipe, &fdset, code, &is_on, fdstd))
			goto fail;
	}

	ret = true;
fail:
	if (!writecolor(NULL, code, &is_on, false, fdstd[STDERR], -1))
		ret = EXIT_FAILURE;
	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE, fdpipe[PIPENUM][PIPEOPS] = {{-1, -1}, {-1, -1}};
	const int fdstd[STDNUM] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
	char *const *arga = argv + 1;
	pid_t pid;
	sigset_t sigset;

	if (argc < 2) {
		fprintf(stderr, "%s command [ args ]\n", argv[0]);
		goto fail;
	}

	if (isatty(fdstd[STDERR]) != 1) {
		(void)execvp(arga[0], arga);
		fprintf(stderr, "execvp() failed\n");
		goto fail;
	}

	if (pipe(fdpipe[PIPEOUT]) != 0 || pipe(fdpipe[PIPEERR]) != 0) {
		perror("pipe()");
		goto fail;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork()");
		goto fail;
	}

	if (pid == 0) {
		exec_child(arga, fdpipe, fdstd);
		goto fail;
	}

	if (sigemptyset(&sigset) != 0) {
		perror("sigemptyset()");
		goto fail;
	}

	if (sigaddset(&sigset, SIGPIPE) != 0) {
		perror("sigaddset()");
		goto fail;
	}

	if (sigprocmask(SIG_BLOCK, &sigset, NULL) != 0) {
		perror("sigprocmask()");
		goto fail;
	}

	if (!color_output(fdpipe, fdstd, argv[0]))
		goto fail;

	ret = EXIT_SUCCESS;
fail:
	return ret;
}
