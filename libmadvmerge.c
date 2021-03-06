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
#include <sys/user.h>	// PAGE_SIZE
#include <sys/mman.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#ifdef MY_DEBUG
#include <stdio.h>
#endif

#ifndef AVOID_CALLOC
#define TRY_CALLOC
#endif

#ifndef AVOID_MPROTECT
#define TRY_MPROTECT
#endif

#define ASSIGN_DLSYM_IF_EXIST(name)					\
	do {								\
		*(void **)(&libc_##name) = dlsym(RTLD_NEXT, #name);	\
	} while (0)

#define COND_ASSIGN_DLSYM_OR_DIE(name)					\
	do {								\
		if (libc_##name == NULL) {				\
			madvmerge_page_init();				\
			ASSIGN_DLSYM_IF_EXIST(name);			\
			if (libc_##name == NULL)			\
				_exit(1);				\
		}							\
	} while (0)

#ifdef MY_DEBUG
#define DEBUG_PERROR(msg)	do { if (msg != NULL) perror(msg); } while (0)
#else
#define DEBUG_PERROR(msg)	do { } while (0)
#endif

static void *(*libc_malloc)(size_t) = NULL;
static void *(*libc_realloc)(void *, size_t) = NULL;
static int (*libc_brk)() = NULL;
static void *(*libc_sbrk)(intptr_t) = NULL;
static void *(*libc_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static void *(*libc_mremap)(void *, size_t, size_t, int flags, ...) = NULL;
#ifdef TRY_CALLOC
static void *(*libc_calloc)(size_t, size_t) = NULL;
#endif
static void *(*libc_valloc)(size_t) = NULL;
static void *(*libc_memalign)(size_t, size_t) = NULL;
static int (*libc_posix_memalign)(void **, size_t, size_t) = NULL;
static void *(*libc_mmap2)(void *, size_t, int, int, int, off_t) = NULL;
#ifdef TRY_MPROTECT
static int (*libc_mprotect)(void *, size_t, int) = NULL;
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 0
#endif

static long pagesize = PAGE_SIZE;
static uintptr_t page_base_mask;
static uintptr_t page_offset_mask;

void madvmerge_page_init()
{
	if (pagesize == 0) {
		int error = errno;
#ifdef _SC_PAGESIZE
		pagesize = sysconf(_SC_PAGESIZE);
#endif
		if (pagesize <= 0) {
			//if (pagesize < 0) perror("sysconf()");
			_exit(42);
		}
		errno = error;
	}
	page_offset_mask = pagesize - 1;
	page_base_mask = ~page_offset_mask;
}

void madvmerge_align(void *ptr, size_t *size, void **aligned)
{
	if (*size == 0) ++*size;
	*aligned = (void *)((uintptr_t)ptr & page_base_mask);
	*size = (*size + (uintptr_t)ptr - (uintptr_t)*aligned + page_offset_mask) & page_base_mask;
#if 0
	*size += (uintptr_t)ptr & page_offset_mask;
	if ((*size & page_offset_mask) != 0)
		*size = (*size & page_base_mask) + pagesize;
#endif
}

void madvmerge_madvise_mergeable_page_aligned(void *aligned, size_t size)
{
	int error = errno;
	if (size != 0 && madvise(aligned, size, MADV_MERGEABLE) != 0)
		DEBUG_PERROR("madvise()");
	errno = error;
}

void madvmerge_madvise_mergeable(void *ptr, size_t size)
{
	void *aligned;

	madvmerge_align(ptr, &size, &aligned);
	madvmerge_madvise_mergeable_page_aligned(aligned, size);
}

void __attribute__((constructor)) madvmerge_init()
{
	ASSIGN_DLSYM_IF_EXIST(malloc);
	ASSIGN_DLSYM_IF_EXIST(realloc);
	ASSIGN_DLSYM_IF_EXIST(brk);
	ASSIGN_DLSYM_IF_EXIST(sbrk);
	ASSIGN_DLSYM_IF_EXIST(mmap);
	ASSIGN_DLSYM_IF_EXIST(mremap);
#ifdef TRY_CALLOC
	ASSIGN_DLSYM_IF_EXIST(calloc);
#endif
	ASSIGN_DLSYM_IF_EXIST(valloc);
	ASSIGN_DLSYM_IF_EXIST(memalign);
	ASSIGN_DLSYM_IF_EXIST(posix_memalign);
	ASSIGN_DLSYM_IF_EXIST(mmap2);
#ifdef TRY_MPROTECT
	ASSIGN_DLSYM_IF_EXIST(mprotect);
#endif

	madvmerge_page_init();

	if (sizeof(uintptr_t) > 32) {
		madvmerge_madvise_mergeable_page_aligned(NULL + pagesize, (1ull << (40 - 1)) - pagesize);
		madvmerge_madvise_mergeable_page_aligned(NULL + pagesize, (1ull << (48 - 1)) - pagesize);
		madvmerge_madvise_mergeable_page_aligned(NULL + pagesize, (1ull << (64 - 1)) - pagesize);
	} else {
		madvmerge_madvise_mergeable_page_aligned(NULL + pagesize, (1ull << (32 - 1)) - pagesize);
		madvmerge_madvise_mergeable_page_aligned(NULL + pagesize, (1ull << (32 - 1)) + (1ull << (32 - 2)) - pagesize);
	}
}

void *malloc(size_t size)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(malloc);
	ptr = libc_malloc(size);
	if (ptr != NULL)
		madvmerge_madvise_mergeable(ptr, size);
	return ptr;
}

void *realloc(void *oldptr, size_t size)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(realloc);
	ptr = libc_realloc(oldptr, size);
	if (ptr != NULL)
		madvmerge_madvise_mergeable(ptr, size);
	return ptr;
}

int brk(void *ptr)
{
	int ret;
	void *prev;

	COND_ASSIGN_DLSYM_OR_DIE(sbrk);
	COND_ASSIGN_DLSYM_OR_DIE(brk);
	prev = libc_sbrk(0);
	ret = libc_brk(ptr);
	if (ret == 0 && (uintptr_t)ptr > (uintptr_t)prev)
		madvmerge_madvise_mergeable(prev, (intptr_t)ptr - (intptr_t)prev);
	return ret;
}

void *sbrk(intptr_t increment)
{
	void *prev;

	COND_ASSIGN_DLSYM_OR_DIE(sbrk);
	prev = libc_sbrk(increment);
	if (prev != MAP_FAILED && increment > 0)
		madvmerge_madvise_mergeable(prev, increment);
	return prev;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offs)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(mmap);
	ptr = libc_mmap(addr, len, prot, flags, fd, offs);
	if (ptr != MAP_FAILED)
		madvmerge_madvise_mergeable_page_aligned(ptr, len);
	return ptr;
}

