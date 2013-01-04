/*
 * rngd.c -- Random Number Generator daemon
 *
 * rngd reads data from a hardware random number generator, verifies it
 * looks like random data, and adds it to /dev/random's entropy store.
 *
 * In theory, this should allow you to read very quickly from
 * /dev/random; rngd also adds bytes to the entropy store periodically
 * when it's full, which makes predicting the entropy store's contents
 * harder.
 *
 * Copyright (C) 2001 Philipp Rumpf
 * Copyright (C) 2001-2004 Jeff Garzik
 * Copyright (C) 2004 Henrique de Moraes Holschuh <hmh@debian.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <cutils/log.h>

#include "rng-tools-config.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <sys/file.h>
#include <assert.h>

#include "rngd.h"
#include "fips.h"
#include "exits.h"
#include "stats.h"
#include "util.h"
#include "rngd_threads.h"
#include "rngd_signals.h"
#include "rngd_entsource.h"
#include "rngd_linux.h"

#define XSTR(x) STR(x)
#define STR(x) #x
#define PROGNAME "rngd"

/*
 * Globals
 */

#define	RNGD_STAT_SLEEP_TIME 3600

/* Statistics */
struct rng_stats rng_stats;

/* Background/daemon mode */
pid_t masterprocess;			/* PID of the master process */
int am_daemon;				/* Nonzero if we went daemon */
int exitstatus = EXIT_SUCCESS;		/* Exit status on SIGTERM */
static FILE *daemon_lockfp = NULL;	/* Lockfile file pointer */
static int daemon_lockfd;		/* Lockfile file descriptor */

kernel_mode_t kernel;			/* Kernel compatibility mode */

static char doc[] =
	"Check and feed random data from hardware device to kernel entropy pool.\n";

static struct arguments default_arguments = {
	.rng_name	= DEVHWRANDOM,
	.random_name	= DEVRANDOM,
	.pidfile_name	= PIDFILE,
	.feed_interval	= 5,
	.random_step	= 64,
	.fill_watermark = -90,
	.rng_timeout	= 10,
	.daemon		= 1,
	.rng_entropy	= 1.0,
	.rng_buffers	= 3,
	.rng_quality	= 0,
	.rng_driver	= RNGD_ENTSOURCE_UNIXSTREAM,
};
struct arguments *arguments = &default_arguments;

/* Predefined known-good values for HRNGs */
struct trng_params {
	char *tag;		/* Short name of HRNG */
	char *name;		/* Full Name of HRNG */
	int width;		/* Best width for continuous run test */
	int buffers;		/* Recommended value for rng-buffers */
	double entropy;		/* Recommended value for rng-entropy */
	entropy_source_driver_t driver;  /* Entropy source driver */
};

static struct trng_params trng_parameters[] = {
	/* Device: Intel FWH RNG (82802AB/82802AC)
	 * Kernel driver: hw_random or i810_rng
	 * Device width: 8 bits
	 * Entropy: H > 0.999
	 *
	 * Slow, about 20Kibits/s (variable bitrate) with current
	 * kernel drivers, but the hardware should be capable of
	 * about 75kbit/s.  The kernel driver uses a lot of CPU
	 * time.  It is often misdetected (false positive).
	 *
	 * Whitepaper: Cryptographic Research
	 * http://www.cryptography.com/resources/whitepapers/IntelRNG.pdf
	 */
	{ .name 	= "Intel FWH (82802AB/AC) RNG",
	  .tag		= "intelfwh",
	  .width	= 32,
	  .buffers	= 5,
	  .entropy	= 0.998,
	  .driver	= RNGD_ENTSOURCE_UNIXSTREAM,
	},
	{ NULL },
};

/*
 * command line processing
 */
#define SEEN_OPT_RNGBUFFERS	0x01
#define SEEN_OPT_RNGENTROPY	0x02
#define SEEN_OPT_RNGDRIVER	0x04


/*
 * Daemon needs
 */
void die(int status)
{
	if (am_daemon) ALOGE("Exiting with status %d...", status);
	exit(status);
}

/*
 * Write our pid to our pidfile, and lock it
 */
static void get_lock(const char* pidfile_name)
{
	int otherpid = 0;
	int r;

	assert(pidfile_name != NULL);

	if (!daemon_lockfp) {
		if (((daemon_lockfd = open(pidfile_name, O_RDWR|O_CREAT, 0644)) == -1)
		|| ((daemon_lockfp = fdopen(daemon_lockfd, "r+"))) == NULL) {
			ALOGE("can't open or create %s", pidfile_name);
		        die(EXIT_USAGE);
		}
		fcntl(daemon_lockfd, F_SETFD, 1);

		do {
			r = flock(daemon_lockfd, LOCK_EX|LOCK_NB);
		} while (r && (errno == EINTR));

		if (r) {
			if (errno == EWOULDBLOCK) {
				rewind(daemon_lockfp);
				fscanf(daemon_lockfp, "%d", &otherpid);
				ALOGE("can't lock %s, running daemon's pid may be %d",
					pidfile_name, otherpid);
			} else {
				ALOGE("can't lock %s", pidfile_name);
			}
			die(EXIT_USAGE);
		}
	}

	rewind(daemon_lockfp);
	fprintf(daemon_lockfp, "%ld\n", (long int) getpid());
	fflush(daemon_lockfp);
	ftruncate(fileno(daemon_lockfp), ftell(daemon_lockfp));
}


