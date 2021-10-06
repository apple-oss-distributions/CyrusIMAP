/* mupdate-client.c -- cyrus murder database clients
 *
 * $Id: mupdate-client.c,v 1.46 2004/08/25 15:35:41 ken3 Exp $
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

#include <config.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <sasl/sasl.h>
#include <sasl/saslutil.h>
#include <syslog.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#else
#include <varargs.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "assert.h"
#include "cyrusdb.h"
#include "exitcodes.h"
#include "global.h"
#include "imparse.h"
#include "iptostring.h"
#include "mupdate.h"
#include "mupdate_err.h"
#include "prot.h"
#include "protocol.h"
#include "xmalloc.h"

const char service_name[] = "mupdate";

static sasl_security_properties_t *make_secprops(void)
{
  sasl_security_properties_t *ret =
      (sasl_security_properties_t *) xzmalloc(sizeof(sasl_security_properties_t));

  ret->maxbufsize = PROT_BUFSIZE;
  ret->min_ssf = config_getint(IMAPOPT_SASL_MINIMUM_LAYER);	
  ret->max_ssf = config_getint(IMAPOPT_SASL_MAXIMUM_LAYER);

  return ret;
}

int mupdate_connect(const char *server, const char *port,
		    mupdate_handle **handle,
		    sasl_callback_t *cbs)
{
    mupdate_handle *h = NULL;
    struct addrinfo hints, *res0, *res;
    int err = 0;
    int local_cbs = 0;
    int s, saslresult;
    const char *proterr;
    char buf[4096];
    char *mechlist = NULL;
    sasl_security_properties_t *secprops = NULL;
    socklen_t addrsize;
    struct sockaddr_storage saddr_l;
    struct sockaddr_storage saddr_r;
    char localip[60], remoteip[60];
    const char *sasl_status = NULL;
    char portstr[NI_MAXSERV];
    const char *forcemech;
    
    if(!handle)
	return MUPDATE_BADPARAM;

    /* open connection to 'server' */
    if(!server) {
	server = config_mupdate_server;
	if (server == NULL) {
	    fatal("couldn't get mupdate server name", EC_UNAVAILABLE);
	}
    }
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if(port)
	err = getaddrinfo(server, port, &hints, &res0);
    if (!port || err == EAI_SERVICE) {
	err = getaddrinfo(server, "mupdate", &hints, &res0);
	if (err == EAI_SERVICE) {
	    snprintf(portstr, sizeof(portstr), "%d",
		     config_getint(IMAPOPT_MUPDATE_PORT));
	    err = getaddrinfo(server, portstr, &hints, &res0);
	}
    }
    
    if (err) {
	syslog(LOG_ERR, "mupdate-client: getaddrinfo(%s, %s) failed: %s",
	       server, port, gai_strerror(err));
	return MUPDATE_NOCONN;
    }
    
    s = -1;
    for (res = res0; res; res = res->ai_next) {
	s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s < 0)
	    continue;
	if (connect(s, res->ai_addr, res->ai_addrlen) >= 0)
	    break;
	close(s);
	s = -1;
    }

    freeaddrinfo(res0);
    if (s < 0) {
	syslog(LOG_ERR, "mupdate-client: connect(%s): %m", server);
	return MUPDATE_NOCONN;
    }

    h = xzmalloc(sizeof(mupdate_handle));
    *handle = h;
    h->sock = s;

    if(!cbs) {
	local_cbs = 1;
	cbs = mysasl_callbacks(config_getstring(IMAPOPT_MUPDATE_USERNAME),
			       config_getstring(IMAPOPT_MUPDATE_AUTHNAME),
			       config_getstring(IMAPOPT_MUPDATE_REALM),
			       config_getstring(IMAPOPT_MUPDATE_PASSWORD));
    }

    /* create protstream */
    h->pin = prot_new(h->sock, 0);
    h->pout = prot_new(h->sock, 1);

    prot_setflushonread(h->pin, h->pout);
    prot_settimeout(h->pin, 30*60);

    /* set the IP addresses */
    addrsize=sizeof(struct sockaddr_storage);
    if (getpeername(h->sock,(struct sockaddr *)&saddr_r,&addrsize)!=0)
	goto noconn;

    addrsize=sizeof(struct sockaddr_storage);
    if (getsockname(h->sock,(struct sockaddr *)&saddr_l,&addrsize)!=0)
	goto noconn;

    if(iptostring((const struct sockaddr *)&saddr_l, addrsize,
		  localip, 60) != 0)
	goto noconn;
    
    if(iptostring((const struct sockaddr *)&saddr_r, addrsize,
		  remoteip, 60) != 0)
	goto noconn;

    saslresult = sasl_client_new(service_name,
				 server,
				 localip, remoteip,
				 cbs,
				 0,
				 &(h->saslconn));
    if(saslresult != SASL_OK) goto noconn;

    secprops = make_secprops();
    if(!secprops) goto noconn;
    
    saslresult=sasl_setprop(h->saslconn, SASL_SEC_PROPS, secprops);
    if(saslresult != SASL_OK) goto noconn;
    free(secprops);
    secprops = NULL;

    /* Read the mechlist & other capabilities */
    while(1) {
	if (!prot_fgets(buf, sizeof(buf)-1, h->pin)) {
	    goto noconn;
	}

	if(!strncmp(buf, "* AUTH", 6)) {
	    mechlist = xstrdup(buf + 6);
	} else if(!strncmp(buf, "* OK MUPDATE", 12)) {
	    break;
	}
    }

    if(!mechlist) {
	syslog(LOG_ERR, "no AUTH banner from remote");
	mupdate_disconnect(handle);
	free_callbacks(cbs);
	return MUPDATE_NOAUTH;
    }
    
    forcemech = config_getstring(IMAPOPT_FORCE_SASL_CLIENT_MECH);
    if(forcemech) {
	free(mechlist);
	mechlist = xstrdup(forcemech);
    }

    if (h->saslcompleted) {
	syslog(LOG_ERR,
	       "Already authenticated to remote mupdate server in mupdate_connect.  Continuing.");
    } else if(saslclient(h->saslconn,
			 &protocol[PROTOCOL_MUPDATE].sasl_cmd,
			 mechlist, h->pin, h->pout, NULL,
			 &sasl_status) != SASL_OK) {
	syslog(LOG_ERR, "authentication to remote mupdate server failed: %s",
	       sasl_status ? sasl_status : "unspecified saslclient() error");
	free(mechlist);
	mupdate_disconnect(handle);
	free_callbacks(cbs);
	return MUPDATE_NOAUTH;
    }

    free(mechlist);
    mechlist = NULL;

    /* xxx unclear that this is correct, but it prevents a memory leak */
    if(local_cbs) free_callbacks(cbs);
    
    prot_setsasl(h->pin, h->saslconn);
    prot_setsasl(h->pout, h->saslconn);

    h->saslcompleted = 1;

    /* SUCCESS */
    return 0;

 noconn:
    if(mechlist) free(mechlist);
    if(secprops) free(secprops);
    proterr = prot_error(h->pin);
    syslog(LOG_ERR, "mupdate-client: connection to server closed: %s",
	   proterr ? proterr : "(unknown)");
    mupdate_disconnect(handle);

    return MUPDATE_NOCONN;
}

