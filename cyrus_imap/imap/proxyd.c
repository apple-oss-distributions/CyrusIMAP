/* proxyd.c -- IMAP server proxy for Cyrus Murder
 *
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

/* $Id: proxyd.c,v 1.192 2004/12/17 16:32:20 ken3 Exp $ */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>

#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "prot.h"

#include "acl.h"
#include "annotate.h"
#include "util.h"
#include "auth.h"
#include "map.h"
#include "global.h"
#include "tls.h"
#include "version.h"
#include "charset.h"
#include "imparse.h"
#include "iptostring.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mboxname.h"
#include "mailbox.h"
#include "mupdate-client.h"
#include "xmalloc.h"
#include "mboxlist.h"
#include "imapurl.h"
#include "pushstats.h"
#include "telemetry.h"
#include "backend.h"

/* config.c stuff */
const int config_need_data = 0;

/* PROXY STUFF */
/* we want a list of our outgoing connections here and which one we're
   currently piping */
#define IDLE_TIMEOUT (5 * 60)

static const int ultraparanoid = 1; /* should we kick after every operation? */
static unsigned int proxyd_cmdcnt;

static int referral_kick = 0; /* kick after next command recieved, for
				 referrals that are likely to change the
				 mailbox list */

/* all subscription commands go to the backend server containing the
   user's inbox */
struct backend *backend_inbox = NULL;

/* the current server most commands go to */
struct backend *backend_current = NULL;

/* our cached connections */
struct backend **backend_cached = NULL;

/* are we doing virtdomains with multiple IPs? */
static int disable_referrals;

/* has the client issued an RLIST or RLSUB? */
static int supports_referrals;


/* -------- from imapd ---------- */

extern int optind;
extern char *optarg;

/* global state */
static char shutdownfilename[1024];
static int imaps = 0;
static sasl_ssf_t extprops_ssf = 0;
static nosaslpasswdcheck = 0;

/* per-user session state */
struct protstream *proxyd_out = NULL;
struct protstream *proxyd_in = NULL;
static char proxyd_clienthost[NI_MAXHOST*2+1] = "[local]";
static int proxyd_logfd = -1;
time_t proxyd_logtime;
char *proxyd_userid = NULL;
static char *proxyd_magicplus = NULL;
struct auth_state *proxyd_authstate = 0;
int proxyd_userisadmin;
sasl_conn_t *proxyd_saslconn; /* the sasl connection context to the client */
int proxyd_starttls_done = 0; /* have we done a successful starttls yet? */
#ifdef HAVE_SSL
static SSL *tls_conn;
#endif /* HAVE_SSL */

/* the sasl proxy policy context */
static struct proxy_context proxyd_proxyctx = {
    1, 1, &proxyd_authstate, &proxyd_userisadmin, NULL
};

/* current namespace */
static struct namespace proxyd_namespace;

void motd_file(int fd);
void shut_down(int code);
void fatal(const char *s, int code);

void proxyd_downserver(struct backend *s);

void cmdloop(void);
void cmd_login(char *tag, char *user);
void cmd_authenticate(char *tag, char *authtype, char *resp);
void cmd_noop(char *tag, char *cmd);
void cmd_capability(char *tag);
void cmd_append(char *tag, char *name);
void cmd_select(char *tag, char *cmd, char *name);
void cmd_close(char *tag);
void cmd_fetch(char *tag, char *sequence, int usinguid);
void cmd_partial(char *tag, char *msgno, char *data,
		 char *start, char *count);
void cmd_store(char *tag, char *sequence, char *operation, int usinguid);
void cmd_search(char *tag, int usinguid);
void cmd_sort(char *tag, int usinguid);
void cmd_thread(char *tag, int usinguid);
void cmd_copy(char *tag, char *sequence, char *name, int usinguid);
void cmd_expunge(char *tag, char *sequence);
void cmd_create(char *tag, char *name, char *partition);
void cmd_delete(char *tag, char *name);
void cmd_rename(char *tag, char *oldname, char *newname, char *partition);
void cmd_find(char *tag, char *namespace, char *pattern);
void cmd_list(char *tag, int listopts, char *reference, char *pattern);
void cmd_changesub(char *tag, char *namespace, char *name, int add);
void cmd_getacl(const char *tag, const char *name);
void cmd_listrights(char *tag, char *name, char *identifier);
void cmd_myrights(const char *tag, const char *name);
void cmd_setacl(char *tag, const char *name,
		const char *identifier, const char *rights);
void cmd_getquota(char *tag, char *name);
void cmd_getquotaroot(char *tag, char *name);
void cmd_setquota(char *tag, char *quotaroot);
void cmd_status(char *tag, char *name);
void cmd_getuids(char *tag, char *startuid);
void cmd_unselect(char* tag);
void cmd_namespace(char* tag);
void cmd_reconstruct(char *tag, char *name);

void cmd_id(char* tag);
struct idparamlist {
    char *field;
    char *value;
    struct idparamlist *next;
};
extern void id_getcmdline(int argc, char **argv);
extern void id_response(struct protstream *pout);
void id_appendparamlist(struct idparamlist **l, char *field, char *value);
void id_freeparamlist(struct idparamlist *l);

void cmd_idle(char* tag);
void cmd_starttls(char *tag, int imaps);

#ifdef ENABLE_X_NETSCAPE_HACK
void cmd_netscape (char* tag);
#endif

void cmd_getannotation(char* tag, char *mboxpat);
void cmd_setannotation(char* tag, char *mboxpat);

int getannotatefetchdata(char *tag,
			 struct strlist **entries, struct strlist **attribs);
int getannotatestoredata(char *tag, struct entryattlist **entryatts);

void annotate_response(struct entryattlist *l);
int annotate_fetch_proxy(const char *server, const char *mbox_pat,
			 struct strlist *entry_pat,
			 struct strlist *attribute_pat);
int annotate_store_proxy(const char *server, const char *mbox_pat,
			 struct entryattlist *entryatts);

void printstring (const char *s);
void printastring (const char *s);

static int mailboxdata(char *name, int matchlen, int maycreate, void *rock);
static int listdata(char *name, int matchlen, int maycreate, void *rock);
static void mstringdata(char *cmd, char *name, int matchlen, int maycreate);

static int mlookup(const char *name, char **pathp, 
		   char **aclp, void *tid);

extern int saslserver(sasl_conn_t *conn, const char *mech,
		      const char *init_resp, const char *resp_prefix,
		      const char *continuation, const char *empty_chal,
		      struct protstream *pin, struct protstream *pout,
		      int *sasl_result, char **success_data);

/* Enable the resetting of a sasl_conn_t */
static int reset_saslconn(sasl_conn_t **conn);

static struct 
{
    char *ipremoteport;
    char *iplocalport;    
    sasl_ssf_t ssf;
    char *authid;
} saslprops = {NULL,NULL,0,NULL};

#define BUFGROWSIZE 100

/* proxy support functions */
enum {
    PROXY_NOCONNECTION = -1,
    PROXY_OK = 0,
    PROXY_NO = 1,
    PROXY_BAD = 2
};

static void proxyd_gentag(char *tag, size_t len)
{
    snprintf(tag, len, "PROXY%d", proxyd_cmdcnt++);
}

/* pipe_until_tag() reads from 's->in' until the tagged response
   starting with 'tag' appears.  it returns the result of the
   tagged command, and sets 's->last_result' with the tagged line. */

/* 's->last_result' assumes that tagged responses are:
   a) short
   b) don't contain literals
   
   IMAP grammar allows both, unfortunately */

/* force_notfatal says to not fatal() if we lose connection to backend_current
 * even though it is in 95% of the cases, a good idea... */
static int pipe_until_tag(struct backend *s, char *tag, int force_notfatal)
{
    char buf[2048];
    char eol[128];
    int sl;
    int cont = 0, last = 0, r = -1;
    size_t taglen = strlen(tag);

    s->timeout->mark = time(NULL) + IDLE_TIMEOUT;
    
    if(taglen >= sizeof(buf)) {
	fatal("tag too large",EC_TEMPFAIL);
    }

    /* the only complication here are literals */
    while (!last || cont) {
	/* if 'cont' is set, we're looking at the continuation to a very
	   long line.
	   if 'last' is set, we've seen the tag we're looking for, we're
	   just reading the end of the line, and we shouldn't echo it. */
	if (!cont) eol[0] = '\0';

	if (!prot_fgets(buf, sizeof(buf), s->in)) {
	    /* uh oh */
	    if(s == backend_current && !force_notfatal)
		fatal("Lost connection to selected backend", EC_UNAVAILABLE);
	    proxyd_downserver(s);
	    return PROXY_NOCONNECTION;
	}
	if (!cont && buf[taglen] == ' ' && !strncmp(tag, buf, taglen)) {
	    strlcpy(s->last_result, buf + taglen + 1, sizeof(s->last_result));
	    /* guarantee that 's->last_result' has \r\n\0 at the end */
	    s->last_result[LAST_RESULT_LEN - 3] = '\r';
	    s->last_result[LAST_RESULT_LEN - 2] = '\n';
	    s->last_result[LAST_RESULT_LEN - 1] = '\0';
	    switch (buf[taglen + 1]) {
	    case 'O': case 'o':
		r = PROXY_OK;
		break;
	    case 'N': case 'n':
		r = PROXY_NO;
		break;
	    case 'B': case 'b':
		r = PROXY_BAD;
		break;
	    default: /* huh? no result? */
		if(s == backend_current && !force_notfatal)
		    fatal("Lost connection to selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		r = PROXY_NOCONNECTION;
		break;
	    }

	    last = 1;
	}
	
	sl = strlen(buf);
	if (sl == (sizeof(buf) - 1) && buf[sl-1] != '\n') {
            /* only got part of a line */
	    /* we save the last 64 characters in case it has important
	       literal information */
	    strcpy(eol, buf + sl - 64);

	    /* write out this part, but we have to keep reading until we
	       hit the end of the line */
	    if (!last) prot_write(proxyd_out, buf, sl);
	    cont = 1;
	    continue;
	} else {		/* we got the end of the line */
	    int i;
	    int litlen = 0, islit = 0;

	    if (!last) prot_write(proxyd_out, buf, sl);

	    /* now we have to see if this line ends with a literal */
	    if (sl < 64) {
		strcat(eol, buf);
	    } else {
		strcat(eol, buf + sl - 63);
	    }

	    /* eol now contains the last characters from the line; we want
	       to see if we've hit a literal */
	    i = strlen(eol);
	    if (i >= 4 &&
		eol[i-1] == '\n' && eol[i-2] == '\r' && eol[i-3] == '}') {
		/* possible literal */
		i -= 4;
		while (i > 0 && eol[i] != '{' && isdigit((int) eol[i])) {
		    i--;
		}
		if (eol[i] == '{') {
		    islit = 1;
		    litlen = atoi(eol + i + 1);
		}
	    }

	    /* copy the literal over */
	    if (islit) {
		while (litlen > 0) {
		    int j = (litlen > sizeof(buf) ? sizeof(buf) : litlen);
		    
		    j = prot_read(s->in, buf, j);
		    if(!j) {
			/* EOF or other error */
			return -1;
		    }
		    if (!last) prot_write(proxyd_out, buf, j);
		    litlen -= j;
		}

		/* none of our saved information has any relevance now */
		eol[0] = '\0';
		
		/* have to keep going for the end of the line */
		cont = 1;
		continue;
	    }
	}

	/* ok, let's read another line */
	cont = 0;
    }

    return r;
}

static int pipe_including_tag(struct backend *s, char *tag, int force_notfatal)
{
    int r;

    r = pipe_until_tag(s, tag, force_notfatal);
    switch (r) {
    case PROXY_OK:
    case PROXY_NO:
    case PROXY_BAD:
	prot_printf(proxyd_out, "%s %s", tag, s->last_result);
	break;
    case PROXY_NOCONNECTION:
	/* don't have to worry about downing the server, since
	 * pipe_until_tag does that for us */
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, 
		    error_message(IMAP_SERVER_UNAVAILABLE));
	break;
    }
    return r;
}

static int pipe_to_end_of_response(struct backend *s, int force_notfatal)
{
    char buf[2048];
    char eol[128];
    int sl;
    int cont = 1, r = PROXY_OK;

    s->timeout->mark = time(NULL) + IDLE_TIMEOUT;
    
    eol[0]='\0';

    /* the only complication here are literals */
    while (cont) {
	/* if 'cont' is set, we're looking at the continuation to a very
	   long line. */
	if (!prot_fgets(buf, sizeof(buf), s->in)) {
	    /* uh oh */
	    if(s == backend_current && !force_notfatal)
		fatal("Lost connection to selected backend", EC_UNAVAILABLE);
	    proxyd_downserver(s);
	    return PROXY_NOCONNECTION;
	}
	
	sl = strlen(buf);
	if (sl == (sizeof(buf) - 1) && buf[sl-1] != '\n') {
            /* only got part of a line */
	    /* we save the last 64 characters in case it has important
	       literal information */
	    strcpy(eol, buf + sl - 64);

	    /* write out this part, but we have to keep reading until we
	       hit the end of the line */
	    prot_write(proxyd_out, buf, sl);
	    cont = 1;
	    continue;
	} else {		/* we got the end of the line */
	    int i;
	    int litlen = 0, islit = 0;

	    prot_write(proxyd_out, buf, sl);

	    /* now we have to see if this line ends with a literal */
	    if (sl < 64) {
		strcat(eol, buf);
	    } else {
		strcat(eol, buf + sl - 63);
	    }

	    /* eol now contains the last characters from the line; we want
	       to see if we've hit a literal */
	    i = strlen(eol);
	    if (i >= 4 &&
		eol[i-1] == '\n' && eol[i-2] == '\r' && eol[i-3] == '}') {
		/* possible literal */
		i -= 4;
		while (i > 0 && eol[i] != '{' && isdigit((int) eol[i])) {
		    i--;
		}
		if (eol[i] == '{') {
		    islit = 1;
		    litlen = atoi(eol + i + 1);
		}
	    }

	    /* copy the literal over */
	    if (islit) {
		while (litlen > 0) {
		    int j = (litlen > sizeof(buf) ? sizeof(buf) : litlen);
		    
		    j = prot_read(s->in, buf, j);
		    if(!j) {
			/* EOF or other error */
			return -1;
		    }
		    prot_write(proxyd_out, buf, j);
		    litlen -= j;
		}

		/* none of our saved information has any relevance now */
		eol[0] = '\0';
		
		/* have to keep going for the end of the line */
		cont = 1;
		continue;
	    }
	}

	/* ok, if we're here, we're done */
	cont = 0;
    }

    return r;
}


/* copy our current input to 's' until we hit a true EOL.

   'optimistic_literal' is how happy we should be about assuming
   that a command will go through by converting synchronizing literals of
   size less than optimistic_literal to nonsync

   returns 0 on success, <0 on big failure, >0 on full command not sent */
static int pipe_command(struct backend *s, int optimistic_literal)
{
    char buf[2048];
    char eol[128];
    int sl;

    s->timeout->mark = time(NULL) + IDLE_TIMEOUT;
    
    eol[0] = '\0';

    /* again, the complication here are literals */
    for (;;) {
	if (!prot_fgets(buf, sizeof(buf), proxyd_in)) {
	    /* uh oh */
	    return -1;
	}

	sl = strlen(buf);

	if (sl == (sizeof(buf) - 1) && buf[sl-1] != '\n') {
            /* only got part of a line */
	    strcpy(eol, buf + sl - 64);

	    /* and write this out, except for what we've saved */
	    prot_write(s->out, buf, sl - 64);
	    continue;
	} else {
	    int i, nonsynch = 0, islit = 0, litlen = 0;

	    if (sl < 64) {
		strcat(eol, buf);
	    } else {
		/* write out what we have, and copy the last 64 characters
		   to eol */
		prot_printf(s->out, "%s", eol);
		prot_write(s->out, buf, sl - 64);
		strcpy(eol, buf + sl - 64);
	    }

	    /* now determine if eol has a literal in it */
	    i = strlen(eol);
	    if (i >= 4 &&
		eol[i-1] == '\n' && eol[i-2] == '\r' && eol[i-3] == '}') {
		/* possible literal */
		i -= 4;
		if (eol[i] == '+') {
		    nonsynch = 1;
		    i--;
		}
		while (i > 0 && eol[i] != '{' && isdigit((int) eol[i])) {
		    i--;
		}
		if (eol[i] == '{') {
		    islit = 1;
		    litlen = atoi(eol + i + 1);
		}
	    }

	    if (islit) {
		if (nonsynch) {
		    prot_write(s->out, eol, strlen(eol));
		} else if (!nonsynch && (litlen <= optimistic_literal)) {
		    prot_printf(proxyd_out, "+ i am an optimist\r\n");
		    prot_write(s->out, eol, strlen(eol) - 3);
		    /* need to insert a + to turn it into a nonsynch */
		    prot_printf(s->out, "+}\r\n");
		} else {
		    /* we do a standard synchronizing literal */
		    prot_write(s->out, eol, strlen(eol));
		    /* but here the game gets tricky... */
		    prot_fgets(buf, sizeof(buf), s->in);
		    /* but for now we cheat */
		    prot_write(proxyd_out, buf, strlen(buf));
		    if (buf[0] != '+' && buf[1] != ' ') {
			/* char *p = strchr(buf, ' '); */
			/* strncpy(s->last_result, p + 1, LAST_RESULT_LEN);*/

			/* stop sending command now */
			return 1;
		    }
		}

		/* gobble literal and sent it onward */
		while (litlen > 0) {
		    int j = (litlen > sizeof(buf) ? sizeof(buf) : litlen);

		    j = prot_read(proxyd_in, buf, j);
		    if(!j) {
			/* EOF or other error */
			return -1;
		    }
		    prot_write(s->out, buf, j);
		    litlen -= j;
		}

		eol[0] = '\0';
		
		/* have to keep going for the send of the command */
		continue;
	    } else {
		/* no literal, so we're done! */
		prot_write(s->out, eol, strlen(eol));

		return 0;
	    }
	}
    }
}

/* This handles piping of the LSUB command, because we have to figure out
 * what mailboxes actually exist before passing them to the end user.
 *
 * It is also needed if we are doing a FIND MAILBOXES, for that we do an
 * LSUB on the backend anyway, because the semantics of FIND do not allow
 * it to return nonexistant mailboxes (RFC1176), but we need to really dumb
 * down the response when this is the case.
 */
