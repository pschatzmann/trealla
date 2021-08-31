#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>

#ifdef _WIN32
#include <io.h>
#define snprintf _snprintf
#define isatty _isatty
#else
#include <unistd.h>
#endif

#include "history.h"
#include "trealla.h"

#ifdef _WIN32
#include <windows.h>
#define msleep Sleep
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define msleep(ms)                                                                                                             \
{                                                                                                                              \
	struct timespec tv;                                                                                                    \
	tv.tv_sec = (ms) / 1000;                                                                                               \
	tv.tv_nsec = ((ms) % 1000) * 1000 * 1000;                                                                              \
	nanosleep(&tv, &tv);                                                                                                   \
}
#endif

void sigfn(int s)
{
        (void) s;
	signal(SIGINT, &sigfn);
	g_tpl_interrupt = 1;
}

static int daemonize(int argc, char *argv[])
{
	char path[1024];
	path[0] = '\0';
	int watchdog = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watchdog"))
			watchdog = 1;
		else if (!strncmp(argv[i], "--cd=", 5))
			strcpy(path, argv[i] + 5);
	}

#ifdef _WIN32
	char cmd[1024], args[1024 * 8];
	args[0] = 0;
	strcpy(cmd, argv[0]);
	strcat(cmd, ".exe");

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--daemon"))
			continue;

		if (!args[0])
			strcat(args, " ");

		strcat(args, "\"");
		strcat(args, argv[i]);
		strcat(args, "\"");
	}

	STARTUPINFO startInfo = {0};
	PROCESS_INFORMATION process = {0};

	startInfo.cb = sizeof(startInfo);
	startInfo.dwFlags = STARTF_USESHOWWINDOW;
	startInfo.wShowWindow = SW_HIDE;

	if (!CreateProcessA(
		(LPSTR)cmd,              // application name
		(LPSTR)args,             // command line arguments
		NULL,                    // process attributes
		NULL,                    // thread attributes
		FALSE,                   // inherit (file) handles
		DETACHED_PROCESS,        // Detach
		NULL,                    // environment
		(path[0] ? path : NULL), // current directory
		&startInfo,              // startup info
		&process)                // process information
		) {
		fprintf(stdedrr, "Error: creation of the process failed\n");
		return 1;
	}

	return 0;
#else
	pid_t pid;

	if ((pid = fork()) < 0) // Error
		return -1;
	else if (pid != 0) // Parent
		return 0;

	if (watchdog)
		signal(SIGCHLD, SIG_IGN);

	while (watchdog) {
		pid_t pid;

		if ((pid = fork()) < 0) // Error
			return -1;
		else if (pid != 0) // Parent
		{
			if (watchdog) {
				int status;
				wait(&status);
				msleep(1000);
			}
			else
				return 0;
		}
		else // Child
			break;
	}

	if (path[0])
		if (chdir(path) < 0)
			fprintf(stderr, "Error: can't chdir(%s)\n", path);

	setsid();
	umask(0);
	close(2);
	close(1);
	close(0);
	return 1;
#endif
}

