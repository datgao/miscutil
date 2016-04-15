/*
	This file is part of stdansi.
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

#ifdef SPLICE_F_NONBLOCK
static bool writeansi(int *fdp, bool *color, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	ssize_t len;
	struct iovec iov;

	if (*color != want) {
		*color = want;
		iov.iov_base = (void *)ansi;
		iov.iov_len = size;
		if (!writeiov(fda, &iov, 1))
			goto fail;
	}
	if (fdp != NULL) {
		len = splice(*fdp, NULL, fdb, NULL, 65536, SPLICE_F_NONBLOCK);
		if (len < 0 && errno != EINTR) {
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

static bool writeansi(int *fdp, bool *color, bool want, int fda, const char *ansi, size_t size, int fdb)
{
	bool ret = false;
	size_t len = 0;
	char buf[4096];
	struct iovec iov[2];
	unsigned niov = 0;

	if (*color != want) {
		*color = want;
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

static bool writecolor(int *fdp, bool *color, bool want, int fda, int fdb)
{
	bool ret = false;
	static const char highlight[] = "\x1b[31;1m", restore[] = "\x1b[0m";
	const char *ansi = want ? highlight : restore;
	size_t pick = want ? sizeof highlight : sizeof restore;

	if (!writeansi(fdp, color, want, fda, ansi, pick - 1, fdb))
		goto fail;

	ret = true;
fail:
	return ret;
}

static bool writestuff(int *fdro, int *fdre, fd_set *fdset, bool *color, int fdwo, int fdwe)
{
	bool ret, state;
	unsigned i;
	int fdb, *fdp;

	for (i = 0, ret = true, state = *color; i < 2 && ret; i++, state = !state) {
		if (state) {
			fdp = fdre;
			fdb = fdwe;
		} else {
			fdp = fdro;
			fdb = fdwo;
		}
		if (fdp != NULL && (*fdp < 0 || !FD_ISSET(*fdp, fdset)))
			fdp = NULL;
		ret = writecolor(fdp, color, state, fdwe, fdb);
	}

	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE, nfds, flags, fd, outpipe[2], errpipe[2];
	const int fdin = STDIN_FILENO, fdout = STDOUT_FILENO, fderr = STDERR_FILENO;
	bool color = false;
	FILE *fp;
	pid_t pid;
	sigset_t sigset;
	fd_set fdset;

	if (argc < 2) {
		fprintf(stderr, "%s command [ args ]\n", argv[0]);
		goto fail;
	}

	if (isatty(fderr) != 1) {
		(void)execvp(argv[1], argv + 1);
		fprintf(stderr, "execvp() failed\n");
		goto fail;
	}

	if (pipe(outpipe) != 0 || pipe(errpipe) != 0) {
		perror("pipe()");
		goto fail;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork()");
		goto fail;
	}

	if (pid == 0) {
		close(outpipe[0]);
		close(errpipe[0]);
		fd = dup(fderr);
		if (dup2(outpipe[1], fdout) < 0
		|| dup2(errpipe[1], fderr) < 0) {
			perror("dup2()");
			goto fail;
		}
		close(outpipe[1]);
		close(errpipe[1]);
		if (fd >= 0) {
			flags = fcntl(fd, F_GETFD);
			if (flags >= 0)
				(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		}
		(void)execvp(argv[1], argv + 1);
		if (fd < 0)
			goto fail;
		fp = fdopen(fd, "a");
		if (fp == NULL)
			goto fail;
		fprintf(fp, "execvp() failed\n");
		fclose(fp);
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

	close(fdin);
	close(outpipe[1]);
	close(errpipe[1]);

	while (true) {
		FD_ZERO(&fdset);
		if (outpipe[0] >= 0)
			FD_SET(outpipe[0], &fdset);
		if (errpipe[0] >= 0)
			FD_SET(errpipe[0], &fdset);
		nfds = outpipe[0];
		if (nfds < errpipe[0])
			nfds = errpipe[0];
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
		if (!writestuff(&outpipe[0], &errpipe[0], &fdset, &color, fdout, fderr))
			goto fail;
	}

	ret = EXIT_SUCCESS;
fail:
	if (!writecolor(NULL, &color, false, fderr, -1))
		ret = EXIT_FAILURE;
	return ret;
}
