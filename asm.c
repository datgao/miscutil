#define _GNU_SOURCE
#include <ucontext.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <stdint.h>
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

typedef void sigaction_fn(int, siginfo_t *, void *);

enum asm_sig {
	ASM_SIG_BUS,
	ASM_SIG_FPE,
	ASM_SIG_ILL,
	ASM_SIG_SEGV,
	ASM_SIG_TRAP,
	ASM_SIGS
};

struct asm_sig_ctx {
	int signum;
	sigaction_fn *sigfn;
};

struct asm_ctx {
	struct asm_sig_ctx sigctx[ASM_SIGS];

	void *altstack;
	size_t altsz;

	long pagesz;
	void *pgspan;	// rwalias, exec, none, none

	void *rip_rsm;
	void *rsp_rsm;

	unsigned minlen;
	unsigned maxlen;

	unsigned insnlen;
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

	.rip_rsm = NULL,
	.rsp_rsm = NULL,

	.minlen = 1,
	.maxlen = 15,

	.insnlen = 0,
};

static void asm_insn_print()
{
	unsigned i;
	const uint8_t *pfirst = (uint8_t *)ctx.pgspan + ctx.pagesz - ctx.insnlen;

	for (i = 0; i < ctx.insnlen; i++)
		printf(" %02"PRIx8, *pfirst++);

	printf("\n");
}

static void asm_insn_inc(ucontext_t *uctx)
{
	uint8_t *plast = (uint8_t *)ctx.pgspan + ctx.pagesz - 1;
	uint8_t *pfirst, *ptr;

	if (*plast < 0xff) {
		(*plast)++;
		uctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)(ctx.pgspan + 2 * ctx.pagesz - ctx.insnlen);
		return;
	}

	if (ctx.insnlen < 15) {
		pfirst = ctx.pgspan + ctx.pagesz - ctx.insnlen;

		for (ptr = plast; ptr >= pfirst; ptr--)
			if (*ptr < 0xff)
				break;

		if (ptr < pfirst) {
			memmove(ptr, pfirst, ctx.insnlen);
			ctx.insnlen++;
			*plast = 0x00;
			uctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)(ctx.pgspan + 2 * ctx.pagesz - ctx.insnlen);
			return;
		}

		memmove(pfirst + (plast - ptr), ptr, ctx.insnlen - (plast - ptr));
		(*plast)++;
		ctx.insnlen -= plast - ptr;
		uctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)(ctx.pgspan + 2 * ctx.pagesz - ctx.insnlen);
	}

	uctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)ctx.rip_rsm;
	uctx->uc_mcontext.gregs[REG_RSP] = (uintptr_t)ctx.rsp_rsm;
}

