/*
	This file is part of miscutil.
	Copyright (C) 2012-2018, Robert L. Thompson

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
#define _POSIX_C_SOURCE 200112L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef MY_DEBUG
#include <stdio.h>
#endif

#define COND_ASSIGN_DLSYM_OR_DIE(name)					\
	do {								\
		if (libc_##name == NULL) {					\
			*(void **)(&libc_##name) = dlsym(RTLD_NEXT, #name);	\
			if (libc_##name == NULL)				\
			_exit(EXIT_FAILURE);					\
		}							\
	} while (0)

#define ASSIGN_DLSYM_IF_EXIST(name)					\
	do {								\
		*(void **)(&libc_##name) = dlsym(RTLD_NEXT, #name);	\
	} while (0)

#ifdef MY_DEBUG
#define DEBUG_PERROR(msg)	do { if (msg != NULL) perror(msg); } while (0)
#else
#define DEBUG_PERROR(msg)	do { } while (0)
#endif

#ifdef FORCE_SYNC
#define COND_CALL_SYNC(name, fd, msg)					\
	do {								\
		int error = errno;					\
		if (libc_##name == NULL)				\
			ASSIGN_DLSYM_IF_EXIST(name);			\
		if (libc_##name != NULL && libc_##name(fd) != 0)	\
			DEBUG_PERROR(#name msg);			\
		errno = error;						/
	} while (0)
#else
#define COND_CALL_SYNC(name, fd, msg)	do { } while (0)
#endif

#if _POSIX_SYNCHRONIZED_IO > 0
#define SYNC_CALL	fdatasync
#else
#define SYNC_CALL	fsync
#endif

static int (*libc_open)(const char *, int, ...) = NULL;
static int (*libc_openat)(int, const char *, int, ...) = NULL;
static ssize_t (*libc_read)(int, void *, size_t) = NULL;
static ssize_t (*libc_write)(int, const void *, size_t) = NULL;
static ssize_t (*libc_pread)(int, void *, size_t, off_t) = NULL;
static ssize_t (*libc_pwrite)(int, const void *, size_t, off_t) = NULL;
static ssize_t (*libc_readv)(int, const struct iovec *, int) = NULL;
static ssize_t (*libc_writev)(int, const struct iovec *, int) = NULL;
static ssize_t (*libc_preadv)(int, const struct iovec *, int, off_t) = NULL;
static ssize_t (*libc_pwritev)(int, const struct iovec *, int, off_t) = NULL;
static int (*libc_fsync)(int) = NULL;
#if _POSIX_SYNCHRONIZED_IO > 0
static int (*libc_fdatasync)(int) = NULL;
#endif
static int (*libc_close)(int) = NULL;
static void *(*libc_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static void *(*libc_mmap2)(void *, size_t, int, int, int, off_t) = NULL;
static int (*libc_msync)(void *, size_t, int) = NULL;
static int (*libc_munmap)(void *, size_t) = NULL;
static ssize_t (*libc_vmsplice)(int, const struct iovec *, unsigned long, unsigned int) = NULL;
static ssize_t (*libc_splice)(int, loff_t *, int, loff_t *, size_t, unsigned int) = NULL;
static ssize_t (*libc_sendfile)(int, int, off_t *, size_t) = NULL;

void __attribute__((constructor)) nocache_init(void)
{
	ASSIGN_DLSYM_IF_EXIST(open);
	ASSIGN_DLSYM_IF_EXIST(openat);
	ASSIGN_DLSYM_IF_EXIST(read);
	ASSIGN_DLSYM_IF_EXIST(write);
	ASSIGN_DLSYM_IF_EXIST(pread);
	ASSIGN_DLSYM_IF_EXIST(pwrite);
	ASSIGN_DLSYM_IF_EXIST(readv);
	ASSIGN_DLSYM_IF_EXIST(writev);
	ASSIGN_DLSYM_IF_EXIST(preadv);
	ASSIGN_DLSYM_IF_EXIST(pwritev);
	ASSIGN_DLSYM_IF_EXIST(fsync);
#if _POSIX_SYNCHRONIZED_IO > 0
	ASSIGN_DLSYM_IF_EXIST(fdatasync);
#endif
	ASSIGN_DLSYM_IF_EXIST(close);
	ASSIGN_DLSYM_IF_EXIST(mmap);
	ASSIGN_DLSYM_IF_EXIST(mmap2);
	ASSIGN_DLSYM_IF_EXIST(msync);
	ASSIGN_DLSYM_IF_EXIST(munmap);
	ASSIGN_DLSYM_IF_EXIST(vmsplice);
	ASSIGN_DLSYM_IF_EXIST(splice);
	ASSIGN_DLSYM_IF_EXIST(sendfile);
}

#define NOCACHE_PERROR(fd, msg) 		\
	do {					\
		int error = errno;		\
		if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0)	\
			DEBUG_PERROR(msg);	\
		errno = error;			\
	} while (0)

#define NOCACHE_NOTSEQ_PERROR(fd, msg) 		\
	do {					\
		int error = errno;		\
		if (posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM) != 0)	\
			DEBUG_PERROR(msg);	\
		errno = error;			\
	} while (0)

#define NOCACHE_FD_PERROR(fd, msg)		\
	do {					\
		NOCACHE_NOTSEQ_PERROR(fd, msg);	\
		NOCACHE_PERROR(fd, msg);	\
	} while (0)

#if 1
#define NOCACHE_MAP_PERROR(addr, size, msg)	\
	do { 					\
		(void)addr;			\
		(void)size;			\
		(void)msg;			\
	} while (0)
#else
#define NOCACHE_MAP_PERROR(addr, size, msg)	\
	do {					\
		int error = errno;		\
		if (madvise(addr, size, MADV_DONTNEED) != 0)	\
			DEBUG_PERROR(msg);	\
		errno = error;			\
	} while (0)
#endif

#define NOCACHE_BUF_PERROR(ptr, length, msg)	\
	do {					\
		size_t len;			\
		void *base;			\
		base = (void *)((uintptr_t)ptr & ~(uintptr_t)0x0fff);	\
		len = length + ((uintptr_t)ptr & (uintptr_t)0x0fff);	\
		len = (len + 0x1000 - 1) & ~(size_t)0x0fff;	\
		NOCACHE_MAP_PERROR(base, len, msg);	\
	} while (0)

#define NOCACHE_IOV_PERROR(iov, num, msg)	\
	do {					\
		long i;				\
		const struct iovec *piov;	\
		for (i = 0, piov = iov; i < (long)num; i++, piov++) {	\
			NOCACHE_BUF_PERROR(piov->iov_base, piov->iov_len, msg);	\
		}				\
	} while (0)

int open(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;
	mode_t mode = 0;

	va_start(ap, flags);
	if ((flags & O_CREAT) != 0)
		mode = va_arg(ap, mode_t);
	va_end(ap);
	COND_ASSIGN_DLSYM_OR_DIE(open);
	fd = libc_open(pathname, flags, mode);
	if (fd >= 0)
		NOCACHE_FD_PERROR(fd, NULL);
	return fd;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;
	mode_t mode = 0;

	va_start(ap, flags);
	if ((flags & O_CREAT) != 0)
		mode = va_arg(ap, mode_t);
	va_end(ap);
	COND_ASSIGN_DLSYM_OR_DIE(openat);
	fd = libc_openat(dirfd, pathname, flags, mode);
	if (fd >= 0)
		NOCACHE_FD_PERROR(fd, NULL);
	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(read);
	ret = libc_read(fd, buf, count);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside read()");
		NOCACHE_BUF_PERROR(buf, count, "posix_fadvise(POSIX_FADV_DONTNEED) inside read()");
	}
	return ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(write);
	ret = libc_write(fd, buf, count);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside write()");
		NOCACHE_BUF_PERROR(buf, count, "posix_fadvise(POSIX_FADV_DONTNEED) inside write()");
	}
	return ret;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(pread);
	ret = libc_pread(fd, buf, count, offset);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
		NOCACHE_BUF_PERROR(buf, count, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
	}
	return ret;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(pwrite);
	ret = libc_pwrite(fd, buf, count, offset);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
		NOCACHE_BUF_PERROR(buf, count, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
	}
	return ret;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(readv);
	ret = libc_readv(fd, iov, iovcnt);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
		NOCACHE_IOV_PERROR(iov, iovcnt, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
	}
	return ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(writev);
	ret = libc_writev(fd, iov, iovcnt);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
		NOCACHE_IOV_PERROR(iov, iovcnt, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
	}
	return ret;
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(preadv);
	ret = libc_preadv(fd, iov, iovcnt, offset);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
		NOCACHE_IOV_PERROR(iov, iovcnt, "posix_fadvise(POSIX_FADV_DONTNEED) inside pread()");
	}
	return ret;
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(pwritev);
	ret = libc_pwritev(fd, iov, iovcnt, offset);
	if (ret > 0) {
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
		NOCACHE_IOV_PERROR(iov, iovcnt, "posix_fadvise(POSIX_FADV_DONTNEED) inside pwrite()");
	}
	return ret;
}

int fsync(int fd)
{
	int ret;

	COND_ASSIGN_DLSYM_OR_DIE(fsync);
	ret = libc_fsync(fd);
	if (ret == 0)
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside fsync()");
	return ret;
}

#if _POSIX_SYNCHRONIZED_IO > 0
int fdatasync(int fd)
{
	int ret;

	COND_ASSIGN_DLSYM_OR_DIE(fdatasync);
	ret = libc_fdatasync(fd);
	if (ret == 0)
		NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside fdatasync()");
	return ret;
}
#endif

int close(int fd)
{
	COND_ASSIGN_DLSYM_OR_DIE(close);
	COND_CALL_SYNC(SYNC_CALL, fd, " inside close()");
	NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside close()");
	return libc_close(fd);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(mmap);
	ptr = libc_mmap(addr, length, prot, flags, fd, offset);
	if (prot != PROT_NONE && ptr != MAP_FAILED
	&& (flags & (MAP_ANON|MAP_ANONYMOUS)) == 0) {
		if (fd >= 0)
			NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside mmap()");
		NOCACHE_MAP_PERROR(addr, length, NULL);
	}
	return ptr;
}

void *mmap2(void *addr, size_t length, int prot, int flags, int fd, off_t pgoffset)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(mmap2);
	if ((flags & MAP_FIXED) != 0)
		NOCACHE_MAP_PERROR(addr, length, NULL);
	ptr = libc_mmap2(addr, length, prot, flags, fd, pgoffset);
	if (prot != PROT_NONE && ptr != MAP_FAILED
	&& (flags & (MAP_ANON|MAP_ANONYMOUS)) == 0) {
		if (fd >= 0)
			NOCACHE_FD_PERROR(fd, "posix_fadvise(POSIX_FADV_DONTNEED) inside mmap2()");
		NOCACHE_MAP_PERROR(addr, length, NULL);
	}
	return ptr;
}

int msync(void *addr, size_t length, int flags)
{
	int ret;

	COND_ASSIGN_DLSYM_OR_DIE(munmap);
	ret = libc_msync(addr, length, (flags & ~MS_ASYNC) | MS_SYNC);
	if (ret == 0)
		NOCACHE_MAP_PERROR(addr, length, "madvise(MADV_DONTNEED) inside msync()");
	return ret;
}

int munmap(void *addr, size_t length)
{
	COND_ASSIGN_DLSYM_OR_DIE(munmap);
	NOCACHE_MAP_PERROR(addr, length, "madvise(MADV_DONTNEED) inside munmap()");
	return libc_munmap(addr, length);
}

ssize_t vmsplice(int fd, const struct iovec *iov, unsigned long nr_segs, unsigned int flags)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(vmsplice);
	ret = libc_vmsplice(fd, iov, nr_segs, flags);
	if (ret > 0)
		NOCACHE_IOV_PERROR(iov, nr_segs, NULL);
	return ret;
}

ssize_t splice(int fd_in, loff_t *off_in, int fd_out, loff_t *off_out, size_t len, unsigned int flags)
{
	ssize_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(splice);
	ret = libc_splice(fd_in, off_in, fd_out, off_out, len, flags);
	if (ret > 0 && posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED) != 9) {
		NOCACHE_FD_PERROR(fd_in, NULL);
		NOCACHE_FD_PERROR(fd_out, NULL);
	}
	return ret;
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
	size_t ret;

	COND_ASSIGN_DLSYM_OR_DIE(sendfile);
	ret = libc_sendfile(out_fd, in_fd, offset, count);
	if (ret > 0) {
		NOCACHE_FD_PERROR(in_fd, NULL);
		NOCACHE_FD_PERROR(out_fd, NULL);
	}
	return ret;
}