static int pipe_lsub(struct backend *s, char *tag, int force_notfatal,
		     const char *resp)
{
    int taglen = strlen(tag);
    int c;
    int r = PROXY_OK;
    int exist_r;
    char mailboxname[MAX_MAILBOX_PATH + 1];
    static struct buf tagb, cmd, sep, name;
    int cur_flags_size = 64;
    char *flags = xmalloc(cur_flags_size);

    const char *end_strip_flags[] = { " \\NonExistent)", "\\NonExistent)",
				      NULL };
    const char *mid_strip_flags[] = { "\\NonExistent ",
				      NULL 
				    };

    assert(s);
    assert(s->timeout);
    
    s->timeout->mark = time(NULL) + IDLE_TIMEOUT;

    while(1) {
	c = getword(s->in, &tagb);

	if(c == EOF) {
	    if(s == backend_current && !force_notfatal)
		fatal("Lost connection to selected backend", EC_UNAVAILABLE);
	    proxyd_downserver(s);
	    free(flags);
	    return PROXY_NOCONNECTION;
	}

	if(!strncmp(tag, tagb.s, taglen)) {
	    char buf[2048];
	    if(!prot_fgets(buf, sizeof(buf), s->in)) {
		if(s == backend_current && !force_notfatal)
		    fatal("Lost connection to selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		free(flags);
		return PROXY_NOCONNECTION;
	    }	
	    /* Got the end of the response */
	    strlcpy(s->last_result, buf, sizeof(s->last_result));
	    /* guarantee that 's->last_result' has \r\n\0 at the end */
	    s->last_result[LAST_RESULT_LEN - 3] = '\r';
	    s->last_result[LAST_RESULT_LEN - 2] = '\n';
	    s->last_result[LAST_RESULT_LEN - 1] = '\0';
	    switch (buf[0]) {
	    case 'O': case 'o':
		r = PROXY_OK;
		break;
	    case 'N': case 'n':
		r = PROXY_NO;
		break;
	    case 'B': case 'b':
		r = PROXY_BAD;
		break;
	    default: /* huh? no result? */
		if(s == backend_current && !force_notfatal)
		    fatal("Lost connection to selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		r = PROXY_NOCONNECTION;
		break;
	    }
	    break; /* we're done */
	}

	c = getword(s->in, &cmd);

	if(c == EOF) {
	    if(s == backend_current && !force_notfatal)
		fatal("Lost connection to selected backend", EC_UNAVAILABLE);
	    proxyd_downserver(s);
	    free(flags);
	    return PROXY_NOCONNECTION;
	}

	if(strncasecmp("LSUB", cmd.s, 4)) {
	    prot_printf(proxyd_out, "%s %s ", tagb.s, cmd.s);
	    r = pipe_to_end_of_response(s, force_notfatal);
	    if(r != PROXY_OK) {
		free(flags);
		return r;
	    }
	} else {
	    /* build up the response bit by bit */
	    int i = 0;
	    char *p;

	    c = prot_getc(s->in);
	    while(c != ')' && c != EOF) {
		flags[i++] = c;
		if(i == cur_flags_size) {
		    /* expand our buffer */
		    cur_flags_size *= 2;
		    flags = xrealloc(flags, cur_flags_size);
		}
		c = prot_getc(s->in);
	    }

	    if(c != EOF) {
		/* terminate string */
		flags[i++] = ')';
		if(i == cur_flags_size) {
		    /* expand our buffer */
		    cur_flags_size *= 2;
		    flags = xrealloc(flags, cur_flags_size);
		}
		flags[i] = '\0';
		/* get the next character */
 		c = prot_getc(s->in);
	    }
	    
	    if(c != ' ') {
		if(s == backend_current && !force_notfatal)
		    fatal("Bad LSUB response from selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		free(flags);
		return PROXY_NOCONNECTION;
	    }

	    /* Check for flags that we should remove
	     * (e.g. NoSelect, NonExistent) */
	    for(i=0; end_strip_flags[i]; i++) {
		p = strstr(flags, end_strip_flags[i]);
		if(p) {
		    *p = ')';
		    *(p+1) = '\0';
		}
	    }

	    for(i=0; mid_strip_flags[i]; i++) {
		int mid_strip_len = strlen(mid_strip_flags[i]);
		p = strstr(flags, mid_strip_flags[i]);
		while(p) {
		    strcpy(p, p + mid_strip_len);
		    p = strstr(flags, mid_strip_flags[i]);
		}
	    }
		
	    /* Get separator */
	    c = getastring(s->in, s->out, &sep);

	    if(c != ' ') {
		if(s == backend_current && !force_notfatal)
		    fatal("Bad LSUB response from selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		free(flags);
		return PROXY_NOCONNECTION;
	    }

	    /* Get name */
	    c = getastring(s->in, s->out, &name);

	    if(c == '\r') c = prot_getc(s->in);
	    if(c != '\n') {
		if(s == backend_current && !force_notfatal)
		    fatal("Bad LSUB response from selected backend",
			  EC_UNAVAILABLE);
		proxyd_downserver(s);
		free(flags);
		return PROXY_NOCONNECTION;
	    }

	    /* lookup name */
	    exist_r = 1;
	    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace,
							name.s,
							proxyd_userid,
							mailboxname);
	    if (!r) {
		int mbtype;
		exist_r = mboxlist_detail(mailboxname, &mbtype,
					  NULL, NULL, NULL, NULL);
		if(!exist_r && (mbtype & MBTYPE_RESERVE))
		    exist_r = IMAP_MAILBOX_RESERVED;
	    } else {
		/* skip this one */
		syslog(LOG_ERR, "could not convert %s to internal form",
		       name.s);
		continue;
	    }

	    /* send our response */
	    /* we need to set \Noselect if it's not in our mailboxes.db */
	    if(resp[0] == 'L') {
		if(!exist_r) {
		    prot_printf(proxyd_out, "* %s %s \"%s\" ",
				resp, flags, sep.s);
		} else {
		    prot_printf(proxyd_out, "* %s (\\Noselect) \"%s\" ",
				resp, sep.s);
		}

		printstring(name.s);
		prot_printf(proxyd_out, "\r\n");

	    } else if(resp[0] == 'M' && !exist_r) {
		/* Note that it has to exist for a find response */
		prot_printf(proxyd_out, "* %s ", resp);
		printastring(name.s);
		prot_printf(proxyd_out, "\r\n");
	    }
	}
    } /* while(1) */

    free(flags);
    return r;
}

void proxyd_downserver(struct backend *s)
{
    if (!s || !s->timeout) {
	/* already disconnected */
	return;
    }

    /* need to logout of server */
    backend_disconnect(s, &protocol[PROTOCOL_IMAP]);

    if(s == backend_inbox) backend_inbox = NULL;
    if(s == backend_current) backend_current = NULL;

    /* remove the timeout */
    prot_removewaitevent(proxyd_in, s->timeout);
    s->timeout = NULL;
}

struct prot_waitevent *backend_timeout(struct protstream *s __attribute__((unused)),
				       struct prot_waitevent *ev, void *rock)
{
    struct backend *be = (struct backend *) rock;

    if (be != backend_current) {
	/* server is not our current server, and idle too long.
	 * down the backend server (removes the event as a side-effect)
	 */
	proxyd_downserver(be);
	return NULL;
    }
    else {
	/* it will timeout in IDLE_TIMEOUT seconds from now */
	ev->mark = time(NULL) + IDLE_TIMEOUT;
	return ev;
    }
}

/* return the connection to the server */
struct backend *proxyd_findserver(const char *server)
{
    int i = 0;
    struct backend *ret = NULL;

    while (backend_cached && backend_cached[i]) {
	if (!strcmp(server, backend_cached[i]->hostname)) {
	    /* xxx do we want to ping/noop the server here? */
	    ret = backend_cached[i];
	    break;
	}
	i++;
    }

    if (!ret || !ret->timeout) {
	char authid[MAX_MAILBOX_NAME+1];

	/* Translate any separators in userid for AUTH to backend */
	strlcpy(authid, proxyd_userid, sizeof(authid));
        mboxname_hiersep_toexternal(&proxyd_namespace, authid,
				    config_virtdomains ?
				    strcspn(authid, "@") : 0);

	/* need to (re)establish connection to server or create one */
	ret = backend_connect(ret, server, &protocol[PROTOCOL_IMAP],
			      authid, NULL);
	if(!ret) return NULL;

	/* add the timeout */
	ret->timeout = prot_addwaitevent(proxyd_in, time(NULL) + IDLE_TIMEOUT,
					 backend_timeout, ret);
    }

    ret->timeout->mark = time(NULL) + IDLE_TIMEOUT;

    /* insert server in list of cached connections */
    if (!backend_cached[i]) {
	backend_cached = (struct backend **) 
	    xrealloc(backend_cached, (i + 2) * sizeof(struct backend *));
	backend_cached[i] = ret;
	backend_cached[i + 1] = NULL;
    }

    return ret;
}

/* proxy mboxlist_lookup; on misses, it asks the listener for this
   machine to make a roundtrip to the master mailbox server to make
   sure it's up to date */
static int mlookup(const char *name, char **pathp, 
		   char **aclp, void *tid)
{
    int r;
    int mbtype = 0;

    if(pathp) *pathp = NULL;

    r = mboxlist_detail(name, &mbtype, pathp, NULL, aclp, tid);
    if (r == IMAP_MAILBOX_NONEXISTENT || (mbtype & MBTYPE_RESERVE)) {
	/* It is not currently active, make sure we have the most recent
	 * copy of the database */
	kick_mupdate();
	r = mboxlist_lookup(name, pathp, aclp, tid);
    }

    /* xxx hide the fact that we are storing partitions */
    if(pathp && *pathp) {
	char *c;
	c = strchr(*pathp, '!');
	if(c) *c = '\0';
    }
    return r;
}

static struct backend *proxyd_findinboxserver(void)
{
    char inbox[MAX_MAILBOX_NAME+1];
    int r;
    char *server = NULL;
    struct backend *s = NULL;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, "INBOX",
						proxyd_userid, inbox);

    if(!r) {
	r = mlookup(inbox, &server, NULL, NULL);
	if (!r) {
	    s = proxyd_findserver(server);
	}
    }
    
    return s;
}

/* proxyd_refer() issues a referral to the client. */
static void proxyd_refer(const char *tag,
			 const char *server,
			 const char *mailbox)
{
    char url[MAX_MAILBOX_PATH + 1];

    if(!strcmp(proxyd_userid, "anonymous")) {
	imapurl_toURL(url, server, mailbox, "ANONYMOUS");
    } else {
	imapurl_toURL(url, server, mailbox, "*");
    }
    
    prot_printf(proxyd_out, "%s NO [REFERRAL %s] Remote mailbox.\r\n", 
		tag, url);
}

static int proxyd_canon_user(sasl_conn_t *conn, void *context,
			     const char *user, unsigned ulen,
			     unsigned flags, const char *user_realm,
			     char *out, unsigned out_max, unsigned *out_ulen)
{
    char userbuf[MAX_MAILBOX_NAME+1], *p;
    size_t n;
    int r;

    if (!ulen) ulen = strlen(user);

    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS)) {
	/* make a working copy of the auth[z]id */
	if (ulen > MAX_MAILBOX_NAME) {
	    sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	    return SASL_BUFOVER;
	}
	/* make a working copy of the auth[z]id */
	memcpy(userbuf, user, ulen);
	userbuf[ulen] = '\0';
	user = userbuf;

	/* See if we're using the magic plus
	   (currently we don't support anything after '+') */
	if ((p = strchr(userbuf, '+')) && 
	    (n = config_virtdomains ? strcspn(p, "@") : strlen(p)) == 1) {

	    if (flags & SASL_CU_AUTHZID) {
		/* make a copy of the magic plus */
		if (proxyd_magicplus) free(proxyd_magicplus);
		proxyd_magicplus = xstrndup(p, n);
	    }

	    /* strip the magic plus from the auth[z]id */
	    memmove(p, p+n, strlen(p+n)+1);
	    ulen -= n;
	}
    }

    r = mysasl_canon_user(conn, context, user, ulen, flags, user_realm,
			  out, out_max, out_ulen);

    if (!r && proxyd_magicplus && flags == SASL_CU_AUTHZID) {
	/* If we're only doing the authzid, put back the magic plus
	   in case its used in the challenge/response calculation */
	n = strlen(proxyd_magicplus);
	if (*out_ulen + n > out_max) {
	    sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	    r = SASL_BUFOVER;
	}
	else {
	    p = (config_virtdomains && (p = strchr(out, '@'))) ?
		p : out + *out_ulen;
	    memmove(p+n, p, strlen(p)+1);
	    memcpy(p, proxyd_magicplus, n);
	    *out_ulen += n;
	}
    }

    return r;
}

static int proxyd_proxy_policy(sasl_conn_t *conn,
			       void *context,
			       const char *requested_user, unsigned rlen,
			       const char *auth_identity, unsigned alen,
			       const char *def_realm,
			       unsigned urlen,
			       struct propctx *propctx)
{
    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS)) {
	char userbuf[MAX_MAILBOX_NAME+1], *p;
	size_t n;

	/* make a working copy of the authzid */
	if (!rlen) rlen = strlen(requested_user);
	if (rlen > MAX_MAILBOX_NAME) {
	    sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	    return SASL_BUFOVER;
	}
	memcpy(userbuf, requested_user, rlen);
	userbuf[rlen] = '\0';
	requested_user = userbuf;

	/* See if we're using the magic plus */
	if ((p = strchr(userbuf, '+'))) {
	    n = config_virtdomains ? strcspn(p, "@") : strlen(p);

	    /* strip the magic plus from the authzid */
	    memmove(p, p+n, strlen(p+n)+1);
	    rlen -= n;
	}
    }

    return mysasl_proxy_policy(conn, context, requested_user, rlen,
			       auth_identity, alen, def_realm, urlen, propctx);
}

static struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, &mysasl_config, NULL },
    { SASL_CB_PROXY_POLICY, &proxyd_proxy_policy, (void*) &proxyd_proxyctx },
    { SASL_CB_CANON_USER, &proxyd_canon_user, (void*) &disable_referrals },
    { SASL_CB_LIST_END, NULL, NULL }
};

extern void setproctitle_init(int argc, char **argv, char **envp);
extern int proc_register(const char *progname, const char *clienthost, 
			 const char *userid, const char *mailbox);
extern void proc_cleanup(void);

/*
 * run once when process is forked;
 * MUST NOT exit directly; must return with non-zero error code
 */
int service_init(int argc, char **argv, char **envp)
{
    int opt;

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);
    setproctitle_init(argc, argv, envp);

    /* set signal handlers */
    signals_set_shutdown(&shut_down);
    signal(SIGPIPE, SIG_IGN);

    /* load the SASL plugins */
    global_sasl_init(1, 1, mysasl_cb);

    /* open the mboxlist, we'll need it for real work */
    mboxlist_init(0);
    mboxlist_open(NULL);

    while ((opt = getopt(argc, argv, "sp:N")) != EOF) {
	switch (opt) {
	case 's': /* imaps (do starttls right away) */
	    imaps = 1;
	    if (!tls_enabled()) {
		syslog(LOG_ERR, "imaps: required OpenSSL options not present");
		fatal("imaps: required OpenSSL options not present",
		      EC_CONFIG);
	    }
	    break;
	case 'p': /* external protection */
	    extprops_ssf = atoi(optarg);
	    break;
	case 'N': /*bypass SASL password check.  Not recommended unless you know
                   *what you're doing */
            nosaslpasswdcheck = 1;
	   syslog( LOG_NOTICE, "setting nosaslpasswdcheck to true" );
            break;
	default:
	    break;
	}
    }

    /* Initialize the annotatemore extention */
    annotatemore_init(0, annotate_fetch_proxy, annotate_store_proxy);
    annotatemore_open(NULL);

    return 0;
}