void mupdate_disconnect(mupdate_handle **hp)
{
    mupdate_handle *h;

    if(!hp || !(*hp)) return;
    h = *hp;

    if(h->pout) {
	prot_printf(h->pout, "L01 LOGOUT\r\n");
	prot_flush(h->pout);
    }
    
    freebuf(&(h->tag));
    freebuf(&(h->cmd));
    freebuf(&(h->arg1));
    freebuf(&(h->arg2));
    freebuf(&(h->arg3));
    
    if(h->pin) prot_free(h->pin);
    if(h->pout) prot_free(h->pout);
    sasl_dispose(&(h->saslconn));
    close(h->sock);

    if(h->acl_buf) free(h->acl_buf);

    free(h); 
    *hp = NULL;
}

/* We're really only looking for an OK or NO or BAD here -- and the callback
 * is never called in those cases.  So if the callback is called, we have
 * an error! */
static int mupdate_scarf_one(struct mupdate_mailboxdata *mdata __attribute__((unused)),
			     const char *cmd,
			     void *context __attribute__((unused))) 
{
    syslog(LOG_ERR, "mupdate_scarf_one was called, but shouldn't be.  Command recieved was %s", cmd);
    return -1;
}

int mupdate_activate(mupdate_handle *handle, 
		     const char *mailbox, const char *server,
		     const char *acl)
{
    int ret;
    enum mupdate_cmd_response response;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox || !server || !acl) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u ACTIVATE {%d+}\r\n%s {%d+}\r\n%s {%d+}\r\n%s\r\n", 
		handle->tagn++, strlen(mailbox), mailbox, 
		strlen(server), server, strlen(acl), acl);

    ret = mupdate_scarf(handle, mupdate_scarf_one, NULL, 1, &response);
    if (ret) {
	return ret;
    } else if (response != MUPDATE_OK) {
	return MUPDATE_FAIL;
    } else {
	return 0;
    }
}

