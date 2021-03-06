/*
	This file is part of miscutil.
	Copyright (C) 2017-2018, Robert L. Thompson

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
#include <ucontext.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/user.h>	// PAGE_SIZE
#include <limits.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef MAX
# define MAX(x, y)	((x) > (y) ? (x) : (y))
#endif

#ifndef MIN
# define MIN(x, y)	((x) < (y) ? (x) : (y))
#endif

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(a)	(sizeof(a)/sizeof(*(a)))
#endif

#if defined(__x86_64__)
# define	ASM_AMD64
# define	ASM_STR_AX	"rax"
# define	ASM_REG_IP	REG_RIP
# define	ASM_REG_SP	REG_RSP
# define	ASM_CR2(uctx)	((uctx)->uc_mcontext.gregs[REG_CR2])
#elif defined(__i386__)
# define	ASM_X86
# define	ASM_STR_AX	"eax"
# define	ASM_REG_IP	REG_EIP
# define	ASM_REG_SP	REG_ESP
# define	ASM_CR2(uctx)	((uctx)->uc_mcontext.cr2)
#else
# error "Unknown CPU architecture."
#endif

#if defined(PAGE_SIZE) && (PAGE_SIZE) > 0 && ((PAGE_SIZE) & ((PAGE_SIZE) - 1)) == 0
# define ASM_PAGE_SIZE		(PAGE_SIZE)
# define ASM_PAGE_SIZE_ALT	(ctx.pagesz)
#else
# define ASM_PAGE_SIZE		(ctx.pagesz)
# ifdef PAGE_SIZE
#  warning	"Have PAGE_SIZE #define but value is unreasonable."
# else
#  warning	"Missing PAGE_SIZE #define."
# endif
# warning	"Will use sysconf(_SC_PAGESIZE)."
#endif

enum asm_pg { ASM_PG_RW_INSN, ASM_PG_XR_INSN, ASM_PG_NONE_GUARD, ASM_PG_NONE_STACK, ASM_PGS };

#define ASM_OFFT_RW_INSN	(ASM_PG_RW_INSN * ASM_PAGE_SIZE)
#define ASM_OFFT_RW_INSN_END	(ASM_OFFT_RW_INSN + ASM_PAGE_SIZE)
#define ASM_OFFT_XR_INSN	(ASM_PG_XR_INSN * ASM_PAGE_SIZE)
#define ASM_OFFT_XR_INSN_END	(ASM_OFFT_XR_INSN + ASM_PAGE_SIZE)
#define ASM_OFFT_NONE_GUARD	(ASM_PG_NONE_GUARD * ASM_PAGE_SIZE)
#define ASM_OFFT_NONE_GUARD_END	(ASM_OFFT_NONE_GUARD + ASM_PAGE_SIZE)
#define ASM_OFFT_NONE_STACK	(ASM_PG_NONE_STACK * ASM_PAGE_SIZE)
#define ASM_OFFT_NONE_STACK_END	(ASM_OFFT_NONE_STACK + ASM_PAGE_SIZE)
#define ASM_OFFT_LEN		(ASM_PGS * ASM_PAGE_SIZE)

#define ASM_PTR_BASE		((uint8_t *)ctx.pgspan)
#define ASM_PTR_BASE_CONST	((const uint8_t *)ctx.pgspan)
#define ASM_PTR_RW_INSN		(ASM_PTR_BASE + ASM_OFFT_RW_INSN)
#define ASM_PTR_RW_INSN_END	(ASM_PTR_BASE + ASM_OFFT_RW_INSN_END)
#define ASM_PTR_RW_INSN_CONST	(ASM_PTR_BASE_CONST + ASM_OFFT_RW_INSN)
#define ASM_PTR_RW_INSN_END_CONST	(ASM_PTR_BASE_CONST + ASM_OFFT_RW_INSN_END)
#define ASM_PTR_XR_INSN		(ASM_PTR_BASE + ASM_OFFT_XR_INSN)
#define ASM_PTR_XR_INSN_END	(ASM_PTR_BASE + ASM_OFFT_XR_INSN_END)
#define ASM_PTR_XR_INSN_CONST	(ASM_PTR_BASE_CONST + ASM_OFFT_XR_INSN)
#define ASM_PTR_XR_INSN_END_CONST	(ASM_PTR_BASE_CONST + ASM_OFFT_XR_INSN_END)
#define ASM_PTR_NONE_GUARD	(ASM_PTR_BASE + ASM_OFFT_NONE_GUARD)
#define ASM_PTR_NONE_GUARD_END	(ASM_PTR_BASE + ASM_OFFT_NONE_GUARD_END)
#define ASM_PTR_NONE_STACK	(ASM_PTR_BASE + ASM_OFFT_NONE_STACK)
#define ASM_PTR_NONE_STACK_END	(ASM_PTR_BASE + ASM_OFFT_NONE_STACK_END)

typedef void sigaction_fn(int, siginfo_t *, void *);

enum asm_sig { ASM_SIG_BUS, ASM_SIG_FPE, ASM_SIG_ILL, ASM_SIG_SEGV, ASM_SIG_TRAP, ASM_SIGS };

struct asm_sig_ctx {
	int		signum;
	sigaction_fn	*sigfn;
};

struct asm_ctx {
	struct asm_sig_ctx	sigctx[ASM_SIGS];

	void		*altstack;
	size_t		altsz;

	long		pagesz;
	void		*pgspan;	// rwalias, exec, none, none
	unsigned	insnlen;

	unsigned	minlen;
	unsigned	maxlen;
	bool		depth;
	bool		verbose;

	void		*rip_rsm;
	void		*rsp_rsm;
	uint16_t	ds_rsm;
	uint16_t	es_rsm;
	uint16_t	fs_rsm;
	uint16_t	gs_rsm;
	gregset_t	gregs_rsm;
	stack_t		stack_rsm;

	gregset_t	gregs;
	stack_t		stack;

	jmp_buf		jmpenv;
	bool		jmpvalid;

	bool		nested;
};

static void sigfn_bus(int signum, siginfo_t *siginfo, void *puctx);
static void sigfn_fpe(int signum, siginfo_t *siginfo, void *puctx);
static void sigfn_ill(int signum, siginfo_t *siginfo, void *puctx);
static void sigfn_segv(int signum, siginfo_t *siginfo, void *puctx);
static void sigfn_trap(int signum, siginfo_t *siginfo, void *puctx);

static struct asm_ctx ctx = {	// init
	.sigctx = {
		[ASM_SIG_BUS]  = { .signum = SIGBUS,  .sigfn = &sigfn_bus },
		[ASM_SIG_FPE]  = { .signum = SIGFPE,  .sigfn = &sigfn_fpe },
		[ASM_SIG_ILL]  = { .signum = SIGILL,  .sigfn = &sigfn_ill },
		[ASM_SIG_SEGV] = { .signum = SIGSEGV, .sigfn = &sigfn_segv },
		[ASM_SIG_TRAP] = { .signum = SIGTRAP, .sigfn = &sigfn_trap },
	},
	.altstack = MAP_FAILED,
	.altsz = 0,

	.pagesz = -1,
	.pgspan = MAP_FAILED,
	.insnlen = 0,

	.minlen = 1,
	.maxlen = 15,
	.depth = false,
	.verbose = false,

	.rip_rsm = NULL,
	.rsp_rsm = NULL,
	.ds_rsm = 0,
	.es_rsm = 0,
	.fs_rsm = 0,
	.gs_rsm = 0,

	.jmpvalid = false,
	.nested = false,
};

extern void asm_test();

__asm__(".global asm_test\ncall asm_sig_print\nret\n");

static void asm_insn_print()
{
	unsigned i;
	const uint8_t *pfirst = ASM_PTR_RW_INSN_END_CONST - ctx.insnlen;

	for (i = 0; i < ctx.insnlen; i++)
		printf(" %02"PRIx8, *pfirst++);

	printf("\n");
	fflush(stdout);
}

static bool asm_bad(unsigned len, const uint8_t *pfirst, const uint8_t *plast)
{
	bool ret = false;

#ifdef ASM_AMD64

	if (--len > 0) {	// (Intel) MOV FS, GPreg / (AT&T) MOV %GPreg, %FS
		if (pfirst[0] == 0x8e && (pfirst[1] & 0x38) == 0x20)
			goto bad;
		if (plast[-1] == 0x8e && (plast[0] & 0x38) == 0x20)
			goto bad;
	}

	while (len-- > 0) {	// FS: override prefix
		if (*pfirst++ == 0x64)
			goto bad;
	}

#else

	if (--len > 0) {	// (Intel) MOV Sreg, GPreg / (AT&T) MOV %GPreg, %Sreg
		if (pfirst[0] == 0x8e)
			goto bad;
		if (plast[-1] == 0x8e)
			goto bad;
	}

	while (len-- > 0) {	// FS:/GS: or ES:/CS:/SS:/DS: override prefix
		if ((*pfirst & 0xfe) == 0x64)// || (*pfirst & 0xe7) == 0x26)
			goto bad;
		pfirst++;
	}

#endif

out:
	return ret;
bad:
	ret = true;
	goto out;
}

static void asm_insn_inc(int signum, const siginfo_t *siginfo, ucontext_t *uctx)
{
	uint8_t *ptr = NULL, *pend = ASM_PTR_RW_INSN_END, *plast = pend - 1, *pterm = plast, *pfirst = plast + 1 - ctx.insnlen;
	const void *const pxend = (const void *)ASM_PTR_XR_INSN_END_CONST;//pend + ASM_OFFT_XR_INSN_END - ASM_OFFT_RW_INSN_END;
	unsigned span;

	if (ctx.depth && signum == SIGTRAP && siginfo->si_code == TRAP_TRACE) {
		if (siginfo->si_addr != pxend) {
			if (siginfo->si_addr < pxend + 127 + ctx.insnlen
			&& siginfo->si_addr >= pxend - 128 + ctx.insnlen) {
				span = 0;
				if (pxend > siginfo->si_addr)
					span = pxend - siginfo->si_addr;
				if (span == 0 || span >= ctx.insnlen)
					span = ctx.insnlen - 1;
				plast = pend - span;
				ptr = plast--;
				memset(ptr, 0, pend - ptr);
			}
		}
	}

again:
	if (ctx.depth) {
		for (ptr = plast ; ptr >= pfirst; ptr--) {
			if ((*ptr)++ < 0xff) {
				goto resume;
			}
		}
	} else {
		// 0x00-0xff then 0x00-0xff:0x00 - 0x00-0xff:0xff i.e. increment as little-endian
		for (ptr = pfirst; ptr <= plast; ptr++) {
			if ((*ptr)++ < 0xff) {
				goto resume;
			}
		}
	}

	*(--pfirst) = 0x00;
	if (ctx.insnlen++ >= ctx.maxlen)
		goto finish;

resume:
	plast = pterm;
	if (asm_bad(ctx.insnlen, pfirst, pterm))
		goto again;
	memcpy(uctx->uc_mcontext.gregs, ctx.gregs, sizeof ctx.gregs);
	memcpy(&uctx->uc_stack, &ctx.stack, sizeof ctx.stack);
	uctx->uc_mcontext.gregs[ASM_REG_IP] = (uintptr_t)(pxend - ctx.insnlen);
	uctx->uc_mcontext.gregs[ASM_REG_SP] = (uintptr_t)ASM_PTR_NONE_STACK;

#if 0
#ifdef ASM_AMD64
	asm volatile (
		"mov $0, %%ax\n"
		"mov %%ax, %%ds\n"
		"mov %%ax, %%es\n"
#ifdef ASM_X86
		"mov %%ax, %%fs\n"
#endif
		"mov %%ax, %%gs\n"
		:
		:
		: ASM_STR_AX);
#endif
#endif

	goto out;

finish:
	memcpy(uctx->uc_mcontext.gregs, ctx.gregs_rsm, sizeof ctx.gregs_rsm);
	memcpy(&uctx->uc_stack, &ctx.stack_rsm, sizeof ctx.stack_rsm);
	uctx->uc_mcontext.gregs[ASM_REG_IP] = (uintptr_t)ctx.rip_rsm;
	uctx->uc_mcontext.gregs[ASM_REG_SP] = (uintptr_t)ctx.rsp_rsm;
out:
	return;
}

static int asm_sig_addr(char *buf, size_t size, const void *vptr)
{
	int ret = -1;
	const uint8_t *addr = vptr;

	if (addr == NULL)
		ret = snprintf(buf, size, "0");
	else if (addr >= ASM_PTR_XR_INSN_CONST && addr <= ASM_PTR_XR_INSN_END_CONST + 127)
		ret = snprintf(buf, size, "/ %+ld", (long)(addr - ASM_PTR_XR_INSN_END_CONST));
	else if (addr >= ASM_PTR_NONE_GUARD && addr <= ASM_PTR_NONE_STACK_END)
		ret = snprintf(buf, size, "_ %+ld", (long)(addr - ASM_PTR_NONE_STACK));
	else
		ret = snprintf(buf, size, "%016lx", (unsigned long)(uintptr_t)addr);

	return ret;
}

static void asm_sig_print(int signum, const siginfo_t *siginfo, void *puctx)
{
#if 0
	if (!ctx.jmpvalid && setjmp(ctx.jmpenv) != 0) {
		longjmp(ctx.jmpenv, 1);
	}
	ctx.jmpvalid = true;
#endif

	int sicode = siginfo->si_code;
	const void *siaddr = siginfo->si_addr;
	char addr[32] = "(nil)", tmp[32] = "", cr2[32] = "", rip[32] = "", rsp[32] = "";
	const char *name = "", *code = "";
	ucontext_t *uctx = (ucontext_t *)puctx;

#if 0
	asm volatile(
		"mov %0, %%ax\n"
		"mov %%ax, %%ds\n"
		"mov %1, %%ax\n"
		"mov %%ax, %%es\n"
		"mov %2, %%ax\n"
		"mov %%ax, %%gs\n"
#ifdef ASM_X86
		"mov %3, %%ax\n"
		"mov %%ax, %%fs\n"
#endif
		:
		: "g" (ctx.ds_rsm), "g" (ctx.es_rsm), "g" (ctx.gs_rsm)
#ifdef ASM_X86
		, "g" (ctx.fs_rsm)
#endif
		: ASM_STR_AX);
#endif

	if (ctx.nested) {
		printf("nested\n");
		return;
	}
	ctx.nested = true;

	if (signum == SIGBUS) {
		name = "bus ";
		if (sicode == BUS_ADRALN)
			code = "algn";
		if (sicode == BUS_ADRERR)
			code = "nxst";
		if (sicode == BUS_OBJERR)
			code = "ohwe";
	}

	if (signum == SIGILL) {
		name = "fpe ";
		if (sicode == FPE_FLTDIV)
			code = "fdiv";
		if (sicode == FPE_FLTINV)
			code = "finv";
		if (sicode == FPE_FLTOVF)
			code = "fovf";
		if (sicode == FPE_FLTRES)
			code = "xres";
		if (sicode == FPE_FLTSUB)
			code = "subs";
		if (sicode == FPE_FLTUND)
			code = "fudf";
		if (sicode == FPE_INTDIV)
			code = "idiv";
		if (sicode == FPE_INTOVF)
			code = "iovf";
	}

	if (signum == SIGILL) {
		name = "ill ";
		if (sicode == ILL_BADSTK)
			code = "bstk";
		if (sicode == ILL_COPROC)
			code = "cpro";
		if (sicode == ILL_ILLADR)
			code = "addr";
		if (sicode == ILL_ILLOPC)
			code = "opcd";
		if (sicode == ILL_ILLOPN)
			code = "opan";
		if (sicode == ILL_ILLTRP)
			code = "trp-";
		if (sicode == ILL_PRVOPC)
			code = "priv";
		if (sicode == ILL_PRVREG)
			code = "preg";
	}

	if (signum == SIGSEGV) {
		name = "segv";
		if (sicode == SEGV_MAPERR)
			code = "invd";
		if (sicode == SEGV_ACCERR)
			code = "perm";
	}

	if (signum == SIGTRAP) {
		name = "trap";
		if (sicode == TRAP_BRKPT)
			code = "bkpt";
		if (sicode == TRAP_TRACE)
			code = "trce";
#if 0
		if (sicode == TRAP_BRANCH)
			code = "brch";
		if (sicode == TRAP_HWBKPT)
			code = "hwbp";
#endif

		if (ctx.rip_rsm == NULL) {
			ctx.rip_rsm = (void *)(uintptr_t)uctx->uc_mcontext.gregs[ASM_REG_IP];
			ctx.rsp_rsm = (void *)(uintptr_t)uctx->uc_mcontext.gregs[ASM_REG_SP];
			memcpy(ctx.gregs_rsm, uctx->uc_mcontext.gregs, sizeof ctx.gregs_rsm);
			memcpy(&ctx.stack_rsm, &uctx->uc_stack, sizeof ctx.stack_rsm);
			printf("Stashing stack %p\n", ctx.stack_rsm.ss_sp);

			memset(&uctx->uc_stack, 0, sizeof ctx.stack);
			memcpy(&ctx.stack, &uctx->uc_stack, sizeof ctx.stack);

			ctx.insnlen = ctx.minlen;	// already zero page
			uctx->uc_mcontext.gregs[ASM_REG_IP] = (uintptr_t)(ASM_PTR_XR_INSN_END_CONST - ctx.insnlen);
			uctx->uc_mcontext.gregs[ASM_REG_SP] = (uintptr_t)ASM_PTR_NONE_STACK;

			memset(ctx.gregs, 0, sizeof ctx.gregs);
			ctx.gregs[REG_EFL] = 1 << 8;
#ifdef ASM_AMD64
			ctx.gregs[REG_CSGSFS] = uctx->uc_mcontext.gregs[REG_CSGSFS] & 0xffff;
#else
			ctx.gregs[REG_CS] = uctx->uc_mcontext.gregs[REG_CS] & 0xffff;
			ctx.gregs[REG_GS] = uctx->uc_mcontext.gregs[REG_GS] & 0xffff;
			ctx.gregs[REG_FS] = uctx->uc_mcontext.gregs[REG_FS] & 0xffff;
			ctx.gregs[REG_DS] = uctx->uc_mcontext.gregs[REG_DS] & 0xffff;
			ctx.gregs[REG_ES] = uctx->uc_mcontext.gregs[REG_ES] & 0xffff;
			ctx.gregs[REG_SS] = uctx->uc_mcontext.gregs[REG_SS] & 0xffff;
#endif
			ctx.gregs[ASM_REG_IP] = uctx->uc_mcontext.gregs[ASM_REG_IP];
			ctx.gregs[ASM_REG_SP] = uctx->uc_mcontext.gregs[ASM_REG_SP];
			memcpy(uctx->uc_mcontext.gregs, ctx.gregs, sizeof ctx.gregs);

			printf("will resume @ %p [%p], exec = %p [%p]\n"
			, ctx.rip_rsm, ctx.rsp_rsm, ASM_PTR_XR_INSN_END_CONST - ctx.insnlen, ASM_PTR_NONE_STACK);
			goto out;
		}
	}

	if (!ctx.verbose) {
		if (signum == SIGILL && sicode == ILL_ILLOPN)
			goto skip;

		if (signum == SIGSEGV && sicode == SEGV_ACCERR
		&& siaddr == ASM_PTR_NONE_GUARD)
			goto skip;
	}

	asm_sig_addr(addr, sizeof addr, siaddr);
	asm_sig_addr(cr2, sizeof cr2, (const void *)(uintptr_t)ASM_CR2(uctx));
	asm_sig_addr(rip, sizeof rip, (const void *)(uintptr_t)uctx->uc_mcontext.gregs[ASM_REG_IP]);
	asm_sig_addr(rsp, sizeof rsp, (const void *)(uintptr_t)uctx->uc_mcontext.gregs[ASM_REG_SP]);

	if (code == NULL || code[0] == '\0') {
		snprintf(tmp, sizeof tmp, "#%d", sicode);
		code = tmp;
	}

	printf("%c %3u\t%4s ? %8s   ^ %02lx   $ %02lx   # %18s   @ %18s = "
	, (void *)(uintptr_t)uctx->uc_mcontext.gregs[ASM_REG_IP] == ASM_PTR_XR_INSN_END_CONST - ctx.insnlen ? ' ' : '*'
	, ctx.insnlen, name, code, (unsigned long)uctx->uc_mcontext.gregs[REG_ERR]
	, (unsigned long)uctx->uc_mcontext.gregs[REG_TRAPNO], cr2, addr);

	asm_insn_print();

skip:
	asm_insn_inc(signum, siginfo, uctx);
out:
	ctx.nested = false;
}

static void sigfn_bus(int signum, siginfo_t *siginfo, void *puctx)
{
	asm_sig_print(signum, siginfo, puctx);
}

static void sigfn_fpe(int signum, siginfo_t *siginfo, void *puctx)
{
	asm_sig_print(signum, siginfo, puctx);
}

static void sigfn_ill(int signum, siginfo_t *siginfo, void *puctx)
{
	asm_sig_print(signum, siginfo, puctx);
}

static void sigfn_segv(int signum, siginfo_t *siginfo, void *puctx)
{
	asm_sig_print(signum, siginfo, puctx);
}

static void sigfn_trap(int signum, siginfo_t *siginfo, void *puctx)
{
	asm_sig_print(signum, siginfo, puctx);
}

static int sighandle(struct asm_sig_ctx *sigctx, sigset_t *sigset)
{
	int ret = -1;
	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sa.sa_sigaction = sigctx->sigfn;

	if (sigemptyset(&sa.sa_mask) != 0
	|| sigaction(sigctx->signum, &sa, NULL) != 0
	|| sigaddset(sigset, sigctx->signum) != 0)
		goto fail;

	ret = 0;
fail:
	return ret;
}

static int sigsetup(struct asm_ctx *ctx)
{
	int ret = -1;
	unsigned i;
	sigset_t sigset;
	stack_t sigstack;

	ctx->altsz = MAX(SIGSTKSZ, 256 * 1024);
	ctx->altstack = mmap(NULL, ctx->altsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ctx->altstack == MAP_FAILED)
		goto fail;

	sigstack.ss_flags = 0;
	sigstack.ss_sp = ctx->altstack;
	sigstack.ss_size = ctx->altsz;
	if (sigaltstack(&sigstack, NULL) != 0)
		goto fail;

	if (sigemptyset(&sigset) != 0)
		goto fail;

	for (i = 0; i < ARRAY_SIZE(ctx->sigctx); i++)
		if (sighandle(&ctx->sigctx[i], &sigset) != 0)
			goto fail;

	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL) != 0)
		goto fail;

	ret = 0;
fail:
	return ret;
}

static unsigned strtounum(const char *str)
{
	unsigned ret = 0;
	long val;
	char *ep = NULL;

	val = strtol(str, &ep, 0);
	if (val <= 0 || val > (long)MIN((long long)UINT_MAX, (long long)LONG_MAX) || ep == NULL || *ep != '\0')
		goto fail;

	ret = val;
fail:
	return ret;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE, ch, shmfd = -1;
	char shmname[32];
	struct timespec tsreal, tsmono;
	uint16_t cs, ss;

	while ((ch = getopt(argc, argv, "m:n:sv")) >= 0) {
		switch (ch) {
		case 'm':
			ctx.minlen = strtounum(optarg);
			if (ctx.minlen == 0)
				goto usage;
			break;
		case 'n':
			ctx.maxlen = strtounum(optarg);
			if (ctx.maxlen == 0)
				goto usage;
			break;
		case 's':
			ctx.depth = true;
			break;
		case 'v':
			ctx.verbose = true;
			break;
		default:
			goto usage;
		}
	}

	if (ctx.minlen > ctx.maxlen)
		goto usage;

	ctx.pagesz = sysconf(_SC_PAGESIZE);
	if (ctx.pagesz <= 0)
		goto fail;

#ifdef ASM_PAGE_SIZE_ALT
	if (ASM_PAGE_SIZE != ASM_PAGE_SIZE_ALT)
		goto fail;
#endif

	if (clock_gettime(CLOCK_REALTIME, &tsreal) != 0
	|| clock_gettime(CLOCK_MONOTONIC, &tsmono) != 0)
		goto fail;
	srandom(tsreal.tv_sec ^ tsreal.tv_nsec ^ tsmono.tv_sec ^ tsmono.tv_nsec);
	snprintf(shmname, sizeof shmname, "/%llx", (unsigned long long)(getpid() ^ random()));

	shmfd = shm_open(shmname, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (shmfd < 0
	|| shm_unlink(shmname) != 0
	|| ftruncate(shmfd, ASM_OFFT_LEN) != 0)
		goto fail;

#define ASM_PROT_RW	(PROT_READ | PROT_WRITE)
#define ASM_PROT_XR	(PROT_EXEC | PROT_READ)
#define ASM_MAP_RSV	(MAP_PRIVATE | MAP_ANONYMOUS)
#define ASM_REMAP_RSV	(ASM_MAP_RSV | MAP_FIXED)
#define ASM_REMAP_SHR	(MAP_SHARED | MAP_FIXED)

	ctx.pgspan = mmap(NULL, ASM_OFFT_LEN, ASM_PROT_XR, ASM_MAP_RSV, -1, 0);
	if (ctx.pgspan == MAP_FAILED)
		goto fail;

	if (mmap(ASM_PTR_RW_INSN, ASM_PAGE_SIZE, ASM_PROT_RW, ASM_REMAP_SHR, shmfd, 0) != ASM_PTR_RW_INSN
	|| mmap(ASM_PTR_XR_INSN, ASM_PAGE_SIZE, ASM_PROT_XR, ASM_REMAP_SHR, shmfd, 0) != ASM_PTR_XR_INSN
	|| mmap(ASM_PTR_NONE_GUARD, 2 * ASM_PAGE_SIZE, PROT_NONE, ASM_REMAP_RSV, -1, 0) != ASM_PTR_NONE_GUARD)
		goto fail;

	if (sigsetup(&ctx) != 0)
		goto fail;

	asm volatile(
		"mov %%ds, %%ax\n"
		"mov %%ax, %0\n"
		"mov %%es, %%ax\n"
		"mov %%ax, %1\n"
		"mov %%gs, %%ax\n"
		"mov %%ax, %2\n"
		"mov %%fs, %%ax\n"
		"mov %%ax, %3\n"
		: "=g" (ctx.ds_rsm), "=g" (ctx.es_rsm), "=g" (ctx.gs_rsm), "=g" (ctx.fs_rsm)
		:
		: ASM_STR_AX);

	asm volatile(
		"mov %%cs, %%ax\n"
		"mov %%ax, %0\n"
		"mov %%ss, %%ax\n"
		"mov %%ax, %1\n"
		: "=g" (cs), "=g" (ss)
		:
		: ASM_STR_AX);

	printf("GOT: cs = %x, ds = %x, es = %x, fs = %x, gs = %x, ss = %x\n"
	, (unsigned)cs, (unsigned)ctx.ds_rsm, (unsigned)ctx.es_rsm, (unsigned)ctx.fs_rsm, (unsigned)ctx.gs_rsm, (unsigned)ss);

#if 0
#ifdef ASM_AMD64
	asm volatile (
		"mov $0, %%ax\n"
		"mov %%ax, %%ds\n"
		"mov %%ax, %%es\n"
#ifdef ASM_X86_
		"mov %%ax, %%fs\n"
#endif
		"mov %%ax, %%gs\n"
		:
		:
		: ASM_STR_AX);
#endif
#endif

	asm volatile ("int3");

	printf("finished\n");

	ret = EXIT_SUCCESS;
fail:
	if (ctx.pgspan != MAP_FAILED && munmap(ASM_PTR_BASE, ASM_OFFT_LEN) != 0) {
		perror("munmap()");
		ret = EXIT_FAILURE;
	}
	if (shmfd >= 0 && close(shmfd) != 0) {
		perror("close()");
		ret = EXIT_FAILURE;
	}
	return ret;
usage:
	fprintf(stderr, "%s [ -v (verbose) ] [ -s (depth-first) ] [ -m min ] [ -n max ]\n", argv[0]);
	goto fail;
}