static void proxyd_reset(void) 
{
    int i;
    
    proc_cleanup();

    /* close backend connections */
    i = 0;
    while (backend_cached[i]) {
	proxyd_downserver(backend_cached[i]);
	free(backend_cached[i]);
	i++;
    }
    free(backend_cached);
    backend_cached = NULL;
    backend_inbox = backend_current = NULL;

    /* Cleanup file descriptors. note: after last call to proxyd_downserver */
    if(proxyd_in) {
	prot_NONBLOCK(proxyd_in);
	prot_fill(proxyd_in);
	
	prot_free(proxyd_in);
    }
    if(proxyd_out) {
	prot_flush(proxyd_out);
	prot_free(proxyd_out);
    }
    
    proxyd_in = proxyd_out = NULL;

#ifdef HAVE_SSL
    if (tls_conn) {
	if (tls_reset_servertls(&tls_conn) == -1) {
	    fatal("tls_reset() failed", EC_TEMPFAIL);
	}
	tls_conn = NULL;
    }
#endif

    cyrus_reset_stdio(); 
    
    /* Cleanup Globals */
    proxyd_cmdcnt = 0;
    disable_referrals = 0;
    supports_referrals = 0;
    proxyd_userisadmin = 0;
    proxyd_starttls_done = 0;
    proxyd_logtime = 0;

    strcpy(proxyd_clienthost, "[local]");

    if(proxyd_logfd != -1) {
	close(proxyd_logfd);
	proxyd_logfd = -1;
    }

    if(proxyd_userid) {
	free(proxyd_userid);
	proxyd_userid = NULL;
    }
    if(proxyd_magicplus != NULL) {
	free(proxyd_magicplus);
	proxyd_magicplus = NULL;
    }
    if(proxyd_authstate) {
	auth_freestate(proxyd_authstate);
	proxyd_authstate = NULL;
    }

    /* Cleanup SASL */
    if(proxyd_saslconn) {
	sasl_dispose(&proxyd_saslconn);
	proxyd_saslconn = NULL;
    }
    if(saslprops.iplocalport) {
	free(saslprops.iplocalport);
	saslprops.iplocalport = NULL;
    }
    if(saslprops.ipremoteport) {
	free(saslprops.ipremoteport);
	saslprops.ipremoteport = NULL;
    }
    if(saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    saslprops.ssf = 0;
}

int service_main(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
{
    socklen_t salen;
    char hbuf[NI_MAXHOST];
    struct sockaddr_storage proxyd_localaddr, proxyd_remoteaddr;
    char localip[60], remoteip[60];
    int niflags;
    int timeout;
    int proxyd_haveaddr = 0;
    sasl_security_properties_t *secprops = NULL;

    signals_poll();

#ifdef ID_SAVE_CMDLINE
    /* get command line args for use in ID before getopt mangles them */
    id_getcmdline(argc, argv);
#endif

    proxyd_in = prot_new(0, 0);
    proxyd_out = prot_new(1, 1);

    /* Find out name of client host */
    salen = sizeof(proxyd_remoteaddr);
    if (getpeername(0, (struct sockaddr *)&proxyd_remoteaddr, &salen) == 0 &&
	(proxyd_remoteaddr.ss_family == AF_INET ||
	 proxyd_remoteaddr.ss_family == AF_INET6)) {
	if (getnameinfo((struct sockaddr *)&proxyd_remoteaddr, salen,
			hbuf, sizeof(hbuf), NULL, 0, NI_NAMEREQD) == 0) {
	    strncpy(proxyd_clienthost, hbuf, sizeof(hbuf));
	    strlcat(proxyd_clienthost, " ", sizeof(proxyd_clienthost));
	} else {
	    proxyd_clienthost[0] = '\0';
	}
	niflags = NI_NUMERICHOST;
#ifdef NI_WITHSCOPEID
	if (((struct sockaddr *)&proxyd_remoteaddr)->sa_family == AF_INET6)
	    niflags |= NI_WITHSCOPEID;
#endif
	if (getnameinfo((struct sockaddr *)&proxyd_remoteaddr, salen, hbuf,
			sizeof(hbuf), NULL, 0, niflags) != 0)
	    strlcpy(hbuf, "unknown", sizeof(hbuf));
	strlcat(proxyd_clienthost, "[", sizeof(proxyd_clienthost));
	strlcat(proxyd_clienthost, hbuf, sizeof(proxyd_clienthost));
	strlcat(proxyd_clienthost, "]", sizeof(proxyd_clienthost));
	salen = sizeof(proxyd_localaddr);
	if (getsockname(0, (struct sockaddr *)&proxyd_localaddr,
			&salen) == 0) {
	    if(iptostring((struct sockaddr *)&proxyd_remoteaddr,
			  salen, remoteip, 60) == 0
	       && iptostring((struct sockaddr *)&proxyd_localaddr,
			     salen, localip, 60) == 0) {
		proxyd_haveaddr = 1;
	    }
	}
    }

    /* create the SASL connection */
    /* Make a SASL connection and setup some properties for it */
    /* other params should be filled in */
    if (sasl_server_new("imap", config_servername, 
			NULL,
			(proxyd_haveaddr ? localip : NULL),
			(proxyd_haveaddr ? remoteip : NULL),
			NULL, 0, 
			&proxyd_saslconn) != SASL_OK) {
	fatal("SASL failed initializing: sasl_server_new()", EC_TEMPFAIL); 
    }

    if(proxyd_haveaddr) {
        saslprops.ipremoteport = xstrdup(remoteip);
        saslprops.iplocalport = xstrdup(localip);
    }    

    secprops = mysasl_secprops(SASL_SEC_NOPLAINTEXT);
    sasl_setprop(proxyd_saslconn, SASL_SEC_PROPS, secprops);
    sasl_setprop(proxyd_saslconn, SASL_SSF_EXTERNAL, &extprops_ssf);

    proc_register("proxyd", proxyd_clienthost, (char *)0, (char *)0);

    /* Set inactivity timer */
    timeout = config_getint(IMAPOPT_TIMEOUT);
    if (timeout < 30) timeout = 30;
    prot_settimeout(proxyd_in, timeout*60);
    prot_setflushonread(proxyd_in, proxyd_out);

    /* setup the cache */
    backend_cached = xmalloc(sizeof(struct backend *));
    backend_cached[0] = NULL;

    /* we were connected on imaps port so we should do 
       TLS negotiation immediately */
    if (imaps == 1) cmd_starttls(NULL, 1);

    cmdloop();
    
    /* cleanup */    
    prot_flush(proxyd_out);
    proxyd_reset();
    
    /* return to service another connection */
    return 0;
}

/* Called by service API to shut down the service */
void service_abort(int error)
{
    shut_down(error);
}

/*
 * found a motd file; spit out message and return
 */
void motd_file(int fd)
{
    struct protstream *motd_in;
    char buf[1024];
    char *p;

    motd_in = prot_new(fd, 0);

    prot_fgets(buf, sizeof(buf), motd_in);
    if ((p = strchr(buf, '\r')) != NULL) *p = 0;
    if ((p = strchr(buf, '\n')) != NULL) *p = 0;

    for(p = buf; *p == '['; p++); /* can't have [ be first char, sigh */
    prot_printf(proxyd_out, "* OK [ALERT] %s\r\n", p);
}

/*
 * Cleanly shut down and exit
 */
void shut_down(int code) __attribute__((noreturn));
void shut_down(int code)
{
    int i;

    proc_cleanup();

    i = 0;
    while (backend_cached && backend_cached[i]) {
	proxyd_downserver(backend_cached[i]);
	free(backend_cached[i]);
	i++;
    }

    mboxlist_close();
    mboxlist_done();

    annotatemore_close();
    annotatemore_done();

    if (proxyd_in) {
	prot_NONBLOCK(proxyd_in);
	prot_fill(proxyd_in);

	prot_free(proxyd_in);
    }

    if (proxyd_out) {
	prot_flush(proxyd_out);

	prot_free(proxyd_out);
    }

#ifdef HAVE_SSL
    tls_shutdown_serverengine();
#endif

    cyrus_done();

    exit(code);
}

void fatal(const char *s, int code)
{
    static int recurse_code = 0;

    if (recurse_code) {
	/* We were called recursively. Just give up */
	proc_cleanup();
	exit(recurse_code);
    }
    recurse_code = code;
    if (proxyd_out) {
	prot_printf(proxyd_out, "* BYE Fatal error: %s\r\n", s);
	prot_flush(proxyd_out);
    }
    shut_down(code);
}

/*
 * Top-level command loop parsing
 */
void cmdloop()
{
    int fd;
    char motdfilename[1024];
    char hostname[MAXHOSTNAMELEN+1];
    int c;
    int usinguid, havepartition, havenamespace;
    static struct buf tag, cmd, arg1, arg2, arg3, arg4;
    char *p, shut[1024];
    const char *err;

    snprintf(shutdownfilename, sizeof(shutdownfilename), 
	     "%s/msg/shutdown", config_dir);

    gethostname(hostname, sizeof(hostname));
    prot_printf(proxyd_out,
		"* OK %s Cyrus IMAP4 Murder %s server ready\r\n", hostname,
		CYRUS_VERSION);

    snprintf(motdfilename, sizeof(motdfilename), "%s/msg/motd", config_dir);
    if ((fd = open(motdfilename, O_RDONLY, 0)) != -1) {
	motd_file(fd);
	close(fd);
    }

    for (;;) {
	if (! proxyd_userisadmin && shutdown_file(shut, sizeof(shut))) {
	    for (p = shut; *p == '['; p++); /* can't have [ be first char */
	    prot_printf(proxyd_out, "* BYE [ALERT] %s\r\n", p);
	    shut_down(0);
	}

	signals_poll();

	/* Parse tag */
	c = getword(proxyd_in, &tag);
	if (c == EOF) {
	    err = prot_error(proxyd_in);
	    if (err) {
		syslog(LOG_WARNING, "PROTERR: %s", err);
		prot_printf(proxyd_out, "* BYE %s\r\n", err);
	    }
	    return;
	}
	if (c != ' ' || !imparse_isatom(tag.s) || 
	    (tag.s[0] == '*' && !tag.s[1])) {
	    prot_printf(proxyd_out, "* BAD Invalid tag\r\n");
	    eatline(proxyd_in, c);
	    continue;
	}

	/* Parse command name */
	c = getword(proxyd_in, &cmd);
	if (!cmd.s[0]) {
	    prot_printf(proxyd_out, "%s BAD Null command\r\n", tag.s);
	    eatline(proxyd_in, c);
	    continue;
	}
	if (islower((unsigned char) cmd.s[0])) cmd.s[0] = toupper(cmd.s[0]);
	for (p = &cmd.s[1]; *p; p++) {
	    if (isupper((unsigned char) *p)) *p = tolower(*p);
	}

	/* if we need to force a kick, do so */
	if(referral_kick) {
	    kick_mupdate();
	    referral_kick = 0;
	}
	
	/* Only Authenticate/Login/Logout/Noop/Starttls 
	   allowed when not logged in */
	if (!proxyd_userid && !strchr("ALNCIS", cmd.s[0])) goto nologin;
    	switch (cmd.s[0]) {
	case 'A':
	    if (!strcmp(cmd.s, "Authenticate")) {
		int haveinitresp = 0;

		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg1);
		if (!imparse_isatom(arg1.s)) {
		    prot_printf(proxyd_out, 
				"%s BAD Invalid authenticate mechanism\r\n", 
				tag.s);
		    eatline(proxyd_in, c);
		    continue;
		}
		if (c == ' ') {
		    haveinitresp = 1;
		    c = getword(proxyd_in, &arg2);
		    if (c == EOF) goto missingargs;
		}
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		
		if (proxyd_userid) {
		    prot_printf(proxyd_out, 
				"%s BAD Already authenticated\r\n", tag.s);
		    continue;
		}
		cmd_authenticate(tag.s, arg1.s, haveinitresp ? arg2.s : NULL);
	    }
	    else if (!proxyd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Append")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_append(tag.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'B':
	    if (!strcmp(cmd.s, "Bboard")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'C':
	    if (!strcmp(cmd.s, "Capability")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_capability(tag.s);
	    }
	    else if (!proxyd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Check")) {
		if (!backend_current) goto nomailbox;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_noop(tag.s, cmd.s);
	    }
	    else if (!strcmp(cmd.s, "Copy")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    copy:
		c = getword(proxyd_in, &arg1);
		if (c == '\r') goto missingargs;
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;

		cmd_copy(tag.s, arg1.s, arg2.s, usinguid);
	    }
	    else if (!strcmp(cmd.s, "Create")) {
		havepartition = 0;
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == ' ') {
		    havepartition = 1;
		    c = getword(proxyd_in, &arg2);
		    if (!imparse_isatom(arg2.s)) goto badpartition;
		}
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_create(tag.s, arg1.s, havepartition ? arg2.s : 0);
	    }
	    else if (!strcmp(cmd.s, "Close")) {
		if (!backend_current) goto nomailbox;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_close(tag.s);
	    }
	    else goto badcmd;
	    break;

	case 'D':
	    if (!strcmp(cmd.s, "Delete")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_delete(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Deleteacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_setacl(tag.s, arg1.s, arg2.s, (char *)0);
	    }
	    else goto badcmd;
	    break;

	case 'E':
	    if (!strcmp(cmd.s, "Expunge")) {
		if (!backend_current) goto nomailbox;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_expunge(tag.s, 0);
	    }
	    else if (!strcmp(cmd.s, "Examine")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'F':
	    if (!strcmp(cmd.s, "Fetch")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    fetch:
		c = getword(proxyd_in, &arg1);
		if (c == '\r') goto missingargs;
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;
		cmd_fetch(tag.s, arg1.s, usinguid);
	    }
	    else if (!strcmp(cmd.s, "Find")) {
		c = getword(proxyd_in, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_find(tag.s, arg1.s, arg2.s);
	    }
	    else goto badcmd;
	    break;

	case 'G':
	    if (!strcmp(cmd.s, "Getacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_getacl(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Getannotation")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_getannotation(tag.s, arg1.s);

		snmp_increment(GETANNOTATION_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Getquota")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_getquota(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Getquotaroot")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_getquotaroot(tag.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'I':
	    if (!strcmp(cmd.s, "Id")) {
		if (c != ' ') goto missingargs;
		cmd_id(tag.s);
	    }
	    else if (!proxyd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Idle")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_idle(tag.s);
	    }
	    else goto badcmd;
	    break;

	case 'L':
	    if (!strcmp(cmd.s, "Login")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if(c != ' ') goto missingargs;

		cmd_login(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Logout")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		
		prot_printf(proxyd_out, 
			    "* BYE %s\r\n", error_message(IMAP_BYE_LOGOUT));
		prot_printf(proxyd_out, "%s OK %s\r\n", 
			    tag.s, error_message(IMAP_OK_COMPLETED));

		return;
	    }
	    else if (!proxyd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "List")) {
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, proxyd_magicplus ? LIST_SUBSCRIBED : 0,
			 arg1.s, arg2.s);
	    }
	    else if (!strcmp(cmd.s, "Lsub")) {
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, 1, arg1.s, arg2.s);
	    }
	    else if (!strcmp(cmd.s, "Listrights")) {
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_listrights(tag.s, arg1.s, arg2.s);
	    }
	    else goto badcmd;
	    break;

	case 'M':
	    if (!strcmp(cmd.s, "Myrights")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_myrights(tag.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'N':
	    if (!strcmp(cmd.s, "Noop")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_noop(tag.s, cmd.s);
	    }
#ifdef ENABLE_X_NETSCAPE_HACK
	    else if (!strcmp(cmd.s, "Netscape")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_netscape(tag.s);
	    }
#endif
	    else if (!proxyd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Namespace")) {
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_namespace(tag.s);
	    }
	    else goto badcmd;
	    break;

	case 'P':
	    if (!strcmp(cmd.s, "Partial")) {
		if (!backend_current) goto nomailbox;
		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg1);
		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg2);
		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg3);
		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg4);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_partial(tag.s, arg1.s, arg2.s, arg3.s, arg4.s);
	    }
	    else goto badcmd;
	    break;

	case 'R':
	    if (!strcmp(cmd.s, "Rename")) {
		havepartition = 0;
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == ' ') {
		    havepartition = 1;
		    c = getword(proxyd_in, &arg3);
		    if (!imparse_isatom(arg3.s)) goto badpartition;
		}
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_rename(tag.s, arg1.s, arg2.s, havepartition ? arg3.s : 0);
	    }
	    else if (!strcmp(cmd.s, "Rlist")) {
		supports_referrals = !disable_referrals;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, 0, arg1.s, arg2.s);
	    }
	    else if (!strcmp(cmd.s, "Rlsub")) {
		supports_referrals = !disable_referrals;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, 1, arg1.s, arg2.s);
	    } else if(!strcmp(cmd.s, "Reconstruct")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if(c == ' ') {
		    /* Optional RECURSEIVE argument */
		    c = getword(proxyd_in, &arg2);
		    if(!imparse_isatom(arg2.s))
			goto extraargs;
		    else if(strcasecmp(arg2.s, "RECURSIVE"))
			goto extraargs;
		    /* we ignore the argument, because proxyd does not care */
		}
		if(c == '\r') c = prot_getc(proxyd_in);
		if(c != '\n') goto extraargs;
		cmd_reconstruct(tag.s, arg1.s);
	    }
	    else goto badcmd;
	    break;
	    
	case 'S':
	    if (!strcmp(cmd.s, "Starttls")) {
		if (!tls_enabled()) {
		    /* we don't support starttls */
		    goto badcmd;
		}

		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;

		/* if we've already done SASL fail */
		if (proxyd_userid != NULL) {
		    prot_printf(proxyd_out, 
		     "%s BAD Can't Starttls after authentication\r\n", tag.s);
		    continue;
		}
		
		/* check if already did a successful tls */
		if (proxyd_starttls_done == 1) {
		    prot_printf(proxyd_out, 
				"%s BAD Already did a successful Starttls\r\n",
				tag.s);
		    continue;
		}
		cmd_starttls(tag.s, 0);	

		continue;
	    }

	    if (!proxyd_userid) {
		goto nologin;
	    } else if (!strcmp(cmd.s, "Store")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    store:
		c = getword(proxyd_in, &arg1);
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;
		c = getword(proxyd_in, &arg2);
		if (c != ' ') goto badsequence;
		cmd_store(tag.s, arg1.s, arg2.s, usinguid);
	    }
	    else if (!strcmp(cmd.s, "Select")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Search")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    search:
		cmd_search(tag.s, usinguid);
	    }
	    else if (!strcmp(cmd.s, "Subscribe")) {
		if (c != ' ') goto missingargs;
		havenamespace = 0;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == ' ') {
		    havenamespace = 1;
		    c = getastring(proxyd_in, proxyd_out, &arg2);
		}
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		if (havenamespace) {
		    cmd_changesub(tag.s, arg1.s, arg2.s, 1);
		}
		else {
		    cmd_changesub(tag.s, (char *)0, arg1.s, 1);
		}
	    }		
	    else if (!strcmp(cmd.s, "Setacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg2);
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg3);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_setacl(tag.s, arg1.s, arg2.s, arg3.s);
	    }
	    else if (!strcmp(cmd.s, "Setannotation")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_setannotation(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Setquota")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		cmd_setquota(tag.s, arg1.s);
	    }
	    else if (!strcmp(cmd.s, "Sort")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    sort:
		cmd_sort(tag.s, usinguid);
	    }
	    else if (!strcmp(cmd.s, "Status")) {
		if (c != ' ') goto missingargs;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c != ' ') goto missingargs;
		cmd_status(tag.s, arg1.s);
	    }
	    else goto badcmd;
	    break;

	case 'T':
	    if (!strcmp(cmd.s, "Thread")) {
		if (!backend_current) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    thread:
		cmd_thread(tag.s, usinguid);
	    }
	    else goto badcmd;
	    break;

	case 'U':
	    if (!strcmp(cmd.s, "Uid")) {
		if (!backend_current) goto nomailbox;
		usinguid = 1;
		if (c != ' ') goto missingargs;
		c = getword(proxyd_in, &arg1);
		if (c != ' ') goto missingargs;
		lcase(arg1.s);
		if (!strcmp(arg1.s, "fetch")) {
		    goto fetch;
		}
		else if (!strcmp(arg1.s, "store")) {
		    goto store;
		}
		else if (!strcmp(arg1.s, "search")) {
		    goto search;
		}
		else if (!strcmp(arg1.s, "sort")) {
		    goto sort;
		}
		else if (!strcmp(arg1.s, "thread")) {
		    goto thread;
		}
		else if (!strcmp(arg1.s, "copy")) {
		    goto copy;
		}
		else if (!strcmp(arg1.s, "expunge")) {
		    c = getword(proxyd_in, &arg1);
		    if (!imparse_issequence(arg1.s)) goto badsequence;
		    if (c == '\r') c = prot_getc(proxyd_in);
		    if (c != '\n') goto extraargs;
		    cmd_expunge(tag.s, arg1.s);
		}
		else {
		    prot_printf(proxyd_out, 
				"%s BAD Unrecognized UID subcommand\r\n", 
				tag.s);
		    eatline(proxyd_in, c);
		}
	    }
	    else if (!strcmp(cmd.s, "Unsubscribe")) {
		if (c != ' ') goto missingargs;
		havenamespace = 0;
		c = getastring(proxyd_in, proxyd_out, &arg1);
		if (c == ' ') {
		    havenamespace = 1;
		    c = getastring(proxyd_in, proxyd_out, &arg2);
		}
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		if (havenamespace) {
		    cmd_changesub(tag.s, arg1.s, arg2.s, 0);
		}
		else {
		    cmd_changesub(tag.s, (char *)0, arg1.s, 0);
		}
	    }		
	    else if (!strcmp(cmd.s, "Unselect")) {
		if (!backend_current) goto nomailbox;
		if (c == '\r') c = prot_getc(proxyd_in);
		if (c != '\n') goto extraargs;
		cmd_unselect(tag.s);
	    }
	    else goto badcmd;
	    break;

	default:
	badcmd:
	    prot_printf(proxyd_out, "%s BAD Unrecognized command\r\n", tag.s);
	    eatline(proxyd_in, c);
	}

	continue;

    nologin:
	prot_printf(proxyd_out, "%s BAD Please login first\r\n", tag.s);
	eatline(proxyd_in, c);
	continue;

    nomailbox:
	prot_printf(proxyd_out, "%s BAD Please select a mailbox first\r\n", 
		    tag.s);
	eatline(proxyd_in, c);
	continue;

    missingargs:
	prot_printf(proxyd_out, "%s BAD Missing required argument to %s\r\n", 
		    tag.s, cmd.s);
	eatline(proxyd_in, c);
	continue;

    extraargs:
	prot_printf(proxyd_out, "%s BAD Unexpected extra arguments to %s\r\n",
		    tag.s, cmd.s);
	eatline(proxyd_in, c);
	continue;

    badsequence:
	prot_printf(proxyd_out, "%s BAD Invalid sequence in %s\r\n", 
		    tag.s, cmd.s);
	eatline(proxyd_in, c);
	continue;

    badpartition:
	prot_printf(proxyd_out, "%s BAD Invalid partition name in %s\r\n",
		    tag.s, cmd.s);
	eatline(proxyd_in, c);
	continue;
    }
}

