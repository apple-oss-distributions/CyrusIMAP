/* fetchnews.c -- Program to pull new articles from a peer and push to server
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
 *
 * $Id: fetchnews.c,v 1.4 2005/03/05 00:36:48 dasenbro Exp $
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include "cyrusdb.h"
#include "exitcodes.h"
#include "global.h"
#include "lock.h"
#include "prot.h"
#include "xmalloc.h"

/* global state */
const int config_need_data = 0;

#define FNAME_NEWSRCDB "/fetchnews.db"
#define DB (&cyrusdb_flat)

static struct db *newsrc_db = NULL;
static int newsrc_dbopen = 0;

/* must be called after cyrus_init */
int newsrc_init(char *fname, int myflags __attribute__((unused)))
{
    char buf[1024];
    int r = 0;

    if (r != 0)
	syslog(LOG_ERR, "DBERROR: init %s: %s", buf,
	       cyrusdb_strerror(r));
    else {
	char *tofree = NULL;

	/* create db file name */
	if (!fname) {
	    fname = xmalloc(strlen(config_dir)+sizeof(FNAME_NEWSRCDB));
	    tofree = fname;
	    strcpy(fname, config_dir);
	    strcat(fname, FNAME_NEWSRCDB);
	}

	r = DB->open(fname, CYRUSDB_CREATE, &newsrc_db);
	if (r != 0)
	    syslog(LOG_ERR, "DBERROR: opening %s: %s", fname,
		   cyrusdb_strerror(r));
	else
	    newsrc_dbopen = 1;

	if (tofree) free(tofree);
    }

    return r;
}

int newsrc_done(void)
{
    int r = 0;

    if (newsrc_dbopen) {
	r = DB->close(newsrc_db);
	if (r) {
	    syslog(LOG_ERR, "DBERROR: error closing fetchnews.db: %s",
		   cyrusdb_strerror(r));
	}
	newsrc_dbopen = 0;
    }

    return r;
}

void usage(void)
{
    fprintf(stderr,
	    "fetchnews [-C <altconfig>] [-s <server>] [-n] [-w <wildmat>] [-f <tstamp file>]\n"
	    "          [-a <authname> [-p <password>]] <peer>\n");
    exit(-1);
}

int init_net(const char *host, char *port,
	     struct protstream **in, struct protstream **out)
{
    int sock = -1, err;
    struct addrinfo hints, *res, *res0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    if ((err = getaddrinfo(host, port, &hints, &res0)) != 0) {
	syslog(LOG_ERR, "getaddrinfo(%s, %s) failed: %m", host, port);
	return -1;
    }

    for (res = res0; res; res = res->ai_next) {
	if ((sock = socket(res->ai_family, res->ai_socktype,
			   res->ai_protocol)) < 0)
	    continue;
	if (connect(sock, res->ai_addr, res->ai_addrlen) >= 0)
	    break;
	close(sock);
	sock = -1;
    }
    freeaddrinfo(res0);
    if(sock < 0) {
	syslog(LOG_ERR, "connect(%s:%s) failed: %m", host, port);
	return -1;
    }
    
    *in = prot_new(sock, 0);
    *out = prot_new(sock, 1);
    prot_setflushonread(*in, *out);

    return sock;
}