/*
 * Statistics, n is the number of rng buffers
 */
static void init_rng_stats(int n)
{
	set_stat_prefix("stats: ");

	memset(&rng_stats, 0, sizeof(rng_stats));
	rng_stats.buffer_lowmark = n - 1; /* one is always in use */

	pthread_mutex_init(&rng_stats.group1_mutex, NULL);
	pthread_mutex_init(&rng_stats.group2_mutex, NULL);
	pthread_mutex_init(&rng_stats.group3_mutex, NULL);
}

static void dump_rng_stats(void)
{
	int j;
	char buf[256];

	pthread_mutex_lock(&rng_stats.group1_mutex);

	ALOGV("%d bits received from HRNG source", rng_stats.bytes_received * 8);

	pthread_mutex_unlock(&rng_stats.group1_mutex);
	pthread_mutex_lock(&rng_stats.group3_mutex);

	ALOGV("%d bits send to kernel pool", rng_stats.bytes_sent * 8);
	ALOGV("%d entropy added to kernel pool", rng_stats.entropy_sent);

	pthread_mutex_unlock(&rng_stats.group3_mutex);
	pthread_mutex_lock(&rng_stats.group2_mutex);

	ALOGV("%d FIPS 140-2 successes", rng_stats.good_fips_blocks);
	ALOGV("%d FIPS 140-2 failures", rng_stats.bad_fips_blocks);

	for (j = 0; j < N_FIPS_TESTS; j++) {
	//	message(LOG_INFO, dump_stat_counter(buf, sizeof(buf), fips_test_names[j],
	//			rng_stats.fips_failures[j]));
	}
	pthread_mutex_unlock(&rng_stats.group2_mutex);
	pthread_mutex_lock(&rng_stats.group1_mutex);
//	message(LOG_INFO, dump_stat_bw(buf, sizeof(buf),
//			"HRNG source speed", "bits",
//			&rng_stats.source_blockfill, FIPS_RNG_BUFFER_SIZE*8));
	pthread_mutex_unlock(&rng_stats.group1_mutex);
	pthread_mutex_lock(&rng_stats.group2_mutex);
//	message(LOG_INFO, dump_stat_bw(buf, sizeof(buf),
//			"FIPS tests speed", "bits",
//			&rng_stats.fips_blockfill, FIPS_RNG_BUFFER_SIZE*8));
	pthread_mutex_unlock(&rng_stats.group2_mutex);
	pthread_mutex_lock(&rng_stats.group3_mutex);
//	message(LOG_INFO, dump_stat_counter(buf, sizeof(buf),
//			"Lowest ready-buffers level",
//			rng_stats.buffer_lowmark));
//	message(LOG_INFO, dump_stat_counter(buf, sizeof(buf),
//			"Entropy starvations",
//			rng_stats.sink_starved));
//	message(LOG_INFO, dump_stat_stat(buf, sizeof(buf),
//			"Time spent starving for entropy",
//			"us",
//			&rng_stats.sink_wait));
	pthread_mutex_unlock(&rng_stats.group3_mutex);
}

int main(int argc, char **argv)
{
	int fd;
	pthread_t t1,t2,t3;
	int sleeptime;

	kernel = kernel_mode();

	/* Make sure kernel is supported */
	if (kernel == KERNEL_UNSUPPORTED) {
		ALOGE("Unsupported kernel detected, exiting...");
		die (EXIT_OSERR);
	}

	/* close useless FDs we might have gotten somehow */
	for(fd = 3; fd < 250; fd++) (void) close(fd);

	/* Init statistics */
	init_rng_stats(arguments->rng_buffers);

	/* Init signal handling early */
	init_sighandlers();

	/* Init entropy source */
	init_entropy_source();

	/* Init entropy sink */
	init_kernel_rng();

	if (arguments->daemon) {
		/* check if another rngd is running,
		 * create pidfile and lock it */
		get_lock(arguments->pidfile_name);

		if (daemon(0, 0) < 0) {
			ALOGE("can't daemonize");
			return EXIT_OSERR;
		}

		am_daemon = 1;

		/* update pidfile */
		get_lock(arguments->pidfile_name);
	}

	masterprocess = getpid();
	ALOGI(PROGNAME " " VERSION " starting up...");

	/* post-fork initialization */
	init_rng_buffers(arguments->rng_buffers);
	init_sighandlers();

	/* Fire up worker threads */
	if (pthread_create(&t1, NULL, &do_rng_data_source_loop, NULL) |
	    pthread_create(&t2, NULL, &do_rng_fips_test_loop, NULL ) |
	    pthread_create(&t3, NULL, &do_rng_data_sink_loop, NULL )) {
		ALOGE("Insufficient resources to start threads");
		die(EXIT_OSERR);
	}

	/*
	 * All we can do now is spin around waiting for a hit to the head.
	 * Dump stats every hour, and at exit...
	 */
	sleeptime = RNGD_STAT_SLEEP_TIME;
	while (!gotsigterm) {
		sleeptime = sleep(sleeptime);
		if ((sleeptime == 0) || gotsigusr1 || gotsigterm) {
			dump_rng_stats();
			sleeptime = RNGD_STAT_SLEEP_TIME;
			gotsigusr1 = 0;
		}
	}

	if (exitstatus == EXIT_SUCCESS)
		ALOGI("Exiting...");
	else
		ALOGE("Exiting with status %d", exitstatus);

	exit(exitstatus);
}