/*
 * Perform a LOGIN command
 */
void cmd_login(char *tag, char *user)
{
    char userbuf[MAX_MAILBOX_NAME+1], *canon_user = userbuf;
    unsigned userlen;
    char c;
    struct buf passwdbuf;
    char *passwd;
    char *reply = 0;
    int plaintextloginpause;
    int r;

    if (proxyd_userid) {
	eatline(proxyd_in, ' ');
	prot_printf(proxyd_out, "%s BAD Already logged in\r\n", tag);
	return;
    }

    r = proxyd_canon_user(proxyd_saslconn, &disable_referrals, user, 0,
			  SASL_CU_AUTHID | SASL_CU_AUTHZID, NULL,
			  userbuf, sizeof(userbuf), &userlen);

    if (r) {
	syslog(LOG_NOTICE, "badlogin: %s plaintext %s invalid user",
	       proxyd_clienthost, beautify_string(user));
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, 
		    error_message(IMAP_INVALID_USER));
	return;
    }

    /* possibly disallow login */
    if ((proxyd_starttls_done == 0) &&
	(config_getswitch(IMAPOPT_ALLOWPLAINTEXT) == 0) &&
	strcmp(canon_user, "anonymous") != 0) {
	eatline(proxyd_in, ' ');
	prot_printf(proxyd_out, "%s NO Login only available under a layer\r\n",
		    tag);
	return;
    }

    memset(&passwdbuf,0,sizeof(struct buf));
    c = getastring(proxyd_in, proxyd_out, &passwdbuf);

    if(c == '\r') c = prot_getc(proxyd_in);
    if (c != '\n') {
	freebuf(&passwdbuf);
	prot_printf(proxyd_out,
		    "%s BAD Unexpected extra arguments to LOGIN\r\n",
		    tag);
	eatline(proxyd_in, c);
	return;
    }

    passwd = passwdbuf.s;

    if (!strcmp(canon_user, "anonymous")) {
	if (config_getswitch(IMAPOPT_ALLOWANONYMOUSLOGIN)) {
	    passwd = beautify_string(passwd);
	    if (strlen(passwd) > 500) passwd[500] = '\0';
	    syslog(LOG_NOTICE, "login: %s anonymous %s",
		   proxyd_clienthost, passwd);
	    reply = "Anonymous access granted";
	    proxyd_userid = xstrdup("anonymous");
	}
	else {
	    syslog(LOG_NOTICE, "badlogin: %s anonymous login refused",
		   proxyd_clienthost);
	    prot_printf(proxyd_out, "%s NO %s\r\n", tag,
		   error_message(IMAP_ANONYMOUS_NOT_PERMITTED));
	    freebuf(&passwdbuf);
	    return;
	}
    }
    else if ( nosaslpasswdcheck ) {
        /* bypassing sasl_checkpass() */
        proxyd_userid = xstrdup(canon_user);
        syslog( LOG_NOTICE, "bypassing sasl_checkpass()" );  
    }
    else if ((r = sasl_checkpass(proxyd_saslconn,
				 canon_user,
				 strlen(canon_user),
				 passwd,
				 strlen(passwd)))!=SASL_OK) {
	const char *errorstring = sasl_errstring(r, NULL, NULL);
	if (reply) {
	    syslog(LOG_NOTICE, "badlogin: %s plaintext %s %s",
		   proxyd_clienthost, canon_user, reply);
	}
	/* Apply penalty only if not under layer */
	if (proxyd_starttls_done == 0)
	    sleep(3);
	if (errorstring) {
	    prot_printf(proxyd_out, "%s NO Login failed: %s\r\n", 
			tag, errorstring);
	} else {
	    prot_printf(proxyd_out, "%s NO Login failed.", tag);
	}
	freebuf(&passwdbuf);
	return;
    }
    else {
	proxyd_userid = xstrdup(canon_user);

	syslog(LOG_NOTICE, "login: %s %s%s plaintext%s %s", proxyd_clienthost,
	       proxyd_userid, proxyd_magicplus ? proxyd_magicplus : "",
	       proxyd_starttls_done ? "+TLS" : "", 
	       reply ? reply : "");

	plaintextloginpause = config_getint(IMAPOPT_PLAINTEXTLOGINPAUSE);
	if (plaintextloginpause) {

	    /* Apply penalty only if not under layer */
	    if (proxyd_starttls_done == 0)
		sleep(plaintextloginpause);
	}
    }
    
    proxyd_authstate = auth_newstate(proxyd_userid);

    proxyd_userisadmin = global_authisa(proxyd_authstate, IMAPOPT_ADMINS);

    if (!reply) reply = "User logged in";

    prot_printf(proxyd_out, "%s OK %s\r\n", tag, reply);

    /* Create telemetry log */
    proxyd_logfd = telemetry_log(proxyd_userid, proxyd_in, proxyd_out, 0);

    /* Set namespace */
    if ((r = mboxname_init_namespace(&proxyd_namespace, proxyd_userisadmin)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    /* Translate any separators in userid */
    mboxname_hiersep_tointernal(&proxyd_namespace, proxyd_userid,
				config_virtdomains ?
				strcspn(proxyd_userid, "@") : 0);

    freebuf(&passwdbuf);
    return;
}

/*
 * Perform an AUTHENTICATE command
 */
void cmd_authenticate(char *tag, char *authtype, char *resp)
{
    int sasl_result;
    const char *canon_user;
    
    const int *ssfp;
    char *ssfmsg=NULL;

    int r;

    r = saslserver(proxyd_saslconn, authtype, resp, "", "+ ", "",
		   proxyd_in, proxyd_out, &sasl_result, NULL);

    if (r) {
	const char *errorstring = NULL;

	switch (r) {
	case IMAP_SASL_CANCEL:
	    prot_printf(proxyd_out,
			"%s BAD Client canceled authentication\r\n", tag);
	    break;
	case IMAP_SASL_PROTERR:
	    errorstring = prot_error(proxyd_in);

	    prot_printf(proxyd_out,
			"%s NO Error reading client response: %s\r\n",
			tag, errorstring ? errorstring : "");
	    break;
	default: 
	    /* failed authentication */
	    errorstring = sasl_errstring(sasl_result, NULL, NULL);

	    syslog(LOG_NOTICE, "badlogin: %s %s [%s]",
		   proxyd_clienthost, authtype, sasl_errdetail(proxyd_saslconn));

	    snmp_increment_args(AUTHENTICATION_NO, 1,
				VARIABLE_AUTH, 0, /* hash_simple(authtype) */ 
				VARIABLE_LISTEND);
	    sleep(3);

	    if (errorstring) {
		prot_printf(proxyd_out, "%s NO %s\r\n", tag, errorstring);
	    } else {
		prot_printf(proxyd_out, "%s NO Error authenticating\r\n", tag);
	    }
	}

	reset_saslconn(&proxyd_saslconn);
	return;
    }

    /* successful authentication */

    /* get the userid from SASL --- already canonicalized from
     * mysasl_proxy_policy()
     */
    sasl_result = sasl_getprop(proxyd_saslconn, SASL_USERNAME,
			       (const void **)&canon_user);

    if (sasl_result!=SASL_OK)
    {
	prot_printf(proxyd_out, "%s NO weird SASL error %d SASL_USERNAME\r\n", 
		    tag, sasl_result);
	syslog(LOG_ERR, "weird SASL error %d getting SASL_USERNAME", 
	       sasl_result);
	reset_saslconn(&proxyd_saslconn);
	return;
    }

    /* If we're proxying, the authzid may contain a magic plus,
       so re-canonify it */
    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS) && strchr(canon_user, '+')) {
	char userbuf[MAX_MAILBOX_NAME+1];
	unsigned userlen;

	sasl_result = proxyd_canon_user(proxyd_saslconn, NULL, canon_user, 0,
					SASL_CU_AUTHID | SASL_CU_AUTHZID,
					NULL, userbuf, sizeof(userbuf), &userlen);
	if (sasl_result != SASL_OK) {
	    prot_printf(proxyd_out, 
			"%s NO SASL canonification error %d\r\n", 
			tag, sasl_result);
	    reset_saslconn(&proxyd_saslconn);
	    return;
	}

	proxyd_userid = xstrdup(userbuf);
    } else {
	proxyd_userid = xstrdup(canon_user);
    }

    proc_register("proxyd", proxyd_clienthost, proxyd_userid, (char *)0);

    syslog(LOG_NOTICE, "login: %s %s%s %s%s %s", proxyd_clienthost,
	   proxyd_userid, proxyd_magicplus ? proxyd_magicplus : "",
	   authtype, proxyd_starttls_done ? "+TLS" : "", "User logged in");

    sasl_getprop(proxyd_saslconn, SASL_SSF, (const void **) &ssfp);

    if (proxyd_starttls_done) {
	switch(*ssfp) {
	case 0: ssfmsg = "tls protection"; break;
	case 1: ssfmsg = "tls plus integrity protection"; break;
	default: ssfmsg = "tls plus privacy protection"; break;
	}
    } else {
	switch(*ssfp) {
	case 0: ssfmsg="no protection"; break;
	case 1: ssfmsg="integrity protection"; break;
	default: ssfmsg="privacy protection"; break;
	}
    }

    prot_printf(proxyd_out, "%s OK Success (%s)\r\n", tag,ssfmsg);
    prot_flush(proxyd_out);

    prot_setsasl(proxyd_in,  proxyd_saslconn);
    prot_setsasl(proxyd_out, proxyd_saslconn);

    /* Create telemetry log */
    proxyd_logfd = telemetry_log(proxyd_userid, proxyd_in, proxyd_out, 0);

    /* Set namespace */
    if ((r = mboxname_init_namespace(&proxyd_namespace, proxyd_userisadmin)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    /* Translate any separators in userid */
    mboxname_hiersep_tointernal(&proxyd_namespace, proxyd_userid,
				config_virtdomains ?
				strcspn(proxyd_userid, "@") : 0);

    return;
}

/*
 * Perform a NOOP command
 */
void cmd_noop(char *tag, char *cmd)
{
    if (backend_current) {
	prot_printf(backend_current->out, "%s %s\r\n", tag, cmd);
	pipe_including_tag(backend_current, tag, 0);
    } else {
	prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}

/*
 * Parse and perform an ID command.
 *
 * the command has been parsed up to the parameter list.
 *
 * we only allow one ID in non-authenticated state from a given client.
 * we only allow MAXIDFAILED consecutive failed IDs from a given client.
 * we only record MAXIDLOG ID responses from a given client.
 */
void cmd_id(char *tag)
{
    static int did_id = 0;
    static int failed_id = 0;
    static int logged_id = 0;
    int error = 0;
    int c = EOF, npair = 0;
    static struct buf arg, field;
    struct idparamlist *params = 0;

    /* check if we've already had an ID in non-authenticated state */
    if (!proxyd_userid && did_id) {
	prot_printf(proxyd_out,
		    "%s NO Only one Id allowed in non-authenticated state\r\n",
		    tag);
	eatline(proxyd_in, c);
	return;
    }

    /* check if we've had too many failed IDs in a row */
    if (failed_id >= MAXIDFAILED) {
	prot_printf(proxyd_out, "%s NO Too many (%u) invalid Id commands\r\n",
		    tag, failed_id);
	eatline(proxyd_in, c);
	return;
    }

    /* ok, accept parameter list */
    c = getword(proxyd_in, &arg);
    /* check for "NIL" or start of parameter list */
    if (strcasecmp(arg.s, "NIL") && c != '(') {
	prot_printf(proxyd_out, "%s BAD Invalid parameter list in Id\r\n", tag);
	eatline(proxyd_in, c);
	failed_id++;
	return;
    }

    /* parse parameter list */
    if (c == '(') {
	for (;;) {
	    if (c == ')') {
		/* end of string/value pairs */
		break;
	    }

	    /* get field name */
	    c = getstring(proxyd_in, proxyd_out, &field);
	    if (c != ' ') {
		prot_printf(proxyd_out,
			    "%s BAD Invalid/missing field name in Id\r\n",
			    tag);
		error = 1;
		break;
	    }

	    /* get field value */
	    c = getnstring(proxyd_in, proxyd_out, &arg);
	    if (c != ' ' && c != ')') {
		prot_printf(proxyd_out,
			    "%s BAD Invalid/missing value in Id\r\n",
			    tag);
		error = 1;
		break;
	    }

	    /* ok, we're anal, but we'll still process the ID command */
	    if (strlen(field.s) > MAXIDFIELDLEN) {
		prot_printf(proxyd_out, 
			    "%s BAD field longer than %u octets in Id\r\n",
			    tag, MAXIDFIELDLEN);
		error = 1;
		break;
	    }
	    if (strlen(arg.s) > MAXIDVALUELEN) {
		prot_printf(proxyd_out,
			    "%s BAD value longer than %u octets in Id\r\n",
			    tag, MAXIDVALUELEN);
		error = 1;
		break;
	    }
	    if (++npair > MAXIDPAIRS) {
		prot_printf(proxyd_out,
			    "%s BAD too many (%u) field-value pairs in ID\r\n",
			    tag, MAXIDPAIRS);
		error = 1;
		break;
	    }
	    
	    /* ok, we're happy enough */
	    id_appendparamlist(&params, field.s, arg.s);
	}

	if (error || c != ')') {
	    /* erp! */
	    eatline(proxyd_in, c);
	    id_freeparamlist(params);
	    failed_id++;
	    return;
	}
	c = prot_getc(proxyd_in);
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(proxyd_in);
    if (c != '\n') {
	prot_printf(proxyd_out,
		    "%s BAD Unexpected extra arguments to Id\r\n", tag);
	eatline(proxyd_in, c);
	id_freeparamlist(params);
	failed_id++;
	return;
    }

    /* log the client's ID string.
       eventually this should be a callback or something. */
    if (npair && logged_id < MAXIDLOG) {
	char logbuf[MAXIDLOGLEN + 1] = "";
	struct idparamlist *pptr;

	for (pptr = params; pptr; pptr = pptr->next) {
	    /* should we check for and format literals here ??? */
	    snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
		     " \"%s\" ", pptr->field);
	    if (!strcmp(pptr->value, "NIL"))
		snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
			 "NIL");
	    else
		snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
			"\"%s\"", pptr->value);
	}

	syslog(LOG_INFO, "client id:%s", logbuf);

	logged_id++;
    }

    id_freeparamlist(params);

    /* spit out our ID string.
       eventually this might be configurable. */
    if (config_getswitch(IMAPOPT_IMAPIDRESPONSE)) {
	id_response(proxyd_out);

	/* add info about the backend */
	if (backend_current)
	    prot_printf(proxyd_out, " \"backend-url\" \"imap://%s\"",
			backend_current->hostname);
	else
	    prot_printf(proxyd_out, " \"backend-url\" NIL");

	prot_printf(proxyd_out, ")\r\n");
    }
    else
	prot_printf(proxyd_out, "* ID NIL\r\n");

    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));

    failed_id = 0;
    did_id = 1;
}

/*
 * Append the 'field' / 'value' pair to the idparamlist 'l'.
 */
void id_appendparamlist(struct idparamlist **l, char *field, char *value)
{
    struct idparamlist **tail = l;

    while (*tail) tail = &(*tail)->next;

    *tail = (struct idparamlist *)xmalloc(sizeof(struct idparamlist));
    (*tail)->field = xstrdup(field);
    (*tail)->value = xstrdup(value);
    (*tail)->next = 0;
}

/*
 * Free the idparamlist 'l'
 */
void id_freeparamlist(struct idparamlist *l)
{
    struct idparamlist *n;

    while (l) {
	n = l->next;
	free(l->field);
	free(l->value);
	l = n;
    }
}

/*
 * Perform an IDLE command
 */