int fetch(char *msgid, int bymsgid,
	  struct protstream *pin, struct protstream *pout,
	  struct protstream *sin, struct protstream *sout,
	  int *rejected, int *accepted, int *failed)
{
    char buf[4096];

    /* see if we want this article */
    prot_printf(sout, "IHAVE %s\r\n", msgid);
    if (!prot_fgets(buf, sizeof(buf), sin)) {
	syslog(LOG_ERR, "IHAVE terminated abnormally");
	return -1;
    }
    else if (strncmp("335", buf, 3)) {
	/* don't want it */
	(*rejected)++;
	return 0;
    }

    /* fetch the article */
    if (bymsgid)
	prot_printf(pout, "ARTICLE %s\r\n", msgid);
    else
	prot_printf(pout, "ARTICLE\r\n");

    if (!prot_fgets(buf, sizeof(buf), pin)) {
	syslog(LOG_ERR, "ARTICLE terminated abnormally");
	return -1;
    }
    else if (strncmp("220", buf, 3)) {
	/* doh! the article doesn't exist, terminate IHAVE */
	prot_printf(sout, ".\r\n");
    }
    else {
	/* store the article */
	while (prot_fgets(buf, sizeof(buf), pin)) {
	    if (buf[0] == '.') {
		if (buf[1] == '\r' && buf[2] == '\n') {
		    /* End of message */
		    prot_printf(sout, ".\r\n");
		    break;
		}
		else if (buf[1] != '.') {
		    /* Add missing dot-stuffing */
		    prot_putc('.', sout);
		}
	    }

	    do {
		/* look for malformed lines with NUL CR LF */
		if (buf[strlen(buf)-1] != '\n' &&
		    strlen(buf)+2 < sizeof(buf)-1 &&
		    buf[strlen(buf)+2] == '\n') {
		    strlcat(buf, "\r\n", sizeof(buf));
		}
		prot_printf(sout, "%s", buf);
	    } while (buf[strlen(buf)-1] != '\n' &&
		     prot_fgets(buf, sizeof(buf), pin));
	}

	if (buf[0] != '.') {
	    syslog(LOG_ERR, "ARTICLE terminated abnormally");
	    return -1;
	}
    }

    /* see how we did */
    if (!prot_fgets(buf, sizeof(buf), sin)) {
	syslog(LOG_ERR, "IHAVE terminated abnormally");
	return -1;
    }
    else if (!strncmp("235", buf, 3))
	(*accepted)++;
    else
	(*failed)++;

    return 0;
}

#define RESP_GROW 100
#define BUFFERSIZE 4096

