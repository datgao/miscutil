/*
	This file is part of icmpdsl.
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
		close(fd);
		*fdp = -1;
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

static bool writeiov(int fd, struct iovec *iov, unsigned niov)
{
	bool ret = false;
	ssize_t len;

	do {
		len = writev(fd, iov, niov);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("write()");
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

static bool writeansi(bool *color, bool want, int fda, const char *ansi, size_t size, int fdb, const char *buf, size_t len)
{
	bool ret = false;
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
	if (len > 0) {
		iov[niov].iov_base = (void *)buf;
		iov[niov].iov_len = len;
		niov++;
	}
	if (niov > 0 && !writeiov(fdb, iov, niov))
		goto fail;

	ret = true;
fail:
	return ret;
}

static bool writecolor(int *fdp, fd_set *fdset, bool *color, bool want, int fda, int fdb, char *buf, size_t size)
{
	bool ret = false;
	static const char highlight[] = "\x1b[31;1m", restore[] = "\x1b[0m";
	const char *ansi = want ? highlight : restore;
	size_t len = 0, pick = want ? sizeof highlight : sizeof restore;

	if ((fdp == NULL || (*fdp >= 0 && FD_ISSET(*fdp, fdset)
	&& (len = readfd(fdp, buf, size)) > 0))
	&& !writeansi(color, want, fda, ansi, pick - 1, fdb, buf, len))
		goto fail;

	ret = true;
fail:
	return ret;
}

static bool writestuff(int *fdro, int *fdre, fd_set *fdset, bool *color, int fdwo, int fdwe, char *buf, size_t size)
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
		ret = writecolor(fdp, fdset, color, state, fdwe, fdb, buf, size);
	}

	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE, nfds, flags, fd, outpipe[2], errpipe[2];
	bool color = false;
	FILE *fp;
	pid_t pid;
	sigset_t sigset;
	fd_set fdset;
	char buf[4096];

	if (argc < 2) {
		fprintf(stderr, "%s command [ args ]\n", argv[0]);
		goto fail;
	}

	if (isatty(STDOUT_FILENO) != 1 && isatty(STDERR_FILENO) != 1) {
		(void)execvp(argv[1], argv + 1);
		fprintf(stderr, "execvp() failed\n");
		goto fail;
	}

	if (pipe(outpipe) != 0 || pipe(errpipe) != 0) {
		perror("pipe()");
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

	pid = fork();
	if (pid < 0) {
		perror("fork()");
		goto fail;
	}

	if (pid != 0) {
		close(outpipe[0]);
		close(errpipe[0]);
		fd = dup(STDERR_FILENO);
		if (dup2(outpipe[1], STDOUT_FILENO) < 0
		|| dup2(errpipe[1], STDERR_FILENO) < 0) {
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

	close(STDIN_FILENO);
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
		if (!writestuff(&outpipe[0], &errpipe[0], &fdset, &color, STDOUT_FILENO, STDERR_FILENO, buf, sizeof buf))
			goto fail;
	}

	ret = EXIT_SUCCESS;
fail:
	if (!writecolor(NULL, NULL, &color, false, STDERR_FILENO, -1, NULL, 0))
		ret = EXIT_FAILURE;
	return ret;
}