void cmd_idle(char *tag)
{
    static int idle_period = -1;
    static struct buf arg;
    struct protgroup *protin = protgroup_new(2);
    struct protgroup *protout = NULL;
    struct timeval timeout;
    int c = EOF, n, done = 0, shutdown = 0;
    char buf[2048], shut[1024];

    /* get polling period */
    if (idle_period == -1) {
      idle_period = config_getint(IMAPOPT_IMAPIDLEPOLL);
      if (idle_period < 1) idle_period = 0;
    }

    if (!idle_period) {
	/* IDLE has been disabled */
	prot_printf(proxyd_out, "%s BAD Unrecognized command\r\n", tag);
	return;
    }

    /* Reset protin to all zeros (to preserve memory allocation) */
    protgroup_reset(protin);
    protgroup_insert(protin, proxyd_in);

    if (backend_current && CAPA(backend_current, CAPA_IDLE)) {
	/* Start IDLE on backend */
	prot_printf(backend_current->out, "%s IDLE\r\n", tag);
	if (!prot_fgets(buf, sizeof(buf), backend_current->in)) {

	    /* If we received nothing from the backend, fail */
	    prot_printf(proxyd_out, "%s NO %s\r\n", tag, 
			error_message(IMAP_SERVER_UNAVAILABLE));
	    goto done;
	}
	if (buf[0] != '+') {

	    /* If we received anything but a continuation response,
	       spit out what we received and quit */
	    prot_write(proxyd_out, buf, strlen(buf));
	    goto done;
	}

	protgroup_insert(protin, backend_current->in);
    }

    /* Tell client we are idling and waiting for end of command */
    prot_printf(proxyd_out, "+ go ahead\r\n");
    prot_flush(proxyd_out);

    while (!done) {
	/* check for shutdown file */
	if (!proxyd_userisadmin && shutdown_file(shut, sizeof(shut))) {
	    shutdown = done = 1;
	    goto done;
	}

	if (backend_current && !CAPA(backend_current, CAPA_IDLE)) {
	    /* Simulate IDLE by polling the backend */
	    char mytag[128];
	
	    proxyd_gentag(mytag, sizeof(mytag));
	    prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	    pipe_until_tag(backend_current, mytag, 0);
	    prot_flush(proxyd_out);
	}

	/* Clear protout if needed */
	protgroup_free(protout);
	protout = NULL;

	timeout.tv_sec = idle_period;
	timeout.tv_usec = 0;

	n = prot_select(protin, PROT_NO_FD, &protout, NULL, &timeout);
	if (n == -1) {
	    syslog(LOG_ERR, "prot_select() failed in cmd_idle(): %m");
	    fatal("prot_select() failed in cmd_idle()", EC_TEMPFAIL);
	}
	if (n && protout) {
	    struct protstream *ptmp;

	    for (; n; n--) {
		ptmp = protgroup_getelement(protout, n-1);
		if (ptmp == proxyd_in) {
		    /* The client sent us something, we're done */
		    done = 1;
		}
		else if (backend_current && ptmp == backend_current->in) {
		    /* Get unsolicited untagged responses from the backend */
		    do {
			int c = prot_read(backend_current->in, buf, sizeof(buf));
			if (c == 0 || c < 0) break;
			prot_write(proxyd_out, buf, c);
		    } while (backend_current->in->cnt > 0);
		    prot_flush(proxyd_out);

		    if (prot_error(backend_current->in)) {
			/* uh oh, we're not happy */
			fatal("Lost connection to selected backend",
			      EC_UNAVAILABLE);
		    }
		}
		else {
		    /* XXX shouldn't get here !!! */
		    fatal("unknown protstream returned by prot_select in cmd_idle",
			  EC_SOFTWARE);
		}
	    }
	}
    }

    /* Get continuation data */
    c = getword(proxyd_in, &arg);

  done:
    protgroup_free(protin);
    protgroup_free(protout);

    if (done && backend_current && CAPA(backend_current, CAPA_IDLE)) {
	/* Either the client timed out, or gave us a continuation,
	   or we found a shutdown file.  In any case we're done,
	   so terminate IDLE on backend */
	prot_printf(backend_current->out, "DONE\r\n");
	pipe_until_tag(backend_current, tag, 0);
    }

    if (shutdown) {
	char *p;

	for (p = shut; *p == '['; p++); /* can't have [ be first char */
	prot_printf(proxyd_out, "* BYE [ALERT] %s\r\n", p);
	shut_down(0);
    }

    if (c != EOF) {
	if (!strcasecmp(arg.s, "Done") &&
	    (c = (c == '\r') ? prot_getc(proxyd_in) : c) == '\n') {
	    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
			error_message(IMAP_OK_COMPLETED));
	}
	else {
	    prot_printf(proxyd_out, 
			"%s BAD Invalid Idle continuation\r\n", tag);
	    eatline(proxyd_in, c);
	}
    }
}

/*
 * Perform a CAPABILITY command
 */
void cmd_capability(char *tag)
{
    const char *sasllist; /* the list of SASL mechanisms */
    unsigned mechcount;

    if (backend_current) {
	char mytag[128];
	
	proxyd_gentag(mytag, sizeof(mytag));
	/* do i want to do a NOOP for every operation? */
	prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	pipe_until_tag(backend_current, mytag, 0);
    }
    prot_printf(proxyd_out, "* CAPABILITY ");
    prot_printf(proxyd_out, CAPABILITY_STRING);

    if (config_getint(IMAPOPT_IMAPIDLEPOLL) > 0) {
	prot_printf(proxyd_out, " IDLE");
    }

    if (tls_enabled() && !proxyd_starttls_done && !proxyd_authstate) {
	prot_printf(proxyd_out, " STARTTLS");
    }
    if (proxyd_authstate ||
	(!proxyd_starttls_done && !config_getswitch(IMAPOPT_ALLOWPLAINTEXT))) {
	prot_printf(proxyd_out, " LOGINDISABLED");	
    }

    if (!proxyd_authstate &&
	sasl_listmech(proxyd_saslconn, NULL, 
		      "AUTH=", " AUTH=", " SASL-IR",
		      &sasllist,
		      NULL, &mechcount) == SASL_OK && mechcount > 0) {
	prot_printf(proxyd_out, " %s", sasllist);      
    } else {
	/* else don't show anything */
    }

#ifdef ENABLE_X_NETSCAPE_HACK
    prot_printf(proxyd_out, " X-NETSCAPE");
#endif

    prot_printf(proxyd_out, "\r\n");

    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse and perform an APPEND command.
 * The command has been parsed up to and including
 * the mailbox name.
 */
void cmd_append(char *tag, char *name)
{
    int r;
    char mailboxname[MAX_MAILBOX_PATH + 1];
    char *newserver;
    struct backend *s = NULL;

    /* we want to pipeline this whole command through to the server that
       has name on it, and then do a noop on our current server */
    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) {
	r = mlookup(mailboxname, &newserver, NULL, NULL);
    }
    if (!r && supports_referrals) { 
	proxyd_refer(tag, newserver, name);
	/* Eat the argument */
	eatline(proxyd_in, prot_getc(proxyd_in));
	return;
    }
    if (!r) {
	s = proxyd_findserver(newserver);
	if (!s) r = IMAP_SERVER_UNAVAILABLE;
    }
    if (!r) {
	prot_printf(s->out, "%s Append {%d+}\r\n%s ", tag, strlen(name), name);
	if (!pipe_command(s, 16384)) {
	    pipe_until_tag(s, tag, 0);
	}
    } else {
	eatline(proxyd_in, prot_getc(proxyd_in));
    }

    if (backend_current && backend_current != s) {
	char mytag[128];

	proxyd_gentag(mytag, sizeof(mytag));
	
	prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	pipe_until_tag(backend_current, mytag, 0);
    }

    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	/* we're allowed to reference last_result since the noop, if
	   sent, went to a different server */
	prot_printf(proxyd_out, "%s %s", tag, s->last_result);
    }
}

/*
 * Perform a SELECT/EXAMINE/BBOARD command
 */
void cmd_select(char *tag, char *cmd, char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r = 0;
    char *newserver;
    struct backend *backend_next = NULL;

    if (cmd[0] == 'B') {
	/* BBoard namespace is empty */
	r = IMAP_MAILBOX_NONEXISTENT;
    }
    else {
	r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						    proxyd_userid, mailboxname);
    }

    if (!r) r = mlookup(mailboxname, &newserver, NULL, NULL);
    if (!r && supports_referrals) { 
	proxyd_refer(tag, newserver, name);
	return;
    }

    if (!r) {
	backend_next = proxyd_findserver(newserver);
	if (!backend_next) r = IMAP_SERVER_UNAVAILABLE;
    }

    if (backend_current && backend_current != backend_next) {
	char mytag[128];

	/* switching servers; flush old server output */
	proxyd_gentag(mytag, sizeof(mytag));
	prot_printf(backend_current->out, "%s Unselect\r\n", mytag);
	/* do not fatal() here, because we don't really care about this
	 * server anymore anyway */
	pipe_until_tag(backend_current, mytag, 1);
    }
    backend_current = backend_next;

    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }

    prot_printf(backend_current->out, "%s %s {%d+}\r\n%s\r\n", tag, cmd, 
		strlen(name), name);
    switch (pipe_including_tag(backend_current, tag, 0)) {
    case PROXY_OK:
	proc_register("proxyd", proxyd_clienthost, proxyd_userid, mailboxname);
	syslog(LOG_DEBUG, "open: user %s opened %s on %s", proxyd_userid, name,
	       newserver);
	break;
    default:
	syslog(LOG_DEBUG, "open: user %s failed to open %s", proxyd_userid,
	       name);
	/* not successfully selected */
	backend_current = NULL;
	break;
    }
}
	  
/*
 * Perform a CLOSE command
 */
void cmd_close(char *tag)
{
    assert(backend_current != NULL);
    
    prot_printf(backend_current->out, "%s Close\r\n", tag);
    /* xxx do we want this to say OK if the connection is gone?
     * saying NO is clearly wrong, hense the fatal request. */
    pipe_including_tag(backend_current, tag, 0);
    backend_current = NULL;
}

/*
 * Perform an UNSELECT command -- for some support of IMAP proxy.
 * Just like close except no expunge.
 */
void cmd_unselect(char *tag)
{
    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s Unselect\r\n", tag);
    /* xxx do we want this to say OK if the connection is gone?
     * saying NO is clearly wrong, hense the fatal request. */
    pipe_including_tag(backend_current, tag, 0);
    backend_current = NULL;
}

/*
 * Parse and perform a FETCH/UID FETCH command
 * The command has been parsed up to and including
 * the sequence
 */
void cmd_fetch(char *tag, char *sequence, int usinguid)
{
    char *cmd = usinguid ? "UID Fetch" : "Fetch";

    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s %s %s ", tag, cmd, sequence);
    if (!pipe_command(backend_current, 65536)) {
	pipe_including_tag(backend_current, tag, 0);
    }
}

/*
 * Perform a PARTIAL command
 */
void cmd_partial(char *tag, char *msgno, char *data, char *start, char *count)
{
    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s Partial %s %s %s %s\r\n",
		tag, msgno, data, start, count);
    pipe_including_tag(backend_current, tag, 0);
}

/*
 * Parse and perform a STORE/UID STORE command
 * The command has been parsed up to and including
 * the FLAGS/+FLAGS/-FLAGS
 */
void cmd_store(char *tag, char *sequence, char *operation, int usinguid)
{
    const char *cmd = usinguid ? "UID Store" : "Store";

    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s %s %s %s ",
		tag, cmd, sequence, operation);
    if (!pipe_command(backend_current, 65536)) {
	pipe_including_tag(backend_current, tag, 0);
    }
}

void cmd_search(char *tag, int usinguid)
{
    const char *cmd = usinguid ? "UID Search" : "Search";

    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s %s ", tag, cmd);
    if (!pipe_command(backend_current, 65536)) {
	pipe_including_tag(backend_current, tag, 0);
    }
}

void cmd_sort(char *tag, int usinguid)
{
    char *cmd = usinguid ? "UID Sort" : "Sort";

    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s %s ", tag, cmd);
    if (!pipe_command(backend_current, 65536)) {
	pipe_including_tag(backend_current, tag, 0);
    }
}

void cmd_thread(char *tag, int usinguid)
{
    char *cmd = usinguid ? "UID Thread" : "Thread";

    assert(backend_current != NULL);

    prot_printf(backend_current->out, "%s %s ", tag, cmd);
    if (!pipe_command(backend_current, 65536)) {
	pipe_including_tag(backend_current, tag, 0);
    }
}

static int chomp(struct protstream *p, char *s)
{
    int c = prot_getc(p);

    while (*s) {
	if (tolower(c) != tolower(*s)) { break; }
	s++;
	c = prot_getc(p);
    }
    if (*s) {
	if (c != EOF) prot_ungetc(c, p);
	c = EOF;
    }
    return c;
}

/* read characters from 'p' until 'end' is seen */
static char *grab(struct protstream *p, char end)
{
    int alloc = BUFGROWSIZE, cur = 0;
    int c = -1;
    char *ret = (char *) xmalloc(alloc);

    ret[0] = '\0';
    while ((c = prot_getc(p)) != end) {
	if (c == EOF) break;
	if (cur == alloc - 1) {
	    alloc += BUFGROWSIZE;
	    ret = xrealloc(ret, alloc);

	}
	ret[cur++] = c;
    }
    if (cur) ret[cur] = '\0';

    return ret;
}

/* remove \Recent from the flags */
static char *editflags(char *flags)
{
    char *p;

    p = flags;
    while ((p = strchr(p, '\\')) != NULL) {
	if (!strncasecmp(p + 1, "recent", 6)) {
	    if (p[7] == ' ') {
		/* shift everything over so that \recent vanishes */
		char *q;
		
		q = p + 8;
		while (*q) {
		    *p++ = *q++;
		}
		*p = '\0';
	    } else if (p[7] == '\0') {
		/* last flag in line */
		*p = '\0';
	    } else {
		/* not really \recent, i guess */
		p++;
	    }
	} else {
	    p++;
	}
    }

    return flags;
}

/*
 * Perform a COPY/UID COPY command
 */    
