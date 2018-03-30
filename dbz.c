/*
	This file is part of miscutil.
	Copyright (C) 2015-2018, Robert L. Thompson

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

#define _FILE_OFFSET_BITS	64
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static bool dbz_discard(int fd, uint64_t *range)
{
	bool ret = false;

	if (range[1] != 0 && ioctl(fd, BLKDISCARD, range) != 0) {
		perror("ioctl(BLKDISCARD)");
		goto fail;
	}

	ret = true;
fail:
	return ret;
}

static bool dbz(const char *devpath, const char *fname, bool query)
{
	bool ret = false;
	int fd = -1, fdr = -1;
	unsigned does_zero, blksz, blk, i, flags = O_RDWR;
	uint64_t range[2];
	struct stat stdev, stfile;

	if (!query && fname == NULL)
		flags |= O_EXCL;
	fd = open(devpath, flags);
	if (fd < 0) {
		perror("open(devnode)");
		goto fail;
	}
	if (fstat(fd, &stdev) != 0) {
		perror("stat(devnode)");
		goto fail;
	}
	if (!S_ISBLK(stdev.st_mode)) {
		fprintf(stderr, "%s is not a block device\n", devpath);
		goto fail;
	}

	if (query) {
		if (ioctl(fd, BLKDISCARDZEROES, &does_zero) != 0) {
			perror("ioctl(BLKDISCARDZEROES)");
			goto fail;
		}
		if (does_zero == 0) {
			fprintf(stderr, "Device does NOT return zeroes for discarded blocks.\n");
			goto fail;
		}
		fprintf(stderr, "Device DOES return zeroes for discarded blocks.\n");
	} else {
		if (fname != NULL) {
			fdr = open(fname, O_RDWR);
			if (fdr < 0) {
				perror("open(file)");
				goto fail;
			}
			if (fstat(fdr, &stfile) != 0) {
				perror("stat(file)");
				goto fail;
			}
			if (!S_ISREG(stfile.st_mode)) {
				fprintf(stderr, "%s is not a regular file\n", fname);
				goto fail;
			}
			if (stdev.st_rdev != stfile.st_dev) {
				fprintf(stderr, "%s is not the mounted device containing %s\n", devpath, fname);
				goto fail;
			}

			if (ioctl(fdr, FIGETBSZ, &blksz) != 0) {
				perror("ioctl(FIGETBSZ)");
				goto fail;
			}
			range[0] = 0;
			range[1] = 0;
			for (i = 0; i < stfile.st_size / blksz; i++) {
				blk = i;
				if (ioctl(fdr, FIBMAP, &blk) != 0) {
					perror("ioctl(FIBMAP)");
					goto fail;
				}
				if (blk == 0)
					continue;
				if (blk == (range[0] + range[1]) / blksz) {
					range[1] += blksz;
					continue;
				}
				if (!dbz_discard(fd, range))
					goto fail;
				range[0] = (uint64_t)blk * blksz;
				range[1] = blksz;
			}
			if (!dbz_discard(fd, range))
				goto fail;
		} else {
			range[0] = 0;
			if (ioctl(fd, BLKGETSIZE64, &range[1]) != 0) {
				perror("ioctl(BLKGETSIZE64)");
				goto fail;
			}
			if (!dbz_discard(fd, range))
				goto fail;
		}
	}

	ret = true;
fail:
	if (fd >= 0)
		close(fd);
	if (fdr >= 0)
		close(fdr);
	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE;
	bool query = true;

	if (argc < 3 || argc > 4)
		goto usage;

	if (strcmp(argv[1], "-q") != 0) {
		if (strcmp(argv[1], "-d") != 0)
			goto usage;
		query = false;
	}
	if (!dbz(argv[2], argv[3], query))
		goto fail;
	ret = EXIT_SUCCESS;
fail:
	return ret;
usage:
	fprintf(stderr, "%s [ -q | -d ] /dev/mmcblkX [ /mount/point/dir/file.rm ]\n", argv[0]);
	goto fail;
}