int main(int ac, char *av[])
{
	setlocale(LC_ALL, ".UTF8");
	const char *homedir;
	g_argv0 = av[0];

	if ((homedir = getenv("HOME")) == NULL)
		homedir = ".";

#ifdef FAULTINJECT_ENABLED
	FAULTINJECT_NAME.counter = strtoul(getenv("FAULTSTART")?getenv("FAULTSTART"):"0", NULL, 0);
	FAULTINJECT_NAME.abort = getenv("FAULTABORT")?true:false;
	static bool faultinject_is_off;
	faultinject_is_off = !FAULTINJECT_NAME.counter;
#endif

	char histfile[1024];
	snprintf(histfile, sizeof(histfile), "%s/%s", homedir, ".tpl_history");

	//int did_load = 0;
	int i, do_goal = 0, do_lib = 0;
	int version = 0, quiet = 0, daemon = 0;
	int ns = 0;
	void *pl = pl_create();
	if (!pl)
	{
		fprintf(stderr, "Failed to create the prolog system: %s\n", strerror(errno));
		return 1;
	}

	set_opt(pl, 1);

	for (i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--")) {
			g_ac = ac;
			g_avc = ++i;
			g_av = av;
			break;
		}

		if (!strcmp(av[i], "-h") || !strcmp(av[i], "--help")) {
			version = 2;
		} else if (!strcmp(av[i], "-v") || !strcmp(av[i], "--version")) {
			version = 1;
		} else if (!strcmp(av[i], "-q") || !strcmp(av[i], "--quiet")) {
			set_quiet(pl);
			quiet = 1;
		} else if (!strcmp(av[i], "-O0") || !strcmp(av[i], "--noopt"))
			set_opt(pl, 0);
		else if (!strcmp(av[i], "-t") || !strcmp(av[i], "--trace"))
			set_trace(pl);
		else if (!strcmp(av[i], "--stats"))
			set_stats(pl);
		else if (!strcmp(av[i], "--noindex"))
			set_noindex(pl);
		else if (!strcmp(av[i], "--ns"))
			ns = 1;
		else if (!strcmp(av[i], "-d") || !strcmp(av[i], "--daemon"))
			daemon = 1;
	}

	if (daemon) {
		if (!daemonize(ac, av)) {
			pl_destroy(pl);
			return 0;
		}
	} else
		signal(SIGINT, &sigfn);

	signal(SIGPIPE, SIG_IGN);
	const char *goal = NULL;

	pl_consult(pl, "~/.tplrc");

	for (i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--"))
			break;

#if 0
		if ((av[i][0] == '-') && did_load) {
			fprintf(stderr, "Error: options entered after files\n");
			pl_destroy(pl);
			return 0;
		}
#endif

		if (!strcmp(av[i], "--consult")) {
			if (!pl_consult_fp(pl, stdin, "./")) {
				pl_destroy(pl);
				return 1;
			}
		} else if (!strcmp(av[i], "--library")) {
			do_goal = 0;
			do_lib = 1;
		} else if (!strcmp(av[i], "-f") || !strcmp(av[i], "-l") || !strcmp(av[i], "--consult-file")) {
			do_lib = do_goal = 0;
		} else if (!strcmp(av[i], "-g") || !strcmp(av[i], "--query-goal")) {
			do_lib = 0;
			do_goal = 1;
		} else if (av[i][0] == '-') {
			continue;
		} else if (do_lib) {
			g_tpl_lib = strdup(av[i]);
			do_lib = 0;
		} else if (do_goal) {
			do_goal = 0;
			goal = av[i];
		} else {
			//did_load = 1;

			if (!pl_consult(pl, av[i]) || ns) {
				pl_destroy(pl);
				return 1;
			}
		}
	}


	if (goal) {
		if (!pl_eval(pl, goal)) {
			int halt_code = get_halt_code(pl);
			pl_destroy(pl);
#ifdef FAULTINJECT_ENABLED
			if (faultinject_is_off)
				fprintf(stderr, "\nCDEBUG FAULT INJECTION MAX %llu\n", 0LLU-FAULTINJECT_NAME.counter);
#endif
			return halt_code;
		}

		if (get_halt(pl) || ns) {
			int halt_code = get_halt_code(pl);
			pl_destroy(pl);
#ifdef FAULTINJECT_ENABLED
			if (faultinject_is_off)
				fprintf(stderr, "\nCDEBUG FAULT INJECTION MAX %llu\n", 0LLU-FAULTINJECT_NAME.counter);
#endif
			return halt_code;
		}
	}

	if (!quiet)
		printf("Trealla Prolog (c) Infradig 2020-2021, %s\n", VERSION);

	if ((version == 2) && !quiet) {
		fprintf(stdout, "Usage:\n");
		fprintf(stdout, "  tpl [options] [files] [-- args]\n");
		fprintf(stdout, "Options:\n");
		fprintf(stdout, "  -l file\t\t- consult file\n");
		fprintf(stdout, "  -g goal\t\t- query goal (only used once)\n");
		fprintf(stdout, "  --library path\t- alt to TPL_LIBRARY_PATH env variable\n");
		fprintf(stdout, "  -v, --version\t\t- print version info and exit\n");
		fprintf(stdout, "  -h, --help\t\t- print help info and exit\n");
		fprintf(stdout, "  -O0, --noopt\t\t- turn off optimization\n");
		fprintf(stdout, "  -q, --quiet\t\t- quiet mode\n");
		fprintf(stdout, "  -t, --trace\t\t- trace mode\n");
		fprintf(stdout, "  -d, --daemon\t\t- daemonize\n");
		fprintf(stdout, "  -w, --watchdog\t- create watchdog\n");
		fprintf(stdout, "  --consult\t\t- consult from STDIN\n");
		fprintf(stdout, "  --stats\t\t- print stats\n");
		fprintf(stdout, "  --noindex\t\t- don't use term indexing\n");
		fprintf(stdout, "  --ns\t\t\t- non-stop (to top-level)\n");
	}

	if ((version && !quiet) || ns) {
		pl_destroy(pl);
		return 0;
	}

#ifdef FAULTINJECT_ENABLED
	fprintf(stderr, "CDEBUG FAULT INJECTION ENABLED!\n"); //Don't use this build for benchmarking and production
#endif

	if (isatty(0))
		history_load(histfile);

	char *line;

	while ((line = history_readline_eol("?- ", '.')) != NULL) {
		const char *src = line;

		while (isspace(*src))
			src++;

		if (!strcmp(src, "halt.")) {
			free(line);
			break;
		}

		if (!*src || (*src == '\n')) {
			free(line);
			continue;
		}

		pl_eval(pl, src);
		free(line);

		if (get_halt(pl))
			break;

		if (!get_dump_vars(pl)) {
			printf("%s", get_status(pl) ? "true" : "false");
			printf(".\n");
		}
	}

	if (isatty(0))
		history_save();

	int halt_code = get_halt_code(pl);
	pl_destroy(pl);

#ifdef FAULTINJECT_ENABLED
	if (faultinject_is_off)
		fprintf(stderr, "\nCDEBUG FAULT INJECTION MAX %llu\n", 0LLU-FAULTINJECT_NAME.counter);
#endif

	return halt_code;
}