void cmd_copy(char *tag, char *sequence, char *name, int usinguid)
{
    char *server;
    char *cmd = usinguid ? "UID Copy" : "Copy";
    struct backend *s = NULL;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r;

    assert(backend_current != NULL);

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);
    if (!r) r = mlookup(mailboxname, &server, NULL, NULL);
    if (!r) s = proxyd_findserver(server);

    if (!s) {
	/* no such mailbox or other problem */
	r = mboxlist_createmailboxcheck(mailboxname, 0, 0, proxyd_userisadmin, 
					proxyd_userid, proxyd_authstate,
					NULL, NULL);
	if(!r && server) {
	    char *c;
	    c = strchr(server, '!');
	    if(c) *c = '\0';
	}
	prot_printf(proxyd_out, "%s NO %s%s\r\n", tag,
		    r == 0 ? "[TRYCREATE] " : "", error_message(r));
    } else if (s == backend_current) {
	/* this is the easy case */
	prot_printf(backend_current->out, "%s %s %s {%d+}\r\n%s\r\n",
		    tag, cmd, sequence, strlen(name), name);
	pipe_including_tag(backend_current, tag, 0);
    } else {
	char mytag[128];
	struct d {
	    char *idate;
	    char *flags;
	    unsigned int seqno, uid;
	    struct d *next;
	} *head, *p, *q;
	int c;

	/* this is the hard case; we have to fetch the messages and append
	   them to the other mailbox */

	/* find out what the flags & internaldate for this message are */
	proxyd_gentag(mytag, sizeof(mytag));
	prot_printf(backend_current->out, 
		    "%s %s %s (Flags Internaldate)\r\n", 
		    tag, usinguid ? "Uid Fetch" : "Fetch", sequence);
	head = (struct d *) xmalloc(sizeof(struct d));
	head->flags = NULL; head->idate = NULL;
	head->seqno = head->uid = 0;
	head->next = NULL;
	p = head;
	/* read all the responses into the linked list */
	for (/* each FETCH response */;;) {
	    unsigned int seqno = 0, uidno = 0;
	    char *flags = NULL, *idate = NULL;

	    /* read a line */
	    c = prot_getc(backend_current->in);
	    if (c != '*') break;
	    c = prot_getc(backend_current->in);
	    if (c != ' ') { /* protocol error */ c = EOF; break; }
	    
	    /* read seqno */
	    seqno = 0;
	    while (isdigit(c = prot_getc(backend_current->in))) {
		seqno *= 10;
		seqno += c - '0';
	    }
	    if (seqno == 0 || c != ' ') {
		/* we suck and won't handle this case */
		c = EOF; break;
	    }
	    c = chomp(backend_current->in, "fetch (");
	    if (c == EOF) {
		c = chomp(backend_current->in, "exists\r");
		if (c == '\n') { /* got EXISTS response */
		    prot_printf(proxyd_out, "* %d EXISTS\r\n", seqno);
		    continue;
		}
	    }
	    if (c == EOF) {
		c = chomp(backend_current->in, "recent\r");
		if (c == '\n') { /* got RECENT response */
		    prot_printf(proxyd_out, "* %d RECENT\r\n", seqno);
		    continue;
		}
	    }
	    /* huh, don't get this response */
	    if (c == EOF) break;
	    for (/* each fetch item */;;) {
		/* looking at the first character in an item */
		switch (c) {
		case 'f': case 'F': /* flags? */
		    c = chomp(backend_current->in, "lags");
		    if (c != ' ') { c = EOF; }
		    else c = prot_getc(backend_current->in);
		    if (c != '(') { c = EOF; }
		    else {
			flags = grab(backend_current->in, ')');
			c = prot_getc(backend_current->in);
		    }
		    break;
		case 'i': case 'I': /* internaldate? */
		    c = chomp(backend_current->in, "nternaldate");
		    if (c != ' ') { c = EOF; }
		    else c = prot_getc(backend_current->in);
		    if (c != '"') { c = EOF; }
		    else {
			idate = grab(backend_current->in, '"');
			c = prot_getc(backend_current->in);
		    }
		    break;
		case 'u': case 'U': /* uid */
		    c = chomp(backend_current->in, "id");
		    if (c != ' ') { c = EOF; }
		    else {
			uidno = 0;
			while (isdigit(c = prot_getc(backend_current->in))) {
			    uidno *= 10;
			    uidno += c - '0';
			}
		    }
		    break;
		default: /* hmm, don't like the smell of it */
		    c = EOF;
		    break;
		}
		/* looking at either SP seperating items or a RPAREN */
		if (c == ' ') { c = prot_getc(backend_current->in); }
		else if (c == ')') break;
		else { c = EOF; break; }
	    }
	    /* if c == EOF we have either a protocol error or a situation
	       we can't handle, and we should die. */
	    if (c == ')') c = prot_getc(backend_current->in);
	    if (c == '\r') c = prot_getc(backend_current->in);
	    if (c != '\n') { c = EOF; break; }

	    /* if we're missing something, we should echo */
	    if (!flags || !idate) {
		char sep = '(';
		prot_printf(proxyd_out, "* %d FETCH ", seqno);
		if (uidno) {
		    prot_printf(proxyd_out, "%cUID %d", sep, uidno);
		    sep = ' ';
		}
		if (flags) {
		    prot_printf(proxyd_out, "%cFLAGS %s", sep, flags);
		    sep = ' ';
		}
		if (idate) {
		    prot_printf(proxyd_out, "%cINTERNALDATE %s", sep, flags);
		    sep = ' ';
		}
		prot_printf(proxyd_out, ")\r\n");
		continue;
	    }

	    /* add to p->next */
	    p->next = xmalloc(sizeof(struct d));
	    p = p->next;
	    p->idate = idate;
	    p->flags = editflags(flags);
	    p->uid = uidno;
	    p->seqno = seqno;
	    p->next = NULL;
	}
	if (c != EOF) {
	    prot_ungetc(c, backend_current->in);

	    /* we should be looking at the tag now */
	    pipe_until_tag(backend_current, tag, 0);
	}
	if (c == EOF) {
	    /* uh oh, we're not happy */
	    fatal("Lost connection to selected backend", EC_UNAVAILABLE);
	}

	/* start the append */
	prot_printf(s->out, "%s Append {%d+}\r\n%s", tag, strlen(name), name);
	prot_printf(backend_current->out, "%s %s %s (Rfc822.peek)\r\n",
		    mytag, usinguid ? "Uid Fetch" : "Fetch", sequence);
	for (/* each FETCH response */;;) {
	    unsigned int seqno = 0, uidno = 0;

	    /* read a line */
	    c = prot_getc(backend_current->in);
	    if (c != '*') break;
	    c = prot_getc(backend_current->in);
	    if (c != ' ') { /* protocol error */ c = EOF; break; }
	    
	    /* read seqno */
	    seqno = 0;
	    while (isdigit(c = prot_getc(backend_current->in))) {
		seqno *= 10;
		seqno += c - '0';
	    }
	    if (seqno == 0 || c != ' ') {
		/* we suck and won't handle this case */
		c = EOF; break;
	    }
	    c = chomp(backend_current->in, "fetch (");
	    if (c == EOF) { /* not a fetch response */
		c = chomp(backend_current->in, "exists\r");
		if (c == '\n') { /* got EXISTS response */
		    prot_printf(proxyd_out, "* %d EXISTS\r\n", seqno);
		    continue;
		}
	    }
	    if (c == EOF) { /* not an exists response */
		c = chomp(backend_current->in, "recent\r");
		if (c == '\n') { /* got RECENT response */
		    prot_printf(proxyd_out, "* %d RECENT\r\n", seqno);
		    continue;
		}
	    }
	    if (c == EOF) {
                /* huh, don't get this response */
                break;
            }
	    /* find seqno in the list */
	    p = head;
	    while (p->next && seqno != p->next->seqno) p = p->next;
	    if (!p->next) break;
	    q = p->next;
	    p->next = q->next;
	    for (/* each fetch item */;;) {
		int sz = 0;

		switch (c) {
		case 'u': case 'U':
		    c = chomp(backend_current->in, "id");
		    if (c != ' ') { c = EOF; }
		    else {
			uidno = 0;
			while (isdigit(c = prot_getc(backend_current->in))) {
			    uidno *= 10;
			    uidno += c - '0';
			}
		    }
		    break;

		case 'r': case 'R':
		    c = chomp(backend_current->in, "fc822");
		    if (c == ' ') c = prot_getc(backend_current->in);
		    if (c != '{') c = EOF;
		    else {
			sz = 0;
			while (isdigit(c = prot_getc(backend_current->in))) {
			    sz *= 10;
			    sz += c - '0';
                            /* xxx overflow */
			}
		    }
		    if (c == '}') c = prot_getc(backend_current->in);
		    if (c == '\r') c = prot_getc(backend_current->in);
		    if (c != '\n') c = EOF;

		    if (c != EOF) {
			/* append p to s->out */
			prot_printf(s->out, " (%s) \"%s\" {%d+}\r\n", 
				    q->flags, q->idate, sz);
			while (sz) {
			    char buf[2048];
			    int j = (sz > sizeof(buf) ? sizeof(buf) : sz);

			    j = prot_read(backend_current->in, buf, j);
			    if(!j) break;
			    prot_write(s->out, buf, j);
			    sz -= j;
			}
			c = prot_getc(backend_current->in);
		    }

		    break; /* end of case */
		default:
		    c = EOF;
		    break;
		}
		/* looking at either SP seperating items or a RPAREN */
		if (c == ' ') { c = prot_getc(backend_current->in); }
		else if (c == ')') break;
		else { c = EOF; break; }
	    }

	    /* if c == EOF we have either a protocol error or a situation
	       we can't handle, and we should die. */
	    if (c == ')') c = prot_getc(backend_current->in);
	    if (c == '\r') c = prot_getc(backend_current->in);
	    if (c != '\n') { c = EOF; break; }

	    /* free q */
	    free(q->idate);
	    free(q->flags);
	    free(q);
	}
	if (c != EOF) {
	    char *appenduid, *b;
	    int res;

	    /* pushback the first character of the tag we're looking at */
	    prot_ungetc(c, backend_current->in);

	    /* nothing should be left in the linked list */
	    assert(head->next == NULL);

	    /* ok, finish the append; we need the UIDVALIDITY and UIDs
	       to return as part of our COPYUID response code */
	    prot_printf(s->out, "\r\n");

	    /* should be looking at 'mytag' on 'backend_current', 
	       'tag' on 's' */
	    pipe_until_tag(backend_current, mytag, 0);
	    res = pipe_until_tag(s, tag, 0);

	    if (res == PROXY_OK) {
		appenduid = strchr(s->last_result, '[');
		/* skip over APPENDUID */
		appenduid += strlen("[appenduid ");
		b = strchr(appenduid, ']');
		*b = '\0';
		prot_printf(proxyd_out, "%s OK [COPYUID %s] %s\r\n", tag,
			    appenduid, error_message(IMAP_OK_COMPLETED));
	    } else {
		prot_printf(proxyd_out, "%s %s", tag, s->last_result);
	    }
	} else {
	    /* abort the append */
	    prot_printf(s->out, " {0}\r\n");
	    pipe_until_tag(backend_current, mytag, 0);
	    pipe_until_tag(s, tag, 0);
	    
	    /* report failure */
	    prot_printf(proxyd_out, "%s NO inter-server COPY failed\r\n", tag);
	}

	/* free dynamic memory */
	while (head) {
	    p = head;
	    head = head->next;
	    if (p->idate) free(p->idate);
	    if (p->flags) free(p->flags);
	    free(p);
	}
    }
}    

/*
 * Perform an EXPUNGE command
 * sequence == NULL if this isn't a UID EXPUNGE
 */
void cmd_expunge(char *tag, char *sequence)
{
    assert(backend_current != NULL);

    if (sequence) {
	prot_printf(backend_current->out, "%s UID Expunge %s\r\n", tag,
		    sequence);
    } else {
	prot_printf(backend_current->out, "%s Expunge\r\n", tag);
    }
    pipe_including_tag(backend_current, tag, 0);
}    

/*
 * Perform a CREATE command
 */
void cmd_create(char *tag, char *name, char *server)
{
    struct backend *s = NULL;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r = 0, res;
    char *acl = NULL;

    if (server && !proxyd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (name[0] && name[strlen(name)-1] == proxyd_namespace.hier_sep) {
	/* We don't care about trailing hierarchy delimiters. */
	name[strlen(name)-1] = '\0';
    }

    if (!r)
	r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						    proxyd_userid, mailboxname);

    if (!r && !server) {
	r = mboxlist_createmailboxcheck(mailboxname, 0, 0, proxyd_userisadmin,
					proxyd_userid, proxyd_authstate,
					&acl, &server);
	if(!r && server) {
	    char *c;
	    c = strchr(server, '!');
	    if(c) *c = '\0';
	}
    }
    if (!r && server) {
	s = proxyd_findserver(server);
	if (!s) r = IMAP_SERVER_UNAVAILABLE;
    }
    if (!r) {
	if (!CAPA(s, CAPA_MUPDATE)) {
	    /* reserve mailbox on MUPDATE */
	}
    }

    if (!r) {
	/* ok, send the create to that server */
	prot_printf(s->out, "%s CREATE {%d+}\r\n%s\r\n", 
		    tag, strlen(name), name);
	res = pipe_including_tag(s, tag, 0);
	tag = "*";		/* can't send another tagged response */
	
	if (!CAPA(s, CAPA_MUPDATE)) {
	    /* do MUPDATE create operations */
	}
	/* make sure we've seen the update */
	if (ultraparanoid && res == PROXY_OK) kick_mupdate();
    }
    
    if (r) prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
}

/*
 * Perform a DELETE command
 */
void cmd_delete(char *tag, char *name)
{
    int r, res;
    char *server;
    struct backend *s = NULL;
    char mailboxname[MAX_MAILBOX_NAME+1];

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) r = mlookup(mailboxname, &server, NULL, NULL);
    if (!r && supports_referrals) { 
	proxyd_refer(tag, server, name);
	referral_kick = 1;
	return;
    }

    if (!r) {
	s = proxyd_findserver(server);
	if (!s) r = IMAP_SERVER_UNAVAILABLE;
    }

    if (!r) {
	prot_printf(s->out, "%s DELETE {%d+}\r\n%s\r\n", 
		    tag, strlen(name), name);
	res = pipe_including_tag(s, tag, 0);
	tag = "*";		/* can't send another tagged response */

	if (!CAPA(s, CAPA_MUPDATE) && res == PROXY_OK) {
	    /* do MUPDATE delete operations */
	}

	/* make sure we've seen the update */
	if (ultraparanoid && res == PROXY_OK) kick_mupdate();
    }

    if (r) prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
}	

/*
 * Perform a RECONSTRUCT command
 */
void cmd_reconstruct(char *tag, char *name)
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *server = NULL;

    if(!proxyd_userisadmin) r = IMAP_PERMISSION_DENIED;
    else {
	r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace,
						    name,
						    proxyd_userid,
						    mailboxname);
    }

    if(!r)
	r = mlookup(mailboxname, &server, NULL, NULL);

    if(!r) {
	proxyd_refer(tag, server, name);
    } else {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    }
}	

/*
 * Perform a RENAME command
 */
void cmd_rename(char *tag, char *oldname, char *newname, char *partition)
{
    int r = 0, res;
    char *server;
    char oldmailboxname[MAX_MAILBOX_NAME+1];
    char newmailboxname[MAX_MAILBOX_NAME+1];
    struct backend *s = NULL;
    char *acl = NULL;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, oldname,
						proxyd_userid, oldmailboxname);
    if (!r) (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, newname,
						    proxyd_userid, newmailboxname);
    if (!r) r = mlookup(oldmailboxname, &server, &acl, NULL);
    if (!r) {
	s = proxyd_findserver(server);
	if (!s) r = IMAP_SERVER_UNAVAILABLE;
    }

    /* Cross Server Rename */
    if (!r && partition) {
	char *destpart;
	
	if(strcmp(oldname, newname)) {
	    prot_printf(proxyd_out,
			"%s NO Cross-server or cross-partition move w/rename not supported\r\n",
			tag);
	    return;
	}

	/* dest partition? */

	destpart = strchr(partition,'!');
	if(destpart) {
	    char newserver[MAX_MAILBOX_NAME+1];	    
	    if(strlen(partition)>=sizeof(newserver)) {
		prot_printf(proxyd_out,
			    "%s NO Partition name too long\r\n", tag);
		return;
	    }
	    strcpy(newserver,partition);
	    newserver[destpart-partition]='\0';
	    destpart++;

	    if(!strcmp(server, newserver)) {
		/* Same Server, different partition */
		/* xxx this would require administrative access to the
		 * backend, which we won't get */
		prot_printf(proxyd_out,
			    "%s NO Can't move across partitions via a proxy\r\n",
			    tag);
		return;
	    } else {
		/* Cross Server */
		/* <tag> XFER <name> <dest server> <dest partition> */
		prot_printf(s->out,
			    "%s XFER {%d+}\r\n%s {%d+}\r\n%s {%d+}\r\n%s\r\n", 
			    tag, strlen(oldname), oldname,
			    strlen(newserver), newserver,
			    strlen(destpart), destpart);
	    }
	    
	} else {
	    /* <tag> XFER <name> <dest server> */
	    prot_printf(s->out, "%s XFER {%d+}\r\n%s {%d+}\r\n%s\r\n", 
			tag, strlen(oldname), oldname,
			strlen(partition), partition);
	}
	
	res = pipe_including_tag(s, tag, 0);

	/* make sure we've seen the update */
	if (ultraparanoid && res == PROXY_OK) kick_mupdate();

	return;
    }

    if (!r) {
	if (!CAPA(s, CAPA_MUPDATE)) {
	    /* do MUPDATE create operations for new mailbox */
	}

	prot_printf(s->out, "%s RENAME {%d+}\r\n%s {%d+}\r\n%s\r\n", 
		    tag, strlen(oldname), oldname,
		    strlen(newname), newname);
	res = pipe_including_tag(s, tag, 0);
	tag = "*";		/* can't send another tagged response */
	
	if (!CAPA(s, CAPA_MUPDATE)) {
	    /* Activate/abort new mailbox in MUPDATE*/
	    /* delete old mailbox from MUPDATE */
	}

	/* make sure we've seen the update */
	if (res == PROXY_OK) kick_mupdate();
    }

    if (r) prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
}

/*
 * Perform a FIND command
 */
void cmd_find(char *tag, char *namespace, char *pattern)
{
    char *p;
    lcase(namespace);

    for (p = pattern; *p; p++) {
	if (*p == '%') *p = '?';
    }

    if (!strcasecmp(namespace, "mailboxes")) {
	if (!backend_inbox) {
	    backend_inbox = proxyd_findinboxserver();
	}

	if (backend_inbox) {
	    prot_printf(backend_inbox->out, 
			"%s Lsub \"\" {%d+}\r\n%s\r\n",
			tag, strlen(pattern), pattern);
	    pipe_lsub(backend_inbox, tag, 0, "MAILBOX");
	} else {		/* user doesn't have an INBOX */
	    /* noop */
	}
    } else if (!strcasecmp(namespace, "all.mailboxes")) {
	/* Translate any separators in pattern */
	mboxname_hiersep_tointernal(&proxyd_namespace, pattern,
				    config_virtdomains ?
				    strcspn(pattern, "@") : 0);

	(*proxyd_namespace.mboxlist_findall)(&proxyd_namespace, pattern,
					     proxyd_userisadmin, proxyd_userid,
					     proxyd_authstate, mailboxdata,
					     NULL);
    } else if (!strcasecmp(namespace, "bboards")
	       || !strcasecmp(namespace, "all.bboards")) {
	;
    } else {
	prot_printf(proxyd_out, "%s BAD Invalid FIND subcommand\r\n", tag);
	return;
    }

    if (backend_current) {
	char mytag[128];

	proxyd_gentag(mytag, sizeof(mytag));

	prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	pipe_until_tag(backend_current, mytag, 0);
    }

    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a LIST or LSUB command
 * LISTs we do locally
 * LSUBs we farm out
 */
void cmd_list(char *tag, int listopts, char *reference, char *pattern)
{
    char *buf = NULL;
    int patlen = 0;
    int reflen = 0;
    static int ignorereference = -1;

    /* Ignore the reference argument?
       (the behavior in 1.5.10 & older) */
    if (ignorereference == -1) {
	ignorereference = config_getswitch(IMAPOPT_IGNOREREFERENCE);
    }

    /* Reset state in mstringdata */
    mstringdata(NULL, NULL, 0, 0);
    
    if (!pattern[0] && !(listopts & LIST_LSUB)) {
	/* Special case: query top-level hierarchy separator */
	prot_printf(proxyd_out, "* LIST (\\Noselect) \"%c\" \"\"\r\n",
		    proxyd_namespace.hier_sep);
    } else if (listopts & (LIST_LSUB | LIST_SUBSCRIBED)) {
	/* do an LSUB command; contact our INBOX */
	if (!backend_inbox) {
	    backend_inbox = proxyd_findinboxserver();
	}

	if (backend_inbox) {
	    prot_printf(backend_inbox->out, 
			"%s Lsub {%d+}\r\n%s {%d+}\r\n%s\r\n",
			tag, strlen(reference), reference,
			strlen(pattern), pattern);
	    pipe_lsub(backend_inbox, tag, 0, (listopts & LIST_LSUB) ? "LSUB" : "LIST");
	} else {		/* user doesn't have an INBOX */
	    /* noop */
	}
    } else {			/* do a LIST locally */
	/* Do we need to concatenate fields? */
	if (!ignorereference || pattern[0] == proxyd_namespace.hier_sep) {
	    /* Either
	     * - name begins with dot
	     * - we're configured to honor the reference argument */

	    /* Allocate a buffer, figure out how to stick the arguments
	       together, do it, then do that instead of using pattern. */
	    patlen = strlen(pattern);
	    reflen = strlen(reference);
	    
	    buf = xmalloc(patlen + reflen + 1);
	    buf[0] = '\0';

	    if (*reference) {
		/* check for LIST A. .B, change to LIST "" A.B */
		if (reference[reflen-1] == proxyd_namespace.hier_sep &&
		    pattern[0] == proxyd_namespace.hier_sep) {
		    reference[--reflen] = '\0';
		}
		strcpy(buf, reference);
	    }
	    strcat(buf, pattern);
	    pattern = buf;
	}

	/* Translate any separators in pattern */
	mboxname_hiersep_tointernal(&proxyd_namespace, pattern,
 				    config_virtdomains ?
 				    strcspn(pattern, "@") : 0);

	(*proxyd_namespace.mboxlist_findall)(&proxyd_namespace, pattern,
					     proxyd_userisadmin, proxyd_userid,
					     proxyd_authstate, listdata, NULL);
	listdata((char *)0, 0, 0, 0);

	if (buf) free(buf);
    }

    if (backend_current && (backend_current != backend_inbox ||
			    !(listopts & (LIST_LSUB | LIST_SUBSCRIBED)))) {
	/* our Lsub would've done this if 
	   backend_current == backend_inbox */
	char mytag[128];

	proxyd_gentag(mytag, sizeof(mytag));

	prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	pipe_until_tag(backend_current, mytag, 0);
    }

    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}
  
/*
 * Perform a SUBSCRIBE (add is nonzero) or
 * UNSUBSCRIBE (add is zero) command
 */
void cmd_changesub(char *tag, char *namespace, char *name, int add)
{
    char *cmd = add ? "Subscribe" : "Unsubscribe";
    int r = 0;

    if (!backend_inbox) {
	backend_inbox = proxyd_findinboxserver();
    }

    if (backend_inbox) {
	char mailboxname[MAX_MAILBOX_NAME+1];

	if (add) {
	    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace,
							name, proxyd_userid,
							mailboxname);
	    if(!r) r = mlookup(mailboxname, NULL, NULL, NULL);

	    /* Doesn't exist on murder */
	    if(r) goto done;
	}
	
	if (namespace) {
	    prot_printf(backend_inbox->out, 
			"%s %s {%d+}\r\n%s {%d+}\r\n%s\r\n", 
			tag, cmd, 
			strlen(namespace), namespace,
			strlen(name), name);
	} else {
	    prot_printf(backend_inbox->out, "%s %s {%d+}\r\n%s\r\n", 
			tag, cmd, 
			strlen(name), name);
	}
	pipe_including_tag(backend_inbox, tag, 0);
    } else {
	r = IMAP_SERVER_UNAVAILABLE;
    }

 done:
    if(r) {
	prot_printf(proxyd_out, "%s NO %s: %s\r\n", tag,
		    add ? "Subscribe" : "Unsubscribe", error_message(r));
    }
}

