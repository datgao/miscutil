#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

static volatile sig_atomic_t quit = 0;

static void kira_sigfn(int signo)
{
	(void)signo;

	quit = 1;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE, pid, signum, s, i, k = 1, busy_msec = 1000 / 30, idle_msec = busy_msec, msec;
	struct timespec ts;
	struct sigaction sa;

	if (argc > 2) busy_msec = strtol(argv[k++], NULL, 10);

	if (argc > 3) idle_msec = strtol(argv[k++], NULL, 10);

	if (busy_msec <= 0 || idle_msec <= 0 || sigemptyset(&sa.sa_mask) != 0) goto fail;
	sa.sa_handler = &kira_sigfn;
	if (sigaction(SIGINT, &sa, NULL) != 0) { perror("sigaction()"); goto fail; }

	while (quit == 0) {
		for (s = 0, msec = idle_msec, signum = SIGSTOP; s < 2; s++, msec = busy_msec, signum = SIGCONT) {
			for (i = k; i < argc; i++) {
				pid = strtol(argv[i], NULL, 10);
				if (pid <= 1) goto fail;
				if (kill(pid, signum) != 0) { perror("kill()"); goto fail; }
			}
			ts.tv_sec = msec / 1000;
			ts.tv_nsec = msec % 1000 * 1000000;
			while (quit == 0 && (ts.tv_sec != 0 || ts.tv_nsec != 0) && nanosleep(&ts, &ts) != 0) {
				if (errno != EINTR) { perror("nanosleep()"); goto fail; }
			}
		}
	}

	ret = EXIT_SUCCESS;
fail:
	return ret;
}
