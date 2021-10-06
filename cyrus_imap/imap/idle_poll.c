/* 
 * Copyright (c) 1998-2003 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: idle_poll.c,v 1.10 2003/10/22 20:05:11 ken3 Exp $ */

#include <config.h>

#include <syslog.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>

#include "idle.h"
#include "global.h"

const char *idle_method_desc = "poll";

/* function to report mailbox updates to the client */
static idle_updateproc_t *idle_update = NULL;

/* how often to poll the mailbox */
static time_t idle_period = -1;

int idle_enabled(void)
{
    /* get polling period */
    if (idle_period == -1) {
      idle_period = config_getint(IMAPOPT_IMAPIDLEPOLL);
      if (idle_period < 0) idle_period = 0;
    }

    /* a period of zero disables IDLE */
    return idle_period;
}

static void idle_poll(int sig __attribute__((unused)))
{
    idle_update(IDLE_MAILBOX|IDLE_ALERT);

    alarm(idle_period);
}

int idle_init(struct mailbox *mailbox __attribute__((unused)),
	      idle_updateproc_t *proc)
{
    struct sigaction action;

    idle_update = proc;

    /* Setup the mailbox polling function to be called at 'idle_period'
       seconds from now */
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
#ifdef SA_RESTART
    action.sa_flags |= SA_RESTART;
#endif
    action.sa_handler = idle_poll;
    if (sigaction(SIGALRM, &action, NULL) < 0) {
	syslog(LOG_ERR, "sigaction: %m");
	return 0;
    }

    alarm(idle_period);

    return 1;
}

void idle_done(struct mailbox *mailbox __attribute__((unused)))
{
    /* Remove the polling function */
    signal(SIGALRM, SIG_IGN);
}