/*
 * Perform a GETACL command
 */
void cmd_getacl(const char *tag, const char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, access;
    char *acl;
    char *rights, *nextid;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) r = mlookup(mailboxname, NULL, &acl, NULL);

    if (!r) {
	access = cyrus_acl_myrights(proxyd_authstate, acl);

	if (!(access & (ACL_READ|ACL_ADMIN)) &&
	    !proxyd_userisadmin &&
	    !mboxname_userownsmailbox(proxyd_userid, mailboxname)) {
	    r = (access & ACL_LOOKUP) ?
	      IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
	}
    }
    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(proxyd_out, "* ACL ");
    printastring(name);
    
    while (acl) {
	rights = strchr(acl, '\t');
	if (!rights) break;
	*rights++ = '\0';
	
	nextid = strchr(rights, '\t');
	if (!nextid) break;
	*nextid++ = '\0';
	
	prot_printf(proxyd_out, " ");
	printastring(acl);
	prot_printf(proxyd_out, " ");
	printastring(rights);
	acl = nextid;
    }
    prot_printf(proxyd_out, "\r\n");
    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a LISTRIGHTS command
 */
void cmd_listrights(char *tag, char *name, char *identifier)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, rights;
    char *acl;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) {
	r = mlookup(mailboxname, (char **)0, &acl, NULL);
    }

    if (!r) {
	rights = cyrus_acl_myrights(proxyd_authstate, acl);

	if (!rights && !proxyd_userisadmin &&
	    !mboxname_userownsmailbox(proxyd_userid, mailboxname)) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	}
    }

    if (!r) {
	struct auth_state *authstate = auth_newstate(identifier);
	char *canon_identifier;
	int canonidlen = 0;
	int implicit;
	char rightsdesc[100], optional[33];

	if (global_authisa(authstate, IMAPOPT_ADMINS))
	    canon_identifier = identifier; /* don't canonify global admins */
	else
	    canon_identifier = canonify_userid(identifier, proxyd_userid, NULL);
	auth_freestate(authstate);

	if (canon_identifier) canonidlen = strlen(canon_identifier);

	if (!canon_identifier) {
	    implicit = 0;
	}
	else if (mboxname_userownsmailbox(canon_identifier, mailboxname)) {
	    /* identifier's personal mailbox */
	    implicit = config_implicitrights;
	}
	else if (mboxname_isusermailbox(mailboxname, 1)) {
	    /* anyone can post to an INBOX */
	    implicit = ACL_POST;
	}
	else {
	    implicit = 0;
	}

	/* calculate optional rights */
	cyrus_acl_masktostr(implicit ^ (canon_identifier ? ACL_FULL : 0),
			    optional);

	/* build the rights string */
	if (implicit) {
	    cyrus_acl_masktostr(implicit, rightsdesc);
	}
	else {
	    strcpy(rightsdesc, "\"\"");
	}

	if (*optional) {
	    int i, n = strlen(optional);
	    char *p = rightsdesc + strlen(rightsdesc);

	    for (i = 0; i < n; i++) {
		*p++ = ' ';
		*p++ = optional[i];
	    }
	    *p = '\0';
	}

	prot_printf(proxyd_out, "* LISTRIGHTS ");
	printastring(name);
	prot_putc(' ', proxyd_out);
	printastring(identifier);
	prot_printf(proxyd_out, " %s", rightsdesc);

	prot_printf(proxyd_out, "\r\n%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
	return;
    }

    prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
}

/*
 * Perform a MYRIGHTS command
 */
void cmd_myrights(const char *tag, const char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, rights = 0;
    char *acl;
    char str[ACL_MAXSTR];

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) {
	r = mlookup(mailboxname, (char **)0, &acl, NULL);
    }

    if (!r) {
	rights = cyrus_acl_myrights(proxyd_authstate, acl);

	/* Add in implicit rights */
	if (proxyd_userisadmin) {
	    rights |= ACL_LOOKUP|ACL_ADMIN;
	}
	else if (mboxname_userownsmailbox(proxyd_userid, mailboxname)) {
	    rights |= config_implicitrights;
	}

	if (!rights) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	}
    }
    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(proxyd_out, "* MYRIGHTS ");
    printastring(name);
    prot_printf(proxyd_out, " ");
    printastring(cyrus_acl_masktostr(rights, str));
    prot_printf(proxyd_out, "\r\n%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a SETACL command
 */
void cmd_setacl(char *tag, const char *name,
		const char *identifier, const char *rights)
{
    int r, res;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *server;
    struct backend *s = NULL;
    char *acl = NULL;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);
    if (!r) r = mlookup(mailboxname, &server, &acl, NULL);
    if (!r) {
	s = proxyd_findserver(server);
	if (!s) r = IMAP_SERVER_UNAVAILABLE;
    }

    if (!r && proxyd_userisadmin && supports_referrals) {
	/* They aren't an admin remotely, so let's refer them */
	proxyd_refer(tag, server, name);
	referral_kick = 1;
	return;
    } else if (!r) {
	if (rights) {
	    prot_printf(s->out, 
			"%s Setacl {%d+}\r\n%s {%d+}\r\n%s {%d+}\r\n%s\r\n",
			tag, strlen(name), name,
			strlen(identifier), identifier,
			strlen(rights), rights);
	} else {
	    prot_printf(s->out, 
			"%s Deleteacl {%d+}\r\n%s {%d+}\r\n%s\r\n",
			tag, strlen(name), name,
			strlen(identifier), identifier);
	}	    
	res = pipe_including_tag(s, tag, 0);
	tag = "*";		/* can't send another tagged response */
	if (!CAPA(s, CAPA_MUPDATE) && res == PROXY_OK) {
	    /* setup new ACL in MUPDATE */
	}
	/* make sure we've seen the update */
	if (ultraparanoid && res == PROXY_OK) kick_mupdate();
    }

    if (r) prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
}

/*
 * Callback for (get|set)quota, to ensure that all of the
 * submailboxes are on the same server.
 */
static int quota_cb(char *name, int matchlen __attribute__((unused)),
		    int maycreate __attribute__((unused)), void *rock) 
{
    int r;
    char *this_server;
    const char *servername = (const char *)rock;
    
    r = mlookup(name, &this_server, NULL, NULL);
    if(r) return r;

    if(strcmp(servername, this_server)) {
	/* Not on same server as the root */
	return IMAP_NOT_SINGULAR_ROOT;
    } else {
	return PROXY_OK;
    }
}

/*
 * Perform a GETQUOTA command
 */
void cmd_getquota(char *tag, char *name)
{
    int r;
    char *server_rock = NULL, *server_rock_tmp = NULL;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char quotarootbuf[MAX_MAILBOX_NAME + 3];

    if(!proxyd_userisadmin) r = IMAP_PERMISSION_DENIED;
    else {
	r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace,
						    name,
						    proxyd_userid,
						    mailboxname);
    }

    if(!r)
	r = mlookup(mailboxname, &server_rock_tmp, NULL, NULL);

    if(!r) {
	server_rock = xstrdup(server_rock_tmp);

	snprintf(quotarootbuf, sizeof(quotarootbuf), "%s.*", mailboxname);

	r = mboxlist_findall(&proxyd_namespace, quotarootbuf,
			     proxyd_userisadmin, proxyd_userid,
			     proxyd_authstate, quota_cb, server_rock);
    }

    if (!r) {
	/* Do the referral */
	proxyd_refer(tag, server_rock, name);
	free(server_rock);
    } else {
	if(server_rock) free(server_rock);
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    }
}

/*
 * Perform a GETQUOTAROOT command
 */
void cmd_getquotaroot(char *tag, char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *server;
    int r;
    struct backend *s = NULL;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);
    if (!r) r = mlookup(mailboxname, &server, NULL, NULL);

    if(proxyd_userisadmin) {
	/* If they are an admin, they won't retain that privledge if we
	 * proxy for them, so we need to refer them -- even if they haven't
	 * told us they're able to handle it. */
	proxyd_refer(tag, server, name);
    } else {
	if (!r) s = proxyd_findserver(server);

	if (s) {
	    prot_printf(s->out, "%s Getquotaroot {%d+}\r\n%s\r\n",
			tag, strlen(name), name);
	    pipe_including_tag(s, tag, 0);
	} else {
	    r = IMAP_SERVER_UNAVAILABLE;
	}

	if (r) {
	    prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
	    return;
	}
    }
}

/*
 * Parse and perform a SETQUOTA command
 * The command has been parsed up to the resource list
 */
void cmd_setquota(char *tag, char *quotaroot)
{
    int r;
    char c;
    char *p;
    static struct buf arg;
    int badresource = 0;
    char *server_rock = NULL, *server_rock_tmp = NULL;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char quotarootbuf[MAX_MAILBOX_NAME + 3];

    /* First ensure the validity of the command */
    c = prot_getc(proxyd_in);
    if (c != '(') goto badlist;

    /* xxx maybe we don't want to be this stringant on what types
     * of quota we allow to be set, since we will just be doing a referral
     * anyway... */
    c = getword(proxyd_in, &arg);
    if (c != ')' || arg.s[0] != '\0') {
	for (;;) {
	    if (c != ' ') goto badlist;
	    if (strcasecmp(arg.s, "storage") != 0) badresource = 1;
	    c = getword(proxyd_in, &arg);
	    if (c != ' ' && c != ')') goto badlist;
	    if (arg.s[0] == '\0') goto badlist;
	    /* We are just syntax checking here, no need to save the value */
	    for (p = arg.s; *p; p++) {
		if (!isdigit((int) *p)) goto badlist;
	    }
	    if (c == ')') break;
	}
    }
    c = prot_getc(proxyd_in);
    if (c == '\r') c = prot_getc(proxyd_in);
    if (c != '\n') {
	prot_printf(proxyd_out,
		    "%s BAD Unexpected extra arguments to SETQUOTA\r\n", tag);
	eatline(proxyd_in, c);
	return;
    }

    if(badresource) r = IMAP_UNSUPPORTED_QUOTA;
    else if(!proxyd_userisadmin) r = IMAP_PERMISSION_DENIED;
    else {
	r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace,
						    quotaroot,
						    proxyd_userid,
						    mailboxname);
    }

    if(!r)
	r = mlookup(mailboxname, &server_rock_tmp, NULL, NULL);

    if(!r) {
	server_rock = xstrdup(server_rock_tmp);

	snprintf(quotarootbuf, sizeof(quotarootbuf), "%s.*", mailboxname);

	r = mboxlist_findall(&proxyd_namespace, quotarootbuf,
			     proxyd_userisadmin, proxyd_userid,
			     proxyd_authstate, quota_cb, server_rock);
    }

    if (!r) {
	/* Do the referral */
	proxyd_refer(tag, server_rock, quotaroot);
	free(server_rock);
    } else {
	if(server_rock) free(server_rock);
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    }

    return;

 badlist:
    prot_printf(proxyd_out, "%s BAD Invalid quota list in Setquota\r\n", tag);
    eatline(proxyd_in, c);
}

#ifdef HAVE_SSL
/*
 * this implements the STARTTLS command, as described in RFC 2595.
 * one caveat: it assumes that no external layer is currently present.
 * if a client executes this command, information about the external
 * layer that was passed on the command line is disgarded. this should
 * be fixed.
 */
/* imaps - weather this is an imaps transaction or not */
void cmd_starttls(char *tag, int imaps)
{
    int result;
    int *layerp;
    sasl_ssf_t ssf;
    char *auth_id;
    
    /* SASL and openssl have different ideas about whether ssf is signed */
    layerp = (int *) &(ssf);

    if (proxyd_starttls_done == 1)
    {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, 
		    "TLS already active");
	return;
    }

    result=tls_init_serverengine("imap",
				 5,        /* depth to verify */
				 !imaps,   /* can client auth? */
				 !imaps);  /* TLSv1 only? */

    if (result == -1) {

	syslog(LOG_ERR, "error initializing TLS");

	if (imaps == 0)
	    prot_printf(proxyd_out, "%s NO %s\r\n", 
			tag, "Error initializing TLS");
	else
	    fatal("tls_init() failed", EC_CONFIG);

	return;
    }

    if (imaps == 0)
    {
	prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		    "Begin TLS negotiation now");
	/* must flush our buffers before starting tls */
	prot_flush(proxyd_out);
    }
  
    result=tls_start_servertls(0, /* read */
			       1, /* write */
			       layerp,
			       &auth_id,
			       &tls_conn);

    /* if error */
    if (result==-1) {
	if (imaps == 0)	{
	    prot_printf(proxyd_out, "%s NO Starttls failed\r\n", tag);
	    syslog(LOG_NOTICE, "STARTTLS failed: %s", proxyd_clienthost);
	    return;
	} else {
	    syslog(LOG_NOTICE, "imaps failed: %s", proxyd_clienthost);
	    fatal("tls_start_servertls() failed", EC_TEMPFAIL);
	    return;
	}
    }

    /* tell SASL about the negotiated layer */
    result = sasl_setprop(proxyd_saslconn, SASL_SSF_EXTERNAL, &ssf);
    if (result != SASL_OK) {
	fatal("sasl_setprop() failed: cmd_starttls()", EC_TEMPFAIL);
    }
    saslprops.ssf = ssf;

    result = sasl_setprop(proxyd_saslconn, SASL_AUTH_EXTERNAL, auth_id);
    if (result != SASL_OK) {
       fatal("sasl_setprop() failed: cmd_starttls()", EC_TEMPFAIL);
    }
    if(saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    if(auth_id)
        saslprops.authid = xstrdup(auth_id);

    /* tell the prot layer about our new layers */
    prot_settls(proxyd_in, tls_conn);
    prot_settls(proxyd_out, tls_conn);

    proxyd_starttls_done = 1;
}
#else
void cmd_starttls(char *tag, int imaps)
{
    fatal("cmd_starttls() executed, but starttls isn't implemented!",
	  EC_SOFTWARE);
}
#endif /* HAVE_SSL */

/*
 * Parse and perform a STATUS command
 * The command has been parsed up to the attribute list
 */
void cmd_status(char *tag, char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r;
    char *server;
    struct backend *s = NULL;

    r = (*proxyd_namespace.mboxname_tointernal)(&proxyd_namespace, name,
						proxyd_userid, mailboxname);

    if (!r) r = mlookup(mailboxname, &server, NULL, NULL);
    if (!r && supports_referrals
	&& config_getswitch(IMAPOPT_PROXYD_ALLOW_STATUS_REFERRAL)) { 
	proxyd_refer(tag, server, name);
	/* Eat the argument */
	eatline(proxyd_in, prot_getc(proxyd_in));
	return;
    }

    if (!r) s = proxyd_findserver(server);
    if (!r && !s) r = IMAP_SERVER_UNAVAILABLE;
    if (!r) {
	prot_printf(s->out, "%s Status {%d+}\r\n%s ", tag,
		    strlen(name), name);
	if (!pipe_command(s, 65536)) {
	    pipe_until_tag(s, tag, 0);
	}
	if (backend_current && s != backend_current) {
	    char mytag[128];
	    
	    proxyd_gentag(mytag, sizeof(mytag));

	    prot_printf(backend_current->out, "%s Noop\r\n", mytag);
	    pipe_until_tag(backend_current, mytag, 0);
	}
    } else {
	eatline(proxyd_in, prot_getc(proxyd_in));
    }

    if (!r) {
	prot_printf(proxyd_out, "%s %s", tag, s->last_result);
    } else {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    }
}

#ifdef ENABLE_X_NETSCAPE_HACK
/*
 * Reply to Netscape's crock with a crock of my own
 */
void
cmd_netscape(tag)
    char *tag;
{
    const char *url;
    /* so tempting, and yet ... */
    /* url = "http://random.yahoo.com/ryl/"; */
    url = config_getstring(IMAPOPT_NETSCAPEURL);

    /* I only know of three things to reply with: */
    prot_printf(proxyd_out,
"* OK [NETSCAPE] Carnegie Mellon Cyrus IMAP proxy\r\n* VERSION %s\r\n",
		CYRUS_VERSION);
    prot_printf(proxyd_out,
		"* ACCOUNT-URL %s\r\n%s OK %s\r\n",
		url, tag, error_message(IMAP_OK_COMPLETED));

    /* no tagged response?!? */
}
#endif /* ENABLE_X_NETSCAPE_HACK */

/* Callback for cmd_namespace to be passed to mboxlist_findall.
 * For each top-level mailbox found, print a bit of the response
 * if it is a shared namespace.  The rock is used as an integer in
 * order to ensure the namespace response is correct on a server with
 * no shared namespace.
 */
static int namespacedata(char *name, int matchlen __attribute__((unused)),
			 int maycreate __attribute__((unused)), void *rock)
{
    int* sawone = (int*) rock;

    if (!name) {
	return 0;
    }
    
    if (!(strncmp(name, "INBOX.", 6))) {
	/* The user has a "personal" namespace. */
	sawone[NAMESPACE_INBOX] = 1;
    } else if (mboxname_isusermailbox(name, 0)) {
	/* The user can see the "other users" namespace. */
	sawone[NAMESPACE_USER] = 1;
    } else {
	/* The user can see the "shared" namespace. */
	sawone[NAMESPACE_SHARED] = 1;
    }

    return 0;
}