int mupdate_reserve(mupdate_handle *handle,
		    const char *mailbox, const char *server)
{
    int ret;
    enum mupdate_cmd_response response;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox || !server) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u RESERVE {%d+}\r\n%s {%d+}\r\n%s\r\n",
		handle->tagn++, strlen(mailbox), mailbox, 
		strlen(server), server);

    ret = mupdate_scarf(handle, mupdate_scarf_one, NULL, 1, &response);
    if (ret) {
	return ret;
    } else if (response != MUPDATE_OK) {
	return MUPDATE_FAIL_RESERVE;
    } else {
	return 0;
    }
}

int mupdate_deactivate(mupdate_handle *handle,
		       const char *mailbox, const char *server)
{
    int ret;
    enum mupdate_cmd_response response;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox || !server) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u DEACTIVATE {%d+}\r\n%s {%d+}\r\n%s\r\n",
		handle->tagn++, strlen(mailbox), mailbox, 
		strlen(server), server);

    ret = mupdate_scarf(handle, mupdate_scarf_one, NULL, 1, &response);
    if (ret) {
	return ret;
    } else if (response != MUPDATE_OK) {
	return MUPDATE_FAIL_RESERVE;
    } else {
	return 0;
    }
}

int mupdate_delete(mupdate_handle *handle,
		   const char *mailbox)
{
    int ret;
    enum mupdate_cmd_response response;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u DELETE {%d+}\r\n%s\r\n", handle->tagn++, 
		strlen(mailbox), mailbox);

    ret = mupdate_scarf(handle, mupdate_scarf_one, NULL, 1, &response);
    if (ret) {
	return ret;
    } else if (response != MUPDATE_OK) {
	return MUPDATE_FAIL;
    } else {
	return 0;
    }
}


static int mupdate_find_cb(struct mupdate_mailboxdata *mdata,
			   const char *cmd, void *context) 
{
    struct mupdate_handle_s *h = (struct mupdate_handle_s *)context;

    if(!h || !cmd || !mdata) return 1;

    /* coyp the data to the handle storage */
    /* xxx why can't we just point to the 'mdata' buffers? */
    strlcpy(h->mailbox_buf, mdata->mailbox, sizeof(h->mailbox_buf));
    strlcpy(h->server_buf, mdata->server, sizeof(h->server_buf));

    if(!strncmp(cmd, "MAILBOX", 7)) {
	int len = strlen(mdata->acl) + 1;
	
	h->mailboxdata_buf.t = ACTIVE;
	
	if(len > h->acl_buf_len) {
	    /* we want to at least double the buffer */
	    if (len < 2 * h->acl_buf_len) {
		len = 2 * h->acl_buf_len;
	    }

	    h->acl_buf = xrealloc(h->acl_buf, len);
	    strcpy(h->acl_buf, mdata->acl);
	}
    } else if (!strncmp(cmd, "RESERVE", 7)) {
	h->mailboxdata_buf.t = RESERVE;
	if(!h->acl_buf) {
	    h->acl_buf = xstrdup("");
	    h->acl_buf_len = 1;
	} else {
	    h->acl_buf[0] = '\0';
	}
    } else {
	/* Bad command */
	return 1;
    }
   
    h->mailboxdata_buf.mailbox = h->mailbox_buf;
    h->mailboxdata_buf.server = h->server_buf;
    h->mailboxdata_buf.acl = h->acl_buf;
    
    return 0;
}

int mupdate_find(mupdate_handle *handle, const char *mailbox,
		 struct mupdate_mailboxdata **target) 
{
    int ret;
    enum mupdate_cmd_response response;
    
    if(!handle || !mailbox || !target) return MUPDATE_BADPARAM;

    prot_printf(handle->pout,
		"X%u FIND {%d+}\r\n%s\r\n", handle->tagn++, 
		strlen(mailbox), mailbox);

    memset(&(handle->mailboxdata_buf), 0, sizeof(handle->mailboxdata_buf));

    ret = mupdate_scarf(handle, mupdate_find_cb, handle, 1, &response);

    /* note that the response is still OK even if there was no data returned,
     * so we have to make sure we actually filled in the data too */
    if (!ret && response == MUPDATE_OK && handle->mailboxdata_buf.mailbox) {
	*target = &(handle->mailboxdata_buf);
	return 0;
    } else if(!ret && response == MUPDATE_OK) {
	/* it looked okay, but we didn't get a mailbox */
	*target = NULL;
	return MUPDATE_MAILBOX_UNKNOWN;
    } else {
	/* Something Bad happened */
	*target = NULL;
	return ret ? ret : MUPDATE_FAIL;
    }
}