int main(int argc, char *argv[])
{
    extern char *optarg;
    int opt;
    char *alt_config = NULL, *port = "119";
    const char *peer = NULL, *server = "localhost", *wildmat = "*";
    char *authname = NULL, *password = NULL;
    int psock = -1, ssock = -1;
    struct protstream *pin, *pout, *sin, *sout;
    char buf[BUFFERSIZE];
    char sfile[1024] = "";
    int fd = -1, i, n, offered, rejected, accepted, failed;
    time_t stamp;
    struct tm *tm;
    char **resp = NULL;
    int newnews = 1;

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);

    while ((opt = getopt(argc, argv, "C:s:w:f:a:p:n")) != EOF) {
	switch (opt) {
	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	case 's': /* server */
	    server = xstrdup(optarg);
	    if ((port = strchr(server, ':')))
		*port++ = '\0';
	    else
		port = "119";
	    break;

	case 'w': /* wildmat */
	    wildmat = optarg;
	    break;

	case 'f': /* timestamp file */
	    snprintf(sfile, sizeof(sfile), optarg);
	    break;

	case 'a': /* authname */
	    authname = optarg;
	    break;

	case 'p': /* password */
	    password = optarg;
	    break;

	case 'n': /* no newnews */
	    newnews = 0;
	    break;

	default:
	    usage();
	    /* NOTREACHED */
	}
    }
    if (argc - optind < 1) {
	usage();
	/* NOTREACHED */
    }

    peer = argv[optind++];

    cyrus_init(alt_config, "fetchnews", 0);

    /* connect to the peer */
    /* xxx configurable port number? */
    if ((psock = init_net(peer, "119", &pin, &pout)) < 0) {
	fprintf(stderr, "connection to %s failed\n", peer);
	cyrus_done();
	exit(-1);
    }

    /* read the initial greeting */
    if (!prot_fgets(buf, sizeof(buf), pin) || strncmp("20", buf, 2)) {
	syslog(LOG_ERR, "peer not available");
	goto quit;
    }

    if (authname) {
	/* authenticate to peer */
	/* XXX this should be modified to support SASL and STARTTLS */

	prot_printf(pout, "AUTHINFO USER %s\r\n", authname);
	if (!prot_fgets(buf, sizeof(buf), pin)) {
	    syslog(LOG_ERR, "AUTHINFO USER terminated abnormally");
	    goto quit;
	}
	else if (!strncmp("381", buf, 3)) {
	    /* password required */
	    if (!password)
		password = getpass("Please enter the password: ");

	    if (!password) {
		fprintf(stderr, "failed to get password\n");
		goto quit;
	    }

	    prot_printf(pout, "AUTHINFO PASS %s\r\n", password);
	    if (!prot_fgets(buf, sizeof(buf), pin)) {
		syslog(LOG_ERR, "AUTHINFO PASS terminated abnormally");
		goto quit;
	    }
	}

	if (strncmp("281", buf, 3)) {
	    /* auth failed */
	    goto quit;
	}
    }

    /* change to reader mode - not always necessary, so ignore result */
    prot_printf(pout, "MODE READER\r\n");
    prot_fgets(buf, sizeof(buf), pin);

    if (newnews) {
	/* read the previous timestamp */
	if (!sfile[0]) {
	    char oldfile[1024];

	    snprintf(sfile, sizeof(sfile), "%s/fetchnews.stamp", config_dir);

	    /* upgrade from the old stamp filename to the new */
	    snprintf(oldfile, sizeof(oldfile), "%s/newsstamp", config_dir);
	    rename(oldfile, sfile);
	}

	if ((fd = open(sfile, O_RDWR | O_CREAT, 0644)) == -1) {
	    syslog(LOG_ERR, "can not open %s", sfile);
	    goto quit;
	}
	if (lock_nonblocking(fd) == -1) {
	    syslog(LOG_ERR, "can not lock %s: %m", sfile);
	    goto quit;
	}

	if (read(fd, &stamp, sizeof(stamp)) < sizeof(stamp)) {
	    /* XXX do something better here */
	    stamp = 0;
	}

	/* ask for new articles */
	tm = gmtime(&stamp);
	strftime(buf, sizeof(buf), "%Y%m%d %H%M%S", tm);
	prot_printf(pout, "NEWNEWS %s %s GMT\r\n", wildmat, buf);
	
	if (!prot_fgets(buf, sizeof(buf), pin) || strncmp("230", buf, 3)) {
	    syslog(LOG_ERR, "peer doesn't support NEWNEWS");
	    newnews = 0;
	}

	stamp = time(NULL);
    }

    if (!newnews) {
	prot_printf(pout, "LIST ACTIVE %s\r\n", wildmat);
	
	if (!prot_fgets(buf, sizeof(buf), pin) || strncmp("215", buf, 3)) {
	    syslog(LOG_ERR, "peer doesn't support LIST ACTIVE");
	    goto quit;
	}
    }

    /* process the NEWNEWS/LIST ACTIVE list */
    n = 0;
    while (prot_fgets(buf, sizeof(buf), pin)) {
	if (buf[0] == '.') break;

	if (!(n % RESP_GROW)) { /* time to alloc more */
	    resp = (char **)
		xrealloc(resp, (n + RESP_GROW) * sizeof(char *));
	}
	resp[n++] = xstrdup(buf);
    }
    if (buf[0] != '.') {
	syslog(LOG_ERR, "%s terminated abnormally",
	       newnews ? "NEWNEWS" : "LIST ACTIVE");
	goto quit;
    }

    if (!n) {
	/* nothing matches our wildmat */
	goto quit;
    }

    /* connect to the server */
    if ((ssock = init_net(server, port, &sin, &sout)) < 0) {
	fprintf(stderr, "connection to %s failed\n", server);
	goto quit;
    }

    /* read the initial greeting */
    if (!prot_fgets(buf, sizeof(buf), sin) || strncmp("20", buf, 2)) {
	syslog(LOG_ERR, "server not available");
	goto quit;
    }

    /* fetch and store articles */
    offered = rejected = accepted = failed = 0;
    if (newnews) {
	/* response is a list of msgids */
	for (i = 0; i < n; i++) {
	    /* find the end of the msgid */
	    *(strrchr(resp[i], '>') + 1) = '\0';

	    offered++;
	    if (fetch(resp[i], 1, pin, pout, sin, sout,
		      &rejected, &accepted, &failed)) {
		goto quit;
	    }
	}

	/* write the current timestamp */
	lseek(fd, 0, SEEK_SET);
	if (write(fd, &stamp, sizeof(stamp)) < sizeof(stamp))
	    syslog(LOG_ERR, "error writing %s", sfile);
	lock_unlock(fd);
	close(fd);
    }
    else {
	char group[BUFFERSIZE], msgid[BUFFERSIZE], lastbuf[50];
	const char *data;
	unsigned long low, high, last, cur;
	int start;
	int datalen;
	struct txn *tid = NULL;

	newsrc_init(NULL, 0);

	/*
	 * response is a list of groups.
	 * select each group, and STAT each article we haven't seen yet.
	 */
	for (i = 0; i < n; i++) {
	    /* parse the LIST ACTIVE response */
	    sscanf(resp[i], "%s %lu %lu", group, &high, &low);

	    last = 0;
	    if (!DB->fetchlock(newsrc_db, group, strlen(group),
			       &data, &datalen, &tid)) {
		last = strtoul(data, NULL, 10);
	    }
	    if (high <= last) continue;

	    /* select the group */
	    prot_printf(pout, "GROUP %s\r\n", group);
	    if (!prot_fgets(buf, sizeof(buf), pin)) {
		syslog(LOG_ERR, "GROUP terminated abnormally");
		continue;
	    }
	    else if (strncmp("211", buf, 3)) break;

	    for (start = 1, cur = low > last ? low : ++last;; cur++) {
		if (start) {
		    /* STAT the first article we haven't seen */
		    prot_printf(pout, "STAT %lu\r\n", cur);
		} else {
		    /* continue with the NEXT article */
		    prot_printf(pout, "NEXT\r\n");
		}

		if (!prot_fgets(buf, sizeof(buf), pin)) {
		    syslog(LOG_ERR, "STAT/NEXT terminated abnormally");
		    cur--;
		    break;
		}
		if (!strncmp("223", buf, 3)) {
		    /* parse the STAT/NEXT response */
		    sscanf(buf, "223 %lu %s", &cur, msgid);

		    /* find the end of the msgid */
		    *(strrchr(msgid, '>') + 1) = '\0';

		    if (fetch(msgid, 0, pin, pout, sin, sout,
			      &rejected, &accepted, &failed)) {
			cur--;
			break;
		    }
		    offered++;
		    start = 0;
		}

		/* have we reached the highwater mark? */
		if (cur >= high) break;
	    }

	    snprintf(lastbuf, sizeof(lastbuf), "%lu", cur);
	    DB->store(newsrc_db, group, strlen(group),
		      lastbuf, strlen(lastbuf)+1, &tid);
	}

	if (tid) DB->commit(newsrc_db, tid);
	newsrc_done();
    }

    syslog(LOG_NOTICE,
	   "fetchnews: %s offered %d; %s rejected %d, accepted %d, failed %d",
	   peer, offered, server, rejected, accepted, failed);

  quit:
    if (psock >= 0) {
	prot_printf(pout, "QUIT\r\n");
	prot_flush(pout);

	/* Flush the incoming buffer */
	prot_NONBLOCK(pin);
	prot_fill(pin);

	/* close/free socket & prot layer */
	close(psock);
    
	prot_free(pin);
	prot_free(pout);
    }

    if (ssock >= 0) {
	prot_printf(sout, "QUIT\r\n");
	prot_flush(sout);

	/* Flush the incoming buffer */
	prot_NONBLOCK(sin);
	prot_fill(sin);

	/* close/free socket & prot layer */
	close(psock);
    
	prot_free(sin);
	prot_free(sout);
    }

    cyrus_done();
    
    return 0;
}