/*
 * Print out a response to the NAMESPACE command defined by
 * RFC 2342.
 */
void cmd_namespace(tag)
    char* tag;
{
    int sawone[3] = {0, 0, 0};
    char pattern[2] = {'%','\0'};

    /* now find all the exciting toplevel namespaces -
     * we're using internal names here
     */
    mboxlist_findall(NULL, pattern, proxyd_userisadmin, proxyd_userid,
		     proxyd_authstate, namespacedata, (void*) sawone);

    prot_printf(proxyd_out, "* NAMESPACE");
    if (sawone[NAMESPACE_INBOX]) {
	prot_printf(proxyd_out, " ((\"%s\" \"%c\"))",
		    proxyd_namespace.prefix[NAMESPACE_INBOX],
		    proxyd_namespace.hier_sep);
    } else {
	prot_printf(proxyd_out, " NIL");
    }
    if (sawone[NAMESPACE_USER]) {
	prot_printf(proxyd_out, " ((\"%s\" \"%c\"))",
		    proxyd_namespace.prefix[NAMESPACE_USER],
		    proxyd_namespace.hier_sep);
    } else {
	prot_printf(proxyd_out, " NIL");
    }
    if (sawone[NAMESPACE_SHARED]) {
	prot_printf(proxyd_out, " ((\"%s\" \"%c\"))",
		    proxyd_namespace.prefix[NAMESPACE_SHARED],
		    proxyd_namespace.hier_sep);
    } else {
	prot_printf(proxyd_out, " NIL");
    }
    prot_printf(proxyd_out, "\r\n");

    prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Print 's' as a quoted-string or literal (but not an atom)
 */
void
printstring(s)
const char *s;
{
    const char *p;
    int len = 0;

    /* Look for any non-QCHAR characters */
    for (p = s; *p && len < 1024; p++) {
	len++;
	if (*p & 0x80 || *p == '\r' || *p == '\n'
	    || *p == '\"' || *p == '%' || *p == '\\') break;
    }

    /* if it's too long, literal it */
    if (*p || len >= 1024) {
	prot_printf(proxyd_out, "{%u}\r\n%s", strlen(s), s);
    } else {
	prot_printf(proxyd_out, "\"%s\"", s);
    }
}

/*
 * Print 's' as an atom, quoted-string, or literal
 */
void printastring(const char *s)
{
    const char *p;
    int len = 0;

    if (imparse_isatom(s)) {
	prot_printf(proxyd_out, "%s", s);
	return;
    }

    /* Look for any non-QCHAR characters */
    for (p = s; *p && len < 1024; p++) {
	len++;
	if (*p & 0x80 || *p == '\r' || *p == '\n'
	    || *p == '\"' || *p == '%' || *p == '\\') break;
    }

    /* if it's too long, literal it */
    if (*p || len >= 1024) {
	prot_printf(proxyd_out, "{%u}\r\n%s", strlen(s), s);
    } else {
	prot_printf(proxyd_out, "\"%s\"", s);
    }
}

/*
 * Issue a MAILBOX untagged response
 */
static int mailboxdata(char *name, 
		       int matchlen __attribute__((unused)), 
		       int maycreate __attribute__((unused)), 
		       void* rock __attribute__((unused)))
{
    char mboxname[MAX_MAILBOX_PATH+1];

    (*proxyd_namespace.mboxname_toexternal)(&proxyd_namespace, name,
					    proxyd_userid, mboxname);
    prot_printf(proxyd_out, "* MAILBOX %s\r\n", mboxname);
    return 0;
}

/*
 * Issue a LIST or LSUB untagged response
 */
static void mstringdata(cmd, name, matchlen, maycreate)
char *cmd;
char *name;
int matchlen;
int maycreate;
{
    static char lastname[MAX_MAILBOX_PATH+1];
    static int lastnamedelayed = 0;
    static int lastnamenoinferiors = 0;
    static int sawuser = 0;
    int lastnamehassub = 0;
    int c;
    char mboxname[MAX_MAILBOX_PATH+1];

    /* We have to reset the sawuser flag before each list command.
     * Handle it as a dirty hack.
     */
    if (cmd == NULL) {
	sawuser = 0;
	return;
    }
    
    if (lastnamedelayed) {
	if (name && strncmp(lastname, name, strlen(lastname)) == 0 &&
	    name[strlen(lastname)] == '.') {
	    lastnamehassub = 1;
	}
	prot_printf(proxyd_out, "* %s (%s) \"%c\" ", cmd,
		    lastnamenoinferiors ? "\\Noinferiors" :
		    lastnamehassub ? "\\HasChildren" : "\\HasNoChildren",
		    proxyd_namespace.hier_sep);
	(*proxyd_namespace.mboxname_toexternal)(&proxyd_namespace, lastname,
						proxyd_userid, mboxname);
	printstring(mboxname);
	prot_printf(proxyd_out, "\r\n");
	lastnamedelayed = lastnamenoinferiors = 0;
    }

    /* Special-case to flush any final state */
    if (!name) {
	lastname[0] = '\0';
	return;
    }

    /* Suppress any output of a partial match */
    if ((name[matchlen]
	 && strncmp(lastname, name, matchlen) == 0
	 && (lastname[matchlen] == '\0' || lastname[matchlen] == '.'))) {
	return;
    }
	
    /*
     * We can get a partial match for "user" multiple times with
     * other matches inbetween.  Handle it as a special case
     */
    if (matchlen == 4 && strncasecmp(name, "user", 4) == 0) {
	if (sawuser) return;
	sawuser = 1;
    }

    strcpy(lastname, name);
    lastname[matchlen] = '\0';

    if (!name[matchlen]) {
	lastnamedelayed = 1;
	if (!maycreate) lastnamenoinferiors = 1;
	return;
    }

    c = name[matchlen];
    if (c) name[matchlen] = '\0';
    prot_printf(proxyd_out, "* %s (%s) \"%c\" ", cmd,
		c ? "\\HasChildren \\Noselect" : "",
		proxyd_namespace.hier_sep);
    (*proxyd_namespace.mboxname_toexternal)(&proxyd_namespace, name,
					    proxyd_userid, mboxname);
    printstring(mboxname);
    prot_printf(proxyd_out, "\r\n");
    if (c) name[matchlen] = c;
    return;
}

/*
 * Issue a LIST untagged response
 */
static int listdata(char *name, int matchlen, int maycreate,
		    void *rock __attribute__((unused)))
{
    mstringdata("LIST", name, matchlen, maycreate);
    return 0;
}

/*
 * Parse annotate fetch data.
 *
 * This is a generic routine which parses just the annotation data.
 * Any surrounding command text must be parsed elsewhere, ie,
 * GETANNOTATION, FETCH.
 */

int getannotatefetchdata(char *tag,
			 struct strlist **entries, struct strlist **attribs)
{
    int c;
    static struct buf arg;

    *entries = *attribs = NULL;

    c = prot_getc(proxyd_in);
    if (c == EOF) {
	prot_printf(proxyd_out,
		    "%s BAD Missing annotation entry\r\n", tag);
	goto baddata;
    }
    else if (c == '(') {
	/* entry list */
	do {
	    c = getqstring(proxyd_in, proxyd_out, &arg);
	    if (c == EOF) {
		prot_printf(proxyd_out,
			    "%s BAD Missing annotation entry\r\n", tag);
		goto baddata;
	    }

	    /* add the entry to the list */
	    appendstrlist(entries, arg.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(proxyd_out,
			"%s BAD Missing close paren in annotation entry list \r\n",
			tag);
	    goto baddata;
	}

	c = prot_getc(proxyd_in);
    }
    else {
	/* single entry -- add it to the list */
	prot_ungetc(c, proxyd_in);
	c = getqstring(proxyd_in, proxyd_out, &arg);
	if (c == EOF) {
	    prot_printf(proxyd_out,
			"%s BAD Missing annotation entry\r\n", tag);
	    goto baddata;
	}

	appendstrlist(entries, arg.s);
    }

    if (c != ' ' || (c = prot_getc(proxyd_in)) == EOF) {
	prot_printf(proxyd_out,
		    "%s BAD Missing annotation attribute(s)\r\n", tag);
	goto baddata;
    }

    if (c == '(') {
	/* attrib list */
	do {
	    c = getnstring(proxyd_in, proxyd_out, &arg);
	    if (c == EOF) {
		prot_printf(proxyd_out,
			    "%s BAD Missing annotation attribute(s)\r\n", tag);
		goto baddata;
	    }

	    /* add the attrib to the list */
	    appendstrlist(attribs, arg.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(proxyd_out,
			"%s BAD Missing close paren in "
			"annotation attribute list\r\n", tag);
	    goto baddata;
	}

	c = prot_getc(proxyd_in);
    }
    else {
	/* single attrib */
	prot_ungetc(c, proxyd_in);
	c = getqstring(proxyd_in, proxyd_out, &arg);
	    if (c == EOF) {
		prot_printf(proxyd_out,
			    "%s BAD Missing annotation attribute\r\n", tag);
		goto baddata;
	    }

	appendstrlist(attribs, arg.s);
   }

    return c;

  baddata:
    if (c != EOF) prot_ungetc(c, proxyd_in);
    return EOF;
}

/*
 * Parse annotate store data.
 *
 * This is a generic routine which parses just the annotation data.
 * Any surrounding command text must be parsed elsewhere, ie,
 * SETANNOTATION, STORE, APPEND.
 */

int getannotatestoredata(char *tag, struct entryattlist **entryatts)
{
    int c;
    static struct buf entry, attrib, value;
    struct attvaluelist *attvalues = NULL;

    *entryatts = NULL;

    do {
	/* get entry */
	c = getqstring(proxyd_in, proxyd_out, &entry);
	if (c == EOF) {
	    prot_printf(proxyd_out,
			"%s BAD Missing annotation entry\r\n", tag);
	    goto baddata;
	}

	/* parse att-value list */
	if (c != ' ' || (c = prot_getc(proxyd_in)) != '(') {
	    prot_printf(proxyd_out,
			"%s BAD Missing annotation attribute-values list\r\n",
			tag);
	    goto baddata;
	}

	do {
	    /* get attrib */
	    c = getqstring(proxyd_in, proxyd_out, &attrib);
	    if (c == EOF) {
		prot_printf(proxyd_out,
			    "%s BAD Missing annotation attribute\r\n", tag);
		goto baddata;
	    }

	    /* get value */
	    if (c != ' ' ||
		(c = getnstring(proxyd_in, proxyd_out, &value)) == EOF) {
		prot_printf(proxyd_out,
			    "%s BAD Missing annotation value\r\n", tag);
		goto baddata;
	    }

	    /* add the attrib-value pair to the list */
	    appendattvalue(&attvalues, attrib.s, value.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(proxyd_out,
			"%s BAD Missing close paren in annotation "
			"attribute-values list\r\n", tag);
	    goto baddata;
	}

	/* add the entry to the list */
	appendentryatt(entryatts, entry.s, attvalues);
	attvalues = NULL;

	c = prot_getc(proxyd_in);

    } while (c == ' ');

    return c;

  baddata:
    if (attvalues) freeattvalues(attvalues);
    if (c != EOF) prot_ungetc(c, proxyd_in);
    return EOF;
}

/*
 * Output an entry/attribute-value list response.
 *
 * This is a generic routine which outputs just the annotation data.
 * Any surrounding response text must be output elsewhere, ie,
 * GETANNOTATION, FETCH. 
 */
void annotate_response(struct entryattlist *l)
{
    int islist; /* do we have more than one entry? */

    if (!l) return;

    islist = (l->next != NULL);

    if (islist) prot_printf(proxyd_out, "(");

    while (l) {
	prot_printf(proxyd_out, "\"%s\"", l->entry);

	/* do we have attributes?  solicited vs. unsolicited */
	if (l->attvalues) {
	    struct attvaluelist *av = l->attvalues;

	    prot_printf(proxyd_out, " (");
	    while (av) {
		prot_printf(proxyd_out, "\"%s\" ", av->attrib);
		if (!strcasecmp(av->value, "NIL"))
		    prot_printf(proxyd_out, "NIL");
		else
		    prot_printf(proxyd_out, "\"%s\"", av->value);

		if ((av = av->next) == NULL)
		    prot_printf(proxyd_out, ")");
		else
		    prot_printf(proxyd_out, " ");
	    }
	}
	if ((l = l->next) != NULL)
	    prot_printf(proxyd_out, " ");
    }

    if (islist) prot_printf(proxyd_out, ")");
}

/*
 * Perform a GETANNOTATION command
 *
 * The command has been parsed up to the entries
 */    
void cmd_getannotation(char *tag, char *mboxpat)
{
    int c, r = 0;
    struct strlist *entries = NULL, *attribs = NULL;

    c = getannotatefetchdata(tag, &entries, &attribs);
    if (c == EOF) {
	eatline(proxyd_in, c);
	return;
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(proxyd_in);
    if (c != '\n') {
	prot_printf(proxyd_out,
		    "%s BAD Unexpected extra arguments to Getannotation\r\n",
		    tag);
	eatline(proxyd_in, c);
	goto freeargs;
    }

    r = annotatemore_fetch(mboxpat, entries, attribs, &proxyd_namespace,
			   proxyd_userisadmin, proxyd_userid,
			   proxyd_authstate, proxyd_out);

    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(proxyd_out, "%s OK %s\r\n",
		    tag, error_message(IMAP_OK_COMPLETED));
    }
    
  freeargs:
    if (entries) freestrlist(entries);
    if (attribs) freestrlist(attribs);

    return;
}

/*
 * Perform a SETANNOTATION command
 *
 * The command has been parsed up to the entry-att list
 */    
void cmd_setannotation(char *tag, char *mboxpat __attribute__((unused)))
{
    int c, r = 0;
    struct entryattlist *entryatts = NULL;

    c = getannotatestoredata(tag, &entryatts);
    if (c == EOF) {
	eatline(proxyd_in, c);
	return;
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(proxyd_in);
    if (c != '\n') {
	prot_printf(proxyd_out,
		    "%s BAD Unexpected extra arguments to Setannotation\r\n",
		    tag);
	eatline(proxyd_in, c);
	goto freeargs;
    }

    r = annotatemore_store(mboxpat,
			   entryatts, &proxyd_namespace, proxyd_userisadmin,
			   proxyd_userid, proxyd_authstate);

    if (r) {
	prot_printf(proxyd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(proxyd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }

  freeargs:
    if (entryatts) freeentryatts(entryatts);
    return;
}

/* Proxy GETANNOTATION commands to backend */
int annotate_fetch_proxy(const char *server, const char *mbox_pat,
			 struct strlist *entry_pat,
			 struct strlist *attribute_pat) 
{
    struct backend *be;
    struct strlist *l;
    char mytag[128];
    
    assert(server && mbox_pat && entry_pat && attribute_pat);
    
    be = proxyd_findserver(server);    
    if(!be) return IMAP_SERVER_UNAVAILABLE;

    /* Send command to remote */
    proxyd_gentag(mytag, sizeof(mytag));
    prot_printf(be->out, "%s GETANNOTATION \"%s\" (", mytag, mbox_pat);
    for(l=entry_pat;l;l=l->next) {
	prot_printf(be->out, "\"%s\"%s", l->s, l->next ? " " : "");
    }
    prot_printf(be->out, ") (");
    for(l=attribute_pat;l;l=l->next) {
	prot_printf(be->out, "\"%s\"%s", l->s, l->next ? " " : "");
    }
    prot_printf(be->out, ")\r\n");
    prot_flush(be->out);

    /* Pipe the results.  Note that backend-current may also pipe us other
       messages. */
    pipe_until_tag(be, mytag, 0);

    return 0;
}

/* Proxy SETANNOTATION commands to backend */
int annotate_store_proxy(const char *server, const char *mbox_pat,
			 struct entryattlist *entryatts)
{
    struct backend *be;
    struct entryattlist *e;
    struct attvaluelist *av;
    char mytag[128];
    
    assert(server && mbox_pat && entryatts);
    
    be = proxyd_findserver(server);    
    if(!be) return IMAP_SERVER_UNAVAILABLE;

    /* Send command to remote */
    proxyd_gentag(mytag, sizeof(mytag));
    prot_printf(be->out, "%s SETANNOTATION \"%s\" (", mytag, mbox_pat);
    for (e = entryatts; e; e = e->next) {
	prot_printf(be->out, "\"%s\" (", e->entry);

	for (av = e->attvalues; av; av = av->next) {
	    prot_printf(be->out, "\"%s\" \"%s\"%s", av->attrib, av->value,
			av->next ? " " : "");
	}
	prot_printf(be->out, ")");
	if (e->next) prot_printf(be->out, " ");
    }
    prot_printf(be->out, ")\r\n");
    prot_flush(be->out);

    /* Pipe the results.  Note that backend-current may also pipe us other
       messages. */
    pipe_until_tag(be, mytag, 0);

    return 0;
}

/* Reset the given sasl_conn_t to a sane state */
static int reset_saslconn(sasl_conn_t **conn) 
{
    int ret;
    sasl_security_properties_t *secprops = NULL;

    sasl_dispose(conn);
    /* do initialization typical of service_main */
    ret = sasl_server_new("imap", config_servername,
                         NULL, NULL, NULL,
                         NULL, 0, conn);
    if(ret != SASL_OK) return ret;

    if(saslprops.ipremoteport)
       ret = sasl_setprop(*conn, SASL_IPREMOTEPORT,
                          saslprops.ipremoteport);
    if(ret != SASL_OK) return ret;
    
    if(saslprops.iplocalport)
       ret = sasl_setprop(*conn, SASL_IPLOCALPORT,
                          saslprops.iplocalport);
    if(ret != SASL_OK) return ret;
    
    secprops = mysasl_secprops(SASL_SEC_NOPLAINTEXT);
    ret = sasl_setprop(*conn, SASL_SEC_PROPS, secprops);
    if(ret != SASL_OK) return ret;
    /* end of service_main initialization excepting SSF */

    /* If we have TLS/SSL info, set it */
    if(saslprops.ssf) {
       ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &saslprops.ssf);
    } else {
       ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &extprops_ssf);
    }
    if(ret != SASL_OK) return ret;

    if(saslprops.authid) {
       ret = sasl_setprop(*conn, SASL_AUTH_EXTERNAL, saslprops.authid);
       if(ret != SASL_OK) return ret;
    }
    /* End TLS/SSL Info */

    return SASL_OK;
}
