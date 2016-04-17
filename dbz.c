/*
	This file is part of miscutil.
	Copyright (C) 2015-2016, Robert L. Thompson

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

static bool dbz(const char *devpath, bool query)
{
	bool ret = false;
	int fd = -1;
	unsigned int does_zero;
	uint64_t range[2];

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		perror("open()");
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
		range[0] = 0;
		if (ioctl(fd, BLKGETSIZE64, &range[1]) != 0) {
			perror("ioctl(BLKGETSZ64)");
			goto fail;
		}
		if (ioctl(fd, BLKDISCARD, range) != 0) {
			perror("ioctl(BLKDISCARD)");
			goto fail;
		}
	}

	ret = true;
fail:
	if (fd >= 0)
		close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_FAILURE;
	bool query = true;

	if (argc != 3)
		goto usage;

	if (strcmp(argv[1], "-q") != 0) {
		if (strcmp(argv[1], "-d") != 0)
			goto usage;
		query = false;
	}
	if (!dbz(argv[2], query))
		goto fail;
	ret = EXIT_SUCCESS;
fail:
	return ret;
usage:
	fprintf(stderr, "%s [ -q | -d ] /dev/mmcblkX\n", argv[0]);
	goto fail;
}
