/*
 *  Copyright (c) 2002-2003 Erik Fears
 *  Copyright (c) 2014-2022 ircd-hybrid development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 *  USA
 */

#include "setup.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h> /* getrlimit */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#include "config.h"
#include "irc.h"
#include "log.h"
#include "opercmd.h"
#include "scan.h"
#include "options.h"
#include "memory.h"
#include "main.h"


static int RESTART;  /* Flagged to restart on next cycle */
static int ALARMED;  /* Flagged to call timer functions on next cycle */
static int REOPEN;  /* Flagged to reopen log files on next cycle */

static struct sigaction ALARMACTION;
static struct sigaction INTACTION;
static struct sigaction HUPACTION;
static struct sigaction USR1ACTION;

static const char *CONFNAME = DEFAULTNAME;
static const char *CONFDIR = HOPM_ETCDIR;
static const char *LOGDIR = HOPM_LOGDIR;
static char *CONFFILE, *LOGFILE;

unsigned int OPT_DEBUG = 0;  /* Debug level */


static void
setup_corelimit(void)
{
  struct rlimit rlim;

  if (getrlimit(RLIMIT_CORE, &rlim) == 0)
  {
    rlim.rlim_cur = rlim.rlim_max;
    setrlimit(RLIMIT_CORE, &rlim);
  }
}

static void
do_signal(int signum)
{
  switch (signum)
  {
    case SIGALRM:
      ALARMED = 1;
      alarm(1);
      break;
    case SIGINT:
      log_printf("MAIN -> Caught SIGINT, bye!");
      exit(0);
      break;
    case SIGHUP:
      RESTART = 1;
      break;
    case SIGUSR1:
      REOPEN = 1;
      break;
  }
}

