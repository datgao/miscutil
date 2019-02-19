#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>


static volatile sig_atomic_t quit = 0;


static void kira_sigfn(int signo)
{
	(void)signo;

	quit = 1;
}


static int kira_num2fdpid(const char *str)
{
	long num;
	bool intvl = false;
	char *ep = NULL;

	if (*str == '-') goto fail;

	if (*str == '+') { str++; intvl = true; }
	if (*str == '\0') goto fail;
	if (*str < '0' || *str > '9') goto success;

	num = strtol(str, &ep, 10);
	if (num <= 1 || num >= INT_MAX || *ep != '\0') { fprintf(stderr, "Fatal: value must be between 2 and %d inclusive\n", INT_MAX); goto fail; }

	if (intvl) num = -num;
out:
	return num;
success:
	num = 0;
	goto out;
fail:
	num = -1;
	goto out;
}


#define KIRA_(c)	c, sizeof c - 1


int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE, len, fd = -1, pid, signum, s, i, k = 0, n, msecs[2] = { 0, 0 };
	const char *str;
	struct timespec ts;
	struct sigaction sa;
	const struct { const char *str; int len; } quad[2][2] = { [0] = { [0] = { KIRA_("0\n") }, [1] = { KIRA_("1\n") } }, [1] = { [0] = { KIRA_("powersave\n") }, [1] = { KIRA_("performance\n") } } };

	k = 0;
	for (i = 1; i < argc; i++) {
		str = argv[i];
		pid = kira_num2fdpid(str);
		if (pid > 0) {
			if (k == 0) k = i;
			if (kill(pid, 0) != 0) { perror("kill()"); goto fail; }
			msecs[i - 1] = pid;
			continue;
		}
		if (pid < 0) {
			if (pid == -1 || k != 0 || i > 2 || (i == 2 && msecs[0] == 0)) break;
			msecs[i - 1] = -pid;
			continue;
		}
		fd = open(str, O_RDWR);
		if (fd < 0) { perror("open()"); goto fail; }
		if (close(fd) != 0) { perror("close()"); goto fail; }
		fd = -1;
		break;
	}

	if (k == 0) {
		fprintf(stderr, "%s [ busy_msec [ idle_msec ] ] pid [ pid_ ... ]\n"
		"\t" "[ /sys/devices/system/cpu/cpufreq/boost ] [ /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]\n"
		, argv[0]);
		goto fail;
	}

	for (i = 0; i < 2; i++) {
		if (msecs[i] == 0) msecs[i] = 1000 / 30;
	}

	if (sigemptyset(&sa.sa_mask) != 0) { perror("sigemptyset()"); goto fail; }
	sa.sa_handler = &kira_sigfn;
	if (sigaction(SIGINT, &sa, NULL) != 0) { perror("sigaction()"); goto fail; }

	while (true) {
		for (s = 0, signum = SIGSTOP; s < 2; s++, signum = SIGCONT) {
			for (i = k; i < argc; i++) {
				str = argv[i];
				pid = kira_num2fdpid(str);
				if (pid < 0) continue;
				if (pid > 0) { if (kill(pid, signum) != 0) { perror("kill()"); goto fail; } continue; }
				fd = open(str, O_RDWR);
				if (fd < 0) { perror("open()"); goto fail; }
				pid = strcmp(str + strlen(str) - 6, "/boost") == 0 ? 0 : 1;
				n = (quit == 0) ? s : 0;
				str = quad[pid][n].str;
				len = quad[pid][n].len;
				if (write(fd, str, len) != len) { if (errno == 0) fprintf(stderr, "Fatal: short write\n"); else perror("write()"); goto fail; }
				if (close(fd) != 0) { perror("close()"); goto fail; }
				fd = -1;
			}
			if (quit != 0) break;
			ts.tv_sec = msecs[1 - s] / 1000;
			ts.tv_nsec = msecs[1 - s] % 1000 * 1000000;
			while (quit == 0 && (ts.tv_sec != 0 || ts.tv_nsec != 0) && nanosleep(&ts, &ts) != 0) {
				if (errno != EINTR) { perror("nanosleep()"); goto fail; }
			}
		}
		if (quit != 0) break;
	}

	ret = EXIT_SUCCESS;
fail:
	if (fd >= 0 && close(fd) != 0) { perror("close()"); ret = EXIT_FAILURE; }
	return ret;
}