void *mremap(void *oldaddr, size_t oldsize, size_t newsize, int flags, ...)
{
	va_list ap;
	void *ptr, *newaddr;

	va_start(ap, flags);
	COND_ASSIGN_DLSYM_OR_DIE(mremap);
	if ((flags & MREMAP_MAYMOVE) == 0) {
		ptr = libc_mremap(oldaddr, oldsize, newsize, flags);
	} else {
		newaddr = va_arg(ap, void *);
		ptr = libc_mremap(oldaddr, oldsize, newsize, flags, newaddr);
	}
	if (ptr != MAP_FAILED)
		madvmerge_madvise_mergeable(ptr, newsize);
	va_end(ap);
	return ptr;
}

#ifdef TRY_CALLOC

static /* __thread */ int madvmerge_count = 0;

static char madvmerge_array[65536];

void *calloc(size_t nmemb, size_t size)
{
	void *ptr;

	if (libc_calloc == NULL && madvmerge_count++ == 0) {
		ptr = (nmemb * size > sizeof madvmerge_array) ? NULL : madvmerge_array;
	} else {
		COND_ASSIGN_DLSYM_OR_DIE(calloc);

		ptr = libc_calloc(nmemb, size);
		if (ptr != NULL)
			madvmerge_madvise_mergeable(ptr, nmemb * size);

#if 0
		COND_ASSIGN_DLSYM_OR_DIE(malloc);

		size_t len = nmemb * size;

		ptr = libc_malloc(len);
		if (ptr != NULL && nmemb != 0 && size != 0) {
			memset(ptr, 0, len);
			madvmerge_madvise_mergeable(ptr, len);
		}
#endif
	}
	return ptr;
}

#endif

void *valloc(size_t size)
{
	void *aligned;

	COND_ASSIGN_DLSYM_OR_DIE(valloc);
	aligned = libc_valloc(size);
	if (aligned != NULL)
		madvmerge_madvise_mergeable_page_aligned(aligned, size);
	return aligned;
}

void *memalign(size_t boundary, size_t size)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(memalign);
	ptr = libc_memalign(boundary, size);
	if (ptr != NULL)
		madvmerge_madvise_mergeable(ptr, size);
	return ptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret;

	COND_ASSIGN_DLSYM_OR_DIE(posix_memalign);
	ret = libc_posix_memalign(memptr, alignment, size);
	if (ret == 0)
		madvmerge_madvise_mergeable(*memptr, size);
	return ret;
}

void *mmap2(void *addr, size_t len, int prot, int flags, int fd, off_t pgoffs)
{
	void *ptr;

	COND_ASSIGN_DLSYM_OR_DIE(mmap2);
	ptr = libc_mmap2(addr, len, prot, flags, fd, pgoffs);
	if (ptr != MAP_FAILED)
		madvmerge_madvise_mergeable_page_aligned(ptr, len);
	return ptr;
}

#ifdef TRY_MPROTECT

int mprotect(void *addr, size_t len, int prot)
{
	int ret;

	COND_ASSIGN_DLSYM_OR_DIE(mprotect);
	ret = libc_mprotect(addr, len, prot);
	if (ret == 0 && prot != PROT_NONE)
		madvmerge_madvise_mergeable_page_aligned(addr, len);
	return ret;
}

#endif

