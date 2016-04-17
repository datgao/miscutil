/*
	This file is part of miscutil.
	Copyright (C) 2012-2016, Robert L. Thompson

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

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE	0x1
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE	0x2
#endif

static ssize_t readfd(int fd, off_t offt, char *buf, size_t size)
{
	size_t len = size;
	ssize_t rlen;

	do {
		rlen = pread(fd, buf, len, offt);
		if (rlen < 0) {
			if (errno == EINTR)
				continue;
			perror("read()");
			len = -1;
			goto fail;
		}
		if (rlen == 0)
			break;
		offt += rlen;
		buf += rlen;
		len -= rlen;
	} while (len > 0);

	rlen = size - len;
fail:
	return rlen;
}

static bool punch_hole(int fd, off_t offt, size_t blksz, const char *buf, size_t len)
{
	bool ret = false;
	size_t idx, hole, pos, last;

	pos = 0;
	last = 0;
	while (pos < len) {
		while (pos < len && *buf == '\0') {
			buf++;
			pos++;
		}
		idx = pos % blksz;
		pos -= idx;
		hole = pos - last;
		if (hole > 0 && fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offt, hole) != 0) {
			perror("fallocate()");
			goto fail;
		}
		idx = blksz - idx;
		offt += hole + idx;
		buf += blksz + idx;
		pos += blksz;
		last = pos;
	}
	ret = true;
fail:
	return ret;
}

static bool resparse(int fd, off_t flen, size_t blksz, char *buf, size_t bufsz)
{
	bool ret = false;
	off_t offt;
	ssize_t rlen;
	size_t len;

#if defined(SEEK_DATA) && defined(SEEK_HOLE)
	off_t hole, data;

	offt = 0;
	do {
		data = lseek(fd, offt, SEEK_DATA);
		hole = lseek(fd, data, SEEK_HOLE);
		if (data < 0 || hole < 0) {
			perror("lseek()");
			goto fail;
		}
		if (data > flen)
			break;
		offt = data;
		while (offt < hole) {
			len = hole - offt;
			if (len > bufsz)
				len = bufsz;
			rlen = readfd(fd, offt, buf, len);
			if (rlen < 0)
				goto fail;
			if (rlen == 0)
				goto success;
			len = rlen;
			if (!punch_hole(fd, offt, blksz, buf, len))
				goto fail;
			offt += len;
		}
	} while (hole < flen);
#else
	offt = 0;
	do {
		rlen = readfd(fd, offt, buf, bufsz);
		if (rlen < 0)
			goto fail;
		len = (size_t)rlen % blksz;
		rlen -= len;
		if (rlen == 0)
			goto success;
		len = rlen;
		if (!punch_hole(fd, offt, blksz, buf, len))
			goto fail;
		offt += len;
	} while (offt < flen);
#endif

success:
	ret = true;
fail:
	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE, fd = -1;
	struct stat st;
	off_t flen;
	blksize_t blksz;
	size_t bufsz;
	char *buf = NULL;

	if (argc != 2) {
		fprintf(stderr, "%s file\n", argv[0]);
		goto fail;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open()");
		goto fail;
	}

	if (fstat(fd, &st) < 0) {
		perror("stat()");
		goto fail;
	}

	flen = st.st_size;
	blksz = st.st_blksize;
	if (blksz <= 0) {
		fprintf(stderr, "Fatal: blocksize = %i\n", (int)blksz);
		goto fail;
	}
	if (flen < 0) {
		fprintf(stderr, "Fatal: negative file size\n");
		goto fail;
	}
	flen -= flen % blksz;
	if (flen == 0)
		goto success;

	bufsz = 65536;
	if (bufsz < (size_t)blksz) {
		bufsz = blksz;
	} else {
		bufsz = bufsz - bufsz % blksz;
	}

	buf = malloc(bufsz);
	if (buf == NULL) {
		perror("malloc()");
		goto fail;
	}

	if (!resparse(fd, flen, blksz, buf, bufsz))
		goto fail;

success:
	ret = EXIT_SUCCESS;
fail:
	if (fd >= 0)
		close(fd);
	if (buf != NULL)
		free(buf);
	return ret;
}