int mupdate_list(mupdate_handle *handle, mupdate_callback callback,
		 const char *prefix, void *context) 
{
    int ret;
    enum mupdate_cmd_response response;
    
    if(!handle || !callback) return MUPDATE_BADPARAM;

    if(prefix) {
	prot_printf(handle->pout,
		    "X%u LIST {%d+}\r\n%s\r\n", handle->tagn++,
		    strlen(prefix), prefix);
    } else {
	prot_printf(handle->pout,
		    "X%u LIST\r\n", handle->tagn++);
    }
     
    ret = mupdate_scarf(handle, callback, context, 1, &response);

    if (ret) {
	return ret;
    } else if (response != MUPDATE_OK) {
	return MUPDATE_FAIL;
    } else {
	return 0;
    }
}


int mupdate_noop(mupdate_handle *handle, mupdate_callback callback,
		 void *context)
{
    int ret;
    enum mupdate_cmd_response response;
    
    if(!handle || !callback) return MUPDATE_BADPARAM;

    prot_printf(handle->pout,
		"X%u NOOP\r\n", handle->tagn++);

    ret = mupdate_scarf(handle, callback, context, 1, &response);

    if (!ret && response == MUPDATE_OK) {
	return 0;
    } else {
	return ret ? ret : MUPDATE_FAIL;
    }
}

#define CHECKNEWLINE(c, ch) do { if ((ch) == '\r') (ch)=prot_getc((c)->pin); \
                                 if ((ch) != '\n') { syslog(LOG_ERR, \
                             "extra arguments recieved, aborting connection");\
                                 r = MUPDATE_PROTOCOL_ERROR;\
                                 goto done; }} while(0)

/* Scarf up the incoming data and perform the requested operations */
int mupdate_scarf(mupdate_handle *handle, 
		  mupdate_callback callback,
		  void *context, 
		  int wait_for_ok, 
		  enum mupdate_cmd_response *response)
{
    struct mupdate_mailboxdata box;
    int r = 0;

    if (!handle || !callback) return MUPDATE_BADPARAM;