static void asm_sig_print(int signum, siginfo_t *siginfo, void *puctx)
{
	const char *name = "", *code = "";
	char buf[32] = "(nil)", tmp[32] = "";
	ucontext_t *uctx = (ucontext_t *)puctx;

	if (signum == SIGBUS) {
		name = "bus ";
		if (siginfo->si_code == BUS_ADRALN)
			code = "algn";
		if (siginfo->si_code == BUS_ADRERR)
			code = "nxst";
		if (siginfo->si_code == BUS_OBJERR)
			code = "ohwe";
	}

	if (signum == SIGILL) {
		name = "fpe ";
		if (siginfo->si_code == FPE_FLTDIV)
			code = "fdiv";
		if (siginfo->si_code == FPE_FLTINV)
			code = "finv";
		if (siginfo->si_code == FPE_FLTOVF)
			code = "fovf";
		if (siginfo->si_code == FPE_FLTRES)
			code = "xres";
		if (siginfo->si_code == FPE_FLTSUB)
			code = "subs";
		if (siginfo->si_code == FPE_FLTUND)
			code = "fudf";
		if (siginfo->si_code == FPE_INTDIV)
			code = "idiv";
		if (siginfo->si_code == FPE_INTOVF)
			code = "iovf";
	}

	if (signum == SIGILL) {
		name = "ill ";
		if (siginfo->si_code == ILL_BADSTK)
			code = "bstk";
		if (siginfo->si_code == ILL_COPROC)
			code = "cpro";
		if (siginfo->si_code == ILL_ILLADR)
			code = "addr";
		if (siginfo->si_code == ILL_ILLOPC)
			code = "opcd";
		if (siginfo->si_code == ILL_ILLOPN)
			code = "opan";
		if (siginfo->si_code == ILL_ILLTRP)
			code = "trp-";
		if (siginfo->si_code == ILL_PRVOPC)
			code = "priv";
		if (siginfo->si_code == ILL_PRVREG)
			code = "preg";
	}

	if (signum == SIGSEGV) {
		name = "segv";
		if (siginfo->si_code == SEGV_MAPERR)
			code = "invd";
		if (siginfo->si_code == SEGV_ACCERR)
			code = "perm";
	}

	if (signum == SIGTRAP) {
		name = "trap";
		if (siginfo->si_code == TRAP_BRKPT)
			code = "bkpt";
		if (siginfo->si_code == TRAP_TRACE)
			code = "trce";
#if 0
		if (siginfo->si_code == TRAP_BRANCH)
			code = "brch";
		if (siginfo->si_code == TRAP_HWBKPT)
			code = "hwbp";
#endif

		if (ctx.rip_rsm == NULL) {
			ctx.rip_rsm = (void *)(uintptr_t)uctx->uc_mcontext.gregs[REG_RIP];
			ctx.rsp_rsm = (void *)(uintptr_t)uctx->uc_mcontext.gregs[REG_RSP];
			uctx->uc_mcontext.gregs[REG_RSP] = (uintptr_t)(ctx.pgspan + 3 * ctx.pagesz);

			ctx.insnlen = 1;
			*((char *)ctx.pgspan + ctx.pagesz - ctx.insnlen) = 0x00;
			uctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)(ctx.pgspan + 2 * ctx.pagesz - ctx.insnlen);

			printf("will resume @ %p [%p], exec = %p [%p]\n", ctx.rip_rsm, ctx.rsp_rsm, ctx.pgspan + 2 * ctx.pagesz - ctx.insnlen, ctx.pgspan + 3 * ctx.pagesz);
			return;
		}
	}

	if ((signum != SIGILL || siginfo->si_code != ILL_ILLOPN)
	&& (signum != SIGSEGV || (siginfo->si_code != SEGV_ACCERR && siginfo->si_code != SEGV_MAPERR) || siginfo->si_addr != ctx.pgspan + 2 * ctx.pagesz)) {
		if (siginfo->si_addr == NULL)
			snprintf(buf, sizeof buf, "0");
		else if (siginfo->si_addr >= ctx.pgspan + ctx.pagesz && siginfo->si_addr <= ctx.pgspan + 2 * ctx.pagesz)
			snprintf(buf, sizeof buf, "%+16ld", (long)(siginfo->si_addr - ctx.pgspan - 2 * ctx.pagesz));
		else if (siginfo->si_addr >= ctx.pgspan + 3 * ctx.pagesz && siginfo->si_addr <= ctx.pgspan + 4 * ctx.pagesz)
			snprintf(buf, sizeof buf, "_%+16ld", (long)(siginfo->si_addr - ctx.pgspan - 3 * ctx.pagesz));
		else
			snprintf(buf, sizeof buf, "%016lx", (unsigned long)siginfo->si_addr);

		if (code == NULL || code[0] == '\0') {
			snprintf(tmp, sizeof tmp, "#%d", siginfo->si_code);
			code = tmp;
		}

		printf(":\t%4s ? %8s @ %18s = ", name, code, buf);

		asm_insn_print();
	}

	asm_insn_inc(uctx);
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

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE, shmfd = -1;
	char shmname[32];
	struct timespec tsreal, tsmono;

	(void)argc;
	(void)argv;

	ctx.pagesz = sysconf(_SC_PAGESIZE);
	if (ctx.pagesz <= 0)
		goto fail;

	if (clock_gettime(CLOCK_REALTIME, &tsreal) != 0
	|| clock_gettime(CLOCK_MONOTONIC, &tsmono) != 0)
		goto fail;
	srandom(tsreal.tv_sec ^ tsreal.tv_nsec ^ tsmono.tv_sec ^ tsmono.tv_nsec);
	snprintf(shmname, sizeof shmname, "/%llx", (unsigned long long)(getpid() ^ random()));

	shmfd = shm_open(shmname, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (shmfd < 0 || shm_unlink(shmname) != 0 || ftruncate(shmfd, ctx.pagesz * 4) != 0)
		goto fail;

	ctx.pgspan = mmap(NULL, ctx.pagesz * 4, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ctx.pgspan == MAP_FAILED)
		goto fail;
	if (mmap(ctx.pgspan + 0, ctx.pagesz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shmfd, 0) != ctx.pgspan + 0
	|| mmap(ctx.pgspan + ctx.pagesz, ctx.pagesz, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, shmfd, 0) != ctx.pgspan + ctx.pagesz
	|| mmap(ctx.pgspan + 2 * ctx.pagesz, 2 * ctx.pagesz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != ctx.pgspan + 2 * ctx.pagesz)
		goto fail;

	if (sigsetup(&ctx) != 0)
		goto fail;

#if 0
	int rv, errnum;
	uintptr_t uptr, count;

	for (count = 0, uptr = 0; count < (uintptr_t)1 << (sizeof(size_t) * CHAR_BIT - 20); count++, uptr += 1 << 20) {
		rv = munmap((void *)uptr, (size_t)1 << 20);
		if (rv != 0) {
			errnum = errno;
			break;
			printf("%zu\t%d\t%d\n", (size_t)count, rv, errnum);
		}
		printf("%zu\n", (size_t)count);
	}
#endif

	asm volatile (
		"int3"
	);

	printf("finished\n");

	ret = EXIT_SUCCESS;
fail:
	return ret;
}
