/*
 * Copyright (c) 1995
 *	A.R. Gordon (andrew.gordon@net-tel.co.uk).  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed for the FreeBSD project
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ANDREW GORDON AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/* main() function for status monitor daemon.  Some of the code in this	*/
/* file was generated by running rpcgen /usr/include/rpcsvc/sm_inter.x	*/
/* The actual program logic is in the file procs.c			*/

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <rpc/rpc.h>
#include <rpc/rpc_com.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include "statd.h"

int debug = 0;		/* Controls syslog() calls for debug messages	*/

static void handle_sigchld(int sig);
static void usage(void);

const char *transports[] = { "udp", "tcp", "udp6", "tcp6" };

int
main(int argc, char **argv)
{
  SVCXPRT *transp;
  struct sigaction sa;
  struct netconfig *nconf;
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  int ch, i, maxindex, r, s, sock;
  char *endptr;
  int maxrec = RPC_MAXDATASIZE;
  in_port_t svcport = 0;

  while ((ch = getopt(argc, argv, "dp:")) != -1)
    switch (ch) {
    case 'd':
      debug = 1;
      break;
    case 'p':
      endptr = NULL;
      svcport = (in_port_t)strtoul(optarg, &endptr, 10);
      if (endptr == NULL || *endptr != '\0' || svcport == 0 || 
          svcport >= IPPORT_MAX)
	usage();
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;

  (void)rpcb_unset(SM_PROG, SM_VERS, NULL);

  /*
   * Check if IPv6 support is present.
   */
  s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0)
      maxindex = 2;
  else {
      close(s);
      maxindex = 4;
  }

  if (svcport != 0) {
      bzero(&sin, sizeof(struct sockaddr_in));
      sin.sin_len = sizeof(struct sockaddr_in);
      sin.sin_family = AF_INET;
      sin.sin_port = htons(svcport);

      bzero(&sin6, sizeof(struct sockaddr_in6));
      sin6.sin6_len = sizeof(struct sockaddr_in6);
      sin6.sin6_family = AF_INET6;
      sin6.sin6_port = htons(svcport);
  }

  rpc_control(RPC_SVC_CONNMAXREC_SET, &maxrec);

  for (i = 0; i < maxindex; i++) {
      nconf = getnetconfigent(transports[i]);
      if (nconf == NULL)
    	  errx(1, "cannot get %s netconf: %s.", transports[i],
		nc_sperror());

      if (svcport != 0) {
	  if (strcmp(nconf->nc_netid, "udp6") == 0) {
	      sock = socket(AF_INET6, SOCK_DGRAM,
			    IPPROTO_UDP);
	      if (sock != -1) {
		  r = bindresvport_sa(sock, 
		        (struct sockaddr *)&sin6);
		  if (r != 0) {
		      syslog(LOG_ERR, "bindresvport: %m");
		      exit(1);
		  }
	     }
	  } else if (strcmp(nconf->nc_netid, "udp") == 0) {
	      sock = socket(AF_INET, SOCK_DGRAM,
			    IPPROTO_UDP);
	      if (sock != -1) {
		  r = bindresvport(sock, &sin);
		  if (r != 0) {
		      syslog(LOG_ERR, "bindresvport: %m");
		      exit(1);
		  }
	      }
	  } else if (strcmp(nconf->nc_netid, "tcp6") == 0) {
	      sock = socket(AF_INET6, SOCK_STREAM,
			    IPPROTO_TCP);
              if (sock != -1) {
		  r = bindresvport_sa(sock, 
			(struct sockaddr *)&sin6);
	      	  if (r != 0) {
		      syslog(LOG_ERR, "bindresvport: %m");
		      exit(1);
	          }
	      }
	  } else if (strcmp(nconf->nc_netid, "tcp") == 0) {
	      sock = socket(AF_INET, SOCK_STREAM,
			    IPPROTO_TCP);
	      if (sock != -1) {
		  r = bindresvport(sock, &sin);
		  if (r != 0) {
		      syslog(LOG_ERR, "bindresvport: %m");
		      exit(1);
		  }
	      }
	  }

	  if (nconf->nc_semantics != NC_TPI_CLTS)
	      listen(sock, SOMAXCONN);

	  transp = svc_tli_create(sock, nconf, NULL,
	    	RPC_MAXDATASIZE, RPC_MAXDATASIZE);
      } else {
	  transp = svc_tli_create(RPC_ANYFD, nconf, NULL,
	    	RPC_MAXDATASIZE, RPC_MAXDATASIZE);
      }

      if (transp == NULL) {
	  errx(1, "cannot create %s service.", transports[i]);
	  /* NOTREACHED */
      }
      if (!svc_reg(transp, SM_PROG, SM_VERS, sm_prog_1, nconf)) {
	  errx(1, "unable to register (SM_PROG, NLM_SM, %s)",
		 transports[i]);
	  /* NOTREACHED */
      }
      freenetconfigent(nconf);
  }
  init_file("/var/db/statd.status");

  /* Note that it is NOT sensible to run this program from inetd - the 	*/
  /* protocol assumes that it will run immediately at boot time.	*/
  daemon(0, 0);
  openlog("rpc.statd", 0, LOG_DAEMON);
  if (debug) syslog(LOG_INFO, "Starting - debug enabled");
  else syslog(LOG_INFO, "Starting");

  /* Install signal handler to collect exit status of child processes	*/
  sa.sa_handler = handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGCHLD);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

  /* Initialisation now complete - start operating			*/
  notify_hosts();	/* Forks a process (if necessary) to do the	*/
			/* SM_NOTIFY calls, which may be slow.		*/

  svc_run();	/* Should never return					*/
  exit(1);
}

static void
usage()
{
      fprintf(stderr, "usage: rpc.statd [-d] [-p <port>]\n");
      exit(1);
}

/* handle_sigchld ---------------------------------------------------------- */
/*
   Purpose:	Catch SIGCHLD and collect process status
   Retruns:	Nothing.
   Notes:	No special action required, other than to collect the
		process status and hence allow the child to die:
		we only use child processes for asynchronous transmission
		of SM_NOTIFY to other systems, so it is normal for the
		children to exit when they have done their work.
*/

static void handle_sigchld(int sig __unused)
{
  int pid, status;
  pid = wait4(-1, &status, WNOHANG, (struct rusage*)0);
  if (!pid) syslog(LOG_ERR, "Phantom SIGCHLD??");
  else if (status == 0)
  {
    if (debug) syslog(LOG_DEBUG, "Child %d exited OK", pid);
  }
  else syslog(LOG_ERR, "Child %d failed with status %d", pid,
    WEXITSTATUS(status));
}