    /* keep going while we have input or if we're waiting for an OK */
    while (!r) {
	int ch;
	unsigned char *p;
    
	if (wait_for_ok) {
	    prot_BLOCK(handle->pin);
	} else {
	    /* check for input */
	    prot_NONBLOCK(handle->pin);
	    ch = prot_getc(handle->pin);

	    if(ch == EOF && errno == EAGAIN) {
		/* this was just "no input" we return 0 */
		goto done;
	    } else if(ch == EOF) {
		/* this was a fatal error */
		r = MUPDATE_NOCONN;
		goto done;
	    } else {
		/* there's input waiting, put back our character */
		prot_ungetc(ch, handle->pin);
	    }

	    /* Set it back to blocking so we don't get half a word */
	    prot_BLOCK(handle->pin);
	}

	ch = getword(handle->pin, &(handle->tag));
	if (ch == EOF) {
	    /* this was a fatal error */
	    r = MUPDATE_NOCONN;
	    goto done;
	}

	if(ch != ' ') {
	    /* We always have a command */
	    syslog(LOG_ERR, "Protocol error from master: no tag");
	    r = MUPDATE_PROTOCOL_ERROR;
	    goto done;
	}
	ch = getword(handle->pin, &(handle->cmd));
	if(ch != ' ') {
	    /* We always have an argument */
	    syslog(LOG_ERR, "Protocol error from master: no keyword");
	    r = MUPDATE_PROTOCOL_ERROR;
	    break;
	}
	
	if (islower((unsigned char) handle->cmd.s[0])) {
	    handle->cmd.s[0] = toupper((unsigned char) handle->cmd.s[0]);
	}
	for (p = &(handle->cmd.s[1]); *p; p++) {
	    if (islower((unsigned char) *p))
		*p = toupper((unsigned char) *p);
	}
	
	switch(handle->cmd.s[0]) {
	case 'B':
	    if(!strncmp(handle->cmd.s, "BAD", 3)) {
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		CHECKNEWLINE(handle, ch);

		syslog(LOG_ERR, "mupdate BAD response: %s", handle->arg1.s);
		if (wait_for_ok && response) {
		    *response = MUPDATE_BAD;
		}
		goto done;
	    } else if (!strncmp(handle->cmd.s, "BYE", 3)) {
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		CHECKNEWLINE(handle, ch);
		
		syslog(LOG_ERR, "mupdate BYE response: %s", handle->arg1.s);
		if(wait_for_ok && response) {
		    *response = MUPDATE_BYE;
		}
		goto done;
	    }
	    goto badcmd;

	case 'D':
	    if(!strncmp(handle->cmd.s, "DELETE", 6)) {
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		CHECKNEWLINE(handle, ch);

		memset(&box, 0, sizeof(box));
		box.mailbox = handle->arg1.s;

		/* Handle delete command */
		r = callback(&box, handle->cmd.s, context);
		if (r) {
		    syslog(LOG_ERR, 
			   "error deleting mailbox: callback returned %d", r);
		    goto done;
		}
		break;
	    }
	    goto badcmd;

	case 'M':
	    if(!strncmp(handle->cmd.s, "MAILBOX", 7)) {
		/* Mailbox Name */
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		if(ch != ' ') { 
		    r = MUPDATE_PROTOCOL_ERROR;
		    goto done;
		}
		
		/* Server */
		ch = getstring(handle->pin, handle->pout, &(handle->arg2));
		if(ch != ' ') {
		    r = MUPDATE_PROTOCOL_ERROR;
		    goto done;
		}
		
		/* ACL */
		ch = getstring(handle->pin, handle->pout, &(handle->arg3));
		CHECKNEWLINE(handle, ch);
		
		/* Handle mailbox command */
		memset(&box, 0, sizeof(box));
		box.mailbox = handle->arg1.s;
		box.server = handle->arg2.s;
		box.acl = handle->arg3.s;
		r = callback(&box, handle->cmd.s, context);
		if (r) { /* callback error ? */
		    syslog(LOG_ERR, 
			   "error activating mailbox: callback returned %d", r);
		    goto done;
		}
		break;
	    }
	    goto badcmd;
	case 'N':
	    if(!strncmp(handle->cmd.s, "NO", 2)) {
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		CHECKNEWLINE(handle, ch);

		syslog(LOG_DEBUG, "mupdate NO response: %s", handle->arg1.s);
		if (wait_for_ok) {
		    if (response) *response = MUPDATE_NO;
		    goto done;
		}
		break;
	    }
	    goto badcmd;
	case 'O':
	    if(!strncmp(handle->cmd.s, "OK", 2)) {
		/* It's all good, grab the attached string and move on */
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		
		CHECKNEWLINE(handle, ch);
		if (wait_for_ok) {
		    if (response) *response = MUPDATE_OK;
		    goto done;
		}
		break;
	    }
	    goto badcmd;
	case 'R':
	    if(!strncmp(handle->cmd.s, "RESERVE", 7)) {
		/* Mailbox Name */
		ch = getstring(handle->pin, handle->pout, &(handle->arg1));
		if(ch != ' ') {
		    r = MUPDATE_PROTOCOL_ERROR;
		    goto done;
		}
		
		/* Server */
		ch = getstring(handle->pin, handle->pout, &(handle->arg2));
		CHECKNEWLINE(handle, ch);
		
		/* Handle reserve command */
		memset(&box, 0, sizeof(box));
		box.mailbox = handle->arg1.s;
		box.server = handle->arg2.s;
		r = callback(&box, handle->cmd.s, context);
		if (r) { /* callback error ? */
		    syslog(LOG_ERR, 
			   "error reserving mailbox: callback returned %d", r);
		    goto done;
		}
		
		break;
	    }
	    goto badcmd;

	default:
	badcmd:
	    /* Bad Command */
	    syslog(LOG_ERR, "bad/unexpected command from master: %s",
		   handle->cmd.s);
	    r = MUPDATE_PROTOCOL_ERROR;
	    goto done;
	}
    }

 done:
    /* reset blocking */
    prot_NONBLOCK(handle->pin);

    return r;
}

void kick_mupdate(void)
{
    char buf[2048];
    struct sockaddr_un srvaddr;
    int s, r;
    int len;
    
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == -1) {
	syslog(LOG_ERR, "socket: %m");
	return;
    }

    strlcpy(buf, config_dir, sizeof(buf));
    strlcat(buf, FNAME_MUPDATE_TARGET_SOCK, sizeof(buf));
    memset((char *)&srvaddr, 0, sizeof(srvaddr));
    srvaddr.sun_family = AF_UNIX;
    strcpy(srvaddr.sun_path, buf);
    len = sizeof(srvaddr.sun_family) + strlen(srvaddr.sun_path) + 1;

    r = connect(s, (struct sockaddr *)&srvaddr, len);
    if (r == -1) {
	syslog(LOG_ERR, "kick_mupdate: can't connect to target: %m");
	goto done;
    }

    r = read(s, buf, sizeof(buf));
    if (r <= 0) {
	syslog(LOG_ERR, "kick_mupdate: can't read from target: %m");
    }

 done:
    close(s);
    return;
}