int
main(int argc, char *argv[])
{
  pid_t pid;
  size_t lenc, lenl;
  FILE *pidout;
  struct rlimit rlim;

  if (pledge("stdio rpath wpath cpath inet dns proc exec unveil", NULL) == -1) {
    err(1, "pledge");
  }

  if (unveil("/", "")) {
    err(1, "unveil");
  }

  setup_corelimit();

  while (1)
  {
    int c = getopt(argc, argv, "dc:");
    if (c == -1)
      break;

    switch (c)
    {
      case 'c':
        CONFNAME = xstrdup(optarg);
         break;
      case 'd':
        ++OPT_DEBUG;
        break;
      default:  /* Unknown arg, guess we'll just do nothing for now. */
        break;
    }
  }

  lenc = strlen(CONFDIR) + strlen(CONFNAME) + strlen(CONFEXT) + 3;
  lenl = strlen(LOGDIR) + strlen(CONFNAME) + strlen(LOGEXT) + 3;

  CONFFILE = xcalloc(lenc * sizeof *CONFFILE);
  LOGFILE = xcalloc(lenl * sizeof *LOGFILE);

  snprintf(CONFFILE, lenc, "%s/%s.%s", CONFDIR, CONFNAME, CONFEXT);
  snprintf(LOGFILE, lenl, "%s/%s.%s", LOGDIR, CONFNAME, LOGEXT);

  if (unveil(HOPM_PREFIX, "r") == -1) {
    err(1, "unveil");
  }

  if (chdir(HOPM_PREFIX))
  {
    perror("chdir");
    exit(EXIT_FAILURE);
  }

  /* Fork off. */
  if (OPT_DEBUG == 0)
  {
    if ((pid = fork()) < 0)
    {
      perror("fork()");
      exit(EXIT_FAILURE);
    }
    else if (pid != 0)
      _exit(EXIT_SUCCESS);

    /* Get us in our own process group. */
    if (setpgid(0, 0) < 0)
    {
      perror("setpgid()");
      exit(EXIT_FAILURE);
    }

    /* Reset file mode. */
    umask(077);  /* umask 077: u=rwx,g=,o= */

    /* Connect stdin, stdout, and stderr to /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0)
    {
      perror("open()");
      exit(EXIT_FAILURE);
    }

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > STDERR_FILENO)
      close(fd);

    if (unveil(LOGFILE, "wc") == -1) {
      err(1, "unveil");
    }

    log_open(LOGFILE);
  }
  else
    log_printf("MAIN -> Debug level %d", OPT_DEBUG);

  log_printf("MAIN -> HOPM %s started.", VERSION);
  log_printf("MAIN -> Reading configuration file...");

  if (unveil(CONFFILE, "r") == -1) {
    err(1, "unveil");
  }

  config_load(CONFFILE);

  if (OptionsItem.scanlog) {
    if (unveil(OptionsItem.scanlog, "wc")) {
      err(1, "unveil");
    }

    scanlog_open(OptionsItem.scanlog);
  }

  if (unveil(OptionsItem.pidfile, "wc")) {
    err(1, "unveil");
  }

  pidout = fopen(OptionsItem.pidfile, "w");

  if (unveil(HOPM_BINPATH, "x") == -1) {
    err(1, "unveil");
  }

  if (pledge("stdio inet dns exec", NULL) == -1) {
    err(1, "pledge");
  }

  if (pidout)
  {
    fprintf(pidout, "%u\n", (unsigned int)getpid());
    fclose(pidout);
  }
  else
  {
    log_printf("MAIN -> Error opening pid file %s: %s", OptionsItem.pidfile,
               strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* Setup alarm & int handlers. */
  ALARMACTION.sa_handler = &do_signal;
  ALARMACTION.sa_flags = SA_RESTART;
  INTACTION.sa_handler = &do_signal;
  HUPACTION.sa_handler = &do_signal;
  USR1ACTION.sa_handler = &do_signal;

  sigaction(SIGALRM, &ALARMACTION, NULL);
  sigaction(SIGINT, &INTACTION, NULL);
  sigaction(SIGHUP, &HUPACTION, NULL);
  sigaction(SIGUSR1, &USR1ACTION, NULL);

  /* Ignore SIGPIPE. */
  signal(SIGPIPE, SIG_IGN);

  alarm(1);

  while (1)
  {
    /* Main cycles */
    irc_cycle();
    scan_cycle();

    /* Restart HOPM if main_restart() was called (usually happens by m_kill in irc.c) */
    if (RESTART)
    {
      /* If restarted in debug mode, die */
      if (OPT_DEBUG)
        return 1;

      log_printf("MAIN -> Restarting process");

      /* Get upper file descriptor limit */
      if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
      {
        log_printf("MAIN RESTART -> getrlimit() error retrieving RLIMIT_NOFILE (%s)", strerror(errno));
        return 1;
      }

      /* Set file descriptors 0-rlim_cur close on exec */
      for (unsigned int i = 0; i < rlim.rlim_cur; ++i)
        fcntl(i, F_SETFD, FD_CLOEXEC);

      /* execute new process */
      if (execv(HOPM_BINPATH, argv) == -1)
        log_printf("MAIN RESTART -> Execution of \"%s\" failed. ERROR: %s", HOPM_BINPATH, strerror(errno));

      exit(0);  /* Should only get here if execv() failed */
    }

    /* Check for log reopen */
    if (REOPEN)
    {
      log_printf("MAIN -> Caught SIGUSR1, reopening logfiles");
      log_close();
      log_open(LOGFILE);

      if (OptionsItem.scanlog)
      {
        scanlog_close();
        scanlog_open(OptionsItem.scanlog);
      }

      log_printf("MAIN -> reopened logfiles");

      REOPEN = 0;
    }

    /* Call 1 second timers */
    if (ALARMED)
    {
      irc_timer();
      scan_timer();
      command_timer();

      ALARMED = 0;
    }
  }

  if (OPT_DEBUG == 0)
    log_close();

  /* If there's no scanlog open then this will do nothing anyway */
  scanlog_close();
  return 0;
}

void
main_restart(void)
{
  RESTART = 1;
}
