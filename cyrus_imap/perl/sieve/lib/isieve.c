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
 *
 */

/* $Id: isieve.c,v 1.28 2004/06/17 18:06:54 rjs3 Exp $ */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>

#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "isieve.h"
#include "lex.h"
#include "request.h"
#include "iptostring.h"
#include "xmalloc.h"
#include "util.h"

#include <prot.h>

struct iseive_s {
    char *serverFQDN;
    int port;

    int sock;

    sasl_conn_t *conn;
    sasl_callback_t *callbacks;

    char *refer_authinfo;
    sasl_callback_t *refer_callbacks;

    int version;

    struct protstream *pin;
    struct protstream *pout;
};

void fillin_interactions(sasl_interact_t *tlist);

/* we need this separate from the free() call so that we can reuse
 * the same memory for referrals */
static void sieve_dispose(isieve_t *obj) 
{
    if(!obj) return;
    sasl_dispose(&obj->conn);
    free(obj->serverFQDN);

    if (obj->refer_authinfo) free(obj->refer_authinfo);
    if (obj->refer_callbacks) free(obj->refer_callbacks);

    prot_free(obj->pin);
    prot_free(obj->pout);
}

void sieve_free_net(isieve_t *obj) 
{
    sieve_dispose(obj);
    free(obj);
}

/* initialize the network */
int init_net(char *serverFQDN, int port, isieve_t **obj)
{
  struct addrinfo hints, *res0, *res;
  int err;
  char portstr[6];
  int sock = -1;
    
  snprintf(portstr, sizeof(portstr), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if ((err = getaddrinfo(serverFQDN, portstr, &hints, &res0)) != 0) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err)); 
      return -1;
  }
    
  for (res = res0; res; res = res->ai_next) {
      sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (sock < 0)
	  continue;
      if (connect(sock, res->ai_addr, res->ai_addrlen) >= 0)
	  break;
      close(sock);
      sock = -1;
  }

  freeaddrinfo(res0);
  if (sock < 0) {
    perror("connect");
    return -1;
  }

  *obj = (isieve_t *) xmalloc(sizeof(isieve_t));
  if (!*obj) return -1;

  memset(*obj, '\0', sizeof(isieve_t));

  (*obj)->sock = sock;
  (*obj)->serverFQDN = xstrdup(serverFQDN);
  (*obj)->port = port;

  /* set up the prot layer */
  (*obj)->pin = prot_new(sock, 0);
  (*obj)->pout = prot_new(sock, 1); 

  return 0;
}

static sasl_security_properties_t *make_secprops(int min,int max)
{
  sasl_security_properties_t *ret=(sasl_security_properties_t *)
    xmalloc(sizeof(sasl_security_properties_t));

  ret->maxbufsize=1024;
  ret->min_ssf=min;
  ret->max_ssf=max;

  /* should make this configurable */
  ret->security_flags=SASL_SEC_NOANONYMOUS;
  ret->property_names=NULL;
  ret->property_values=NULL;

  return ret;
}

/*
 * Initialize SASL and set necessary options
 */
int init_sasl(isieve_t *obj,
	      int ssf,
	      sasl_callback_t *callbacks)
{
  static int sasl_started = 0;
  int saslresult = SASL_OK;
  sasl_security_properties_t *secprops=NULL;
  socklen_t addrsize=sizeof(struct sockaddr_storage);
  struct sockaddr_storage saddr_l, saddr_r;
  char localip[60], remoteip[60];

  /* attempt to start sasl */
  if(!sasl_started) {
      saslresult=sasl_client_init(NULL);
      obj->conn = NULL;
      sasl_started = 1;
  }

  /* Save the callbacks array */
  obj->callbacks = callbacks;

  if (saslresult!=SASL_OK) return -1;

  addrsize=sizeof(struct sockaddr_storage);
  if (getpeername(obj->sock,(struct sockaddr *)&saddr_r,&addrsize)!=0)
      return -1;
  
  addrsize=sizeof(struct sockaddr_storage);
  if (getsockname(obj->sock,(struct sockaddr *)&saddr_l,&addrsize)!=0)
      return -1;

  /* set the port manually since getsockname is stupid and doesn't */
  ((struct sockaddr_in *)&saddr_l)->sin_port = htons(obj->port);

  if (iptostring((struct sockaddr *)&saddr_r, addrsize, remoteip, 60))
      return -1;

  if (iptostring((struct sockaddr *)&saddr_l, addrsize, localip, 60))
      return -1;

  if(obj->conn) sasl_dispose(&obj->conn);

  /* client new connection */
  saslresult=sasl_client_new(SIEVE_SERVICE_NAME,
			     obj->serverFQDN,
			     localip, remoteip,
			     callbacks,
			     SASL_SUCCESS_DATA,
			     &obj->conn);

  if (saslresult!=SASL_OK) return -1;

  /* create a security structure and give it to sasl */
  secprops = make_secprops(0, ssf);
  if (secprops != NULL)
  {
    sasl_setprop(obj->conn, SASL_SEC_PROPS, secprops);
    free(secprops);
  }

  return 0;
}

char * read_capability(isieve_t *obj)
{
  lexstate_t state;
  char *cap = NULL;

  obj->version = NEW_VERSION;

  while (yylex(&state,obj->pin)==STRING)
  {
      char *attr = string_DATAPTR(state.str);
      char *val = NULL;

      if (yylex(&state,obj->pin)==' ')
      {
	  if (yylex(&state,obj->pin)!=STRING)
	  {
	      parseerror("STRING");
	  }
	  val = string_DATAPTR(state.str);
	  if (yylex(&state,obj->pin)!=EOL)
	  {
	      parseerror("EOL1");
	  }
      }

      if (strcasecmp(attr,"SASL")==0)
      {
	cap = val;
      } else if (strcasecmp(attr,"SIEVE")==0) {

      } else if (strcasecmp(attr,"IMPLEMENTATION")==0) {

      } else if (strcasecmp(attr,"STARTTLS")==0) {
	  /* TODO */
      } else if (val && strncmp(val,"SASL=",5)==0) {
	  obj->version = OLD_VERSION;
	  cap = (char *) xmalloc(strlen(val));
	  memset(cap, '\0', strlen(val));
	  memcpy(cap, val+6, strlen(val)-7);

	  return cap;
      } else {
	  /* unkown capability */
      }
  }

  if (yylex(&state,obj->pin)!=EOL)
  {
      parseerror("EOL2");
  }
  
  return cap;
}

static int getauthline(isieve_t *obj, char **line, unsigned int *linelen,
		       char **errstrp)
{
  lexstate_t state;
  int res;
  int ret;
  size_t len;
  mystring_t *errstr;
  char *last_send;

  /* now let's see what the server said */
  res=yylex(&state, obj->pin);
  *line = NULL;
  if (res!=STRING)
  {
      ret = handle_response(res,obj->version,
			    obj->pin, &last_send, &errstr);
      
      if (res==TOKEN_OK) {
	  /* Was there a last send from the server? */
	  if(last_send) {
	      /* it's base64 encoded */
	      int last_send_len = strlen(last_send);

	      len = last_send_len*2+1;
	      *line = xmalloc(len);

	      sasl_decode64(last_send, last_send_len,
			    *line, len, linelen);

	      free(last_send);
	  }
	  return STAT_OK;
      } else { /* server said no or bye*/
	  /* xxx handle referrals */
	  *errstrp = string_DATAPTR(errstr);
	  return STAT_NO;
      }
  }

  len = state.str->len*2+1;
  *line=(char *) xmalloc(len);

  sasl_decode64(string_DATAPTR(state.str), state.str->len,
		*line, len, linelen);

  if (yylex(&state, obj->pin)!=EOL)
      return STAT_NO;

  return STAT_CONT;
}


int auth_sasl(char *mechlist, isieve_t *obj, const char **mechusing,
	      char **errstr)
{
  sasl_interact_t *client_interact=NULL;
  int saslresult=SASL_INTERACT;
  const char *out;
  unsigned int outlen;
  char *in;
  unsigned int inlen;
  char inbase64[2048];
  unsigned int inbase64len;

  imt_stat status = STAT_CONT;

  if(!mechlist || !obj || !mechusing) return -1;

  /* call sasl client start */
  while (saslresult==SASL_INTERACT)
  {
    saslresult=sasl_client_start(obj->conn, mechlist,
				 &client_interact,
				 &out, &outlen,
				 mechusing);
    if (saslresult==SASL_INTERACT)
      fillin_interactions(client_interact); /* fill in prompts */      
  }

  if ((saslresult!=SASL_OK) && (saslresult!=SASL_CONTINUE)) return saslresult;

  if (out!=NULL)
  {
    prot_printf(obj->pout,"AUTHENTICATE \"%s\" ",*mechusing);

    sasl_encode64(out, outlen,
		  inbase64, sizeof(inbase64), &inbase64len);

    prot_printf(obj->pout, "{%d+}\r\n",inbase64len);
    prot_write(obj->pout,inbase64,inbase64len);
    prot_printf(obj->pout,"\r\n");
  } else {
    prot_printf(obj->pout,"AUTHENTICATE \"%s\"\r\n",*mechusing);
  }
  prot_flush(obj->pout);

  inlen = 0;

  /* get reply */
  status=getauthline(obj,&in,&inlen, errstr);

  while (status==STAT_CONT)
  {
    saslresult=SASL_INTERACT;
    while (saslresult==SASL_INTERACT)
    {
      saslresult=sasl_client_step(obj->conn,
				  in,
				  inlen,
				  &client_interact,
				  &out,&outlen);

      if (saslresult==SASL_INTERACT)
	fillin_interactions(client_interact); /* fill in prompts */
    }

    /* check if sasl suceeded */
    if (saslresult<SASL_OK)
    {
	/* send cancel notice */
	prot_printf(obj->pout, "*\r\n");
	prot_flush(obj->pout);

	/* eat the auth line that confirms that we canceled */
	if(getauthline(obj,&in,&inlen,errstr) != STAT_NO) {
	    *errstr = strdup("protocol error");
	} else {
	    *errstr = strdup(sasl_errstring(saslresult,NULL,NULL));
	}
	
	return saslresult;
    }

    /* send to server */

    sasl_encode64(out, outlen,
		  inbase64, sizeof(inbase64), &inbase64len);

    prot_printf(obj->pout, "{%d+}\r\n",inbase64len);
    prot_flush(obj->pout);
    prot_write(obj->pout,inbase64,inbase64len);
    prot_flush(obj->pout);
    prot_printf(obj->pout,"\r\n");
    prot_flush(obj->pout);

    /* get reply */
    status=getauthline(obj,&in,&inlen, errstr);
  }

  if(status == STAT_OK) {
      /* do we have a last send? */
      if(in) {
	  saslresult=sasl_client_step(obj->conn,
				      in,
				      inlen,
				      &client_interact,
				      &out, &outlen);
	  
	  if(saslresult != SASL_OK)
	      return -1;
      }

      /* turn on layer if need be */
      prot_setsasl(obj->pin,  obj->conn);
      prot_setsasl(obj->pout, obj->conn);

      /* There wasn't a last send, or we are already OK */
      return 0;
  } else {
      /* Error */
      return -1;
  }
}

static int refer_simple_cb(void *context, int id, const char **result,
			    unsigned int *len)
{
    if (!result) {
	return SASL_BADPARAM;
    }

    switch (id) {
    case SASL_CB_USER:
	*result = (char *) context;
	break;
    case SASL_CB_AUTHNAME:
	*result = (char *) context;
	break;
    default:
	return SASL_BADPARAM;
    }
    if (len) {
	*len = *result ? strlen(*result) : 0;
    }

    return SASL_OK;
}

int do_referral(isieve_t *obj, char *refer_to) 
{
    int ret;
    struct servent *serv;
    isieve_t *obj_new;
    char *mechlist;
    int port;
    char *errstr;
    const char *mtried;
    const char *scheme = "sieve://";
    char *host, *p;
    sasl_callback_t *callbacks;

    /* check scheme */
    if (strncasecmp(refer_to, scheme, strlen(scheme)))
	return STAT_NO;

    /* get host */
    if ((host = strrchr(refer_to, '@'))) {
	char *authid, *userid;
	int n;

	*host++ = '\0';

	/* get authid - make a copy so it persists for the callbacks */
	authid = obj->refer_authinfo = xstrdup(refer_to + strlen(scheme));

	/* get userid */
	if ((userid = strrchr(authid, ';')))
	    *userid++ = '\0';

	/* count the callbacks */
	for (n = 0; obj->callbacks[n++].id != SASL_CB_LIST_END;);

	/* copy the callbacks, substituting some of our own */
	callbacks = obj->refer_callbacks = xmalloc(n*sizeof(sasl_callback_t));

	while (--n >= 0) {
	    callbacks[n].id = obj->callbacks[n].id;

	    switch (callbacks[n].id) {
	    case SASL_CB_USER:
		callbacks[n].proc = &refer_simple_cb;
		callbacks[n].context = userid ? userid : authid;
		break;
	    case SASL_CB_AUTHNAME:
		callbacks[n].proc = &refer_simple_cb;
		callbacks[n].context = authid;
		break;
	    default:
		callbacks[n].proc = obj->callbacks[n].proc;
		callbacks[n].context = obj->callbacks[n].context;
		break;
	    }
	}
    }
    else {
	host = refer_to + strlen(scheme);
	callbacks = obj->callbacks;
    }

    /* get port */
    p = host;
    if (*host == '[') {
	if ((p = strrchr(host + 1, ']')) != NULL) {
	    *p++ = '\0';
	    host++;			/* skip first bracket */
        } else
	    p = host;
    }
    if ((p = strchr(p, ':'))) {
	*p++ = '\0';
	port = atoi(p);
    } else {
	serv = getservbyname("sieve", "tcp");
	if (serv == NULL) {
	    port = 2000;
	} else {
	    port = ntohs(serv->s_port);
	}
    }

    ret = init_net(host, port, &obj_new);
    if(ret) return STAT_NO;

    /* Start up SASL */
    ret = init_sasl(obj_new, 128, callbacks);
    if(ret) return STAT_NO;

    /* Authenticate */
    mechlist = read_capability(obj_new);

    do {
	mtried = NULL;
	ret = auth_sasl(mechlist, obj_new, &mtried, &errstr);
	if(ret) init_sasl(obj_new, 128, callbacks);

	if(mtried) {
	    char *newlist = (char*) xmalloc(strlen(mechlist)+1);
	    char *mtr = (char*) xstrdup(mtried);
	    char *tmp;
	    
	    ucase(mtr);
	    tmp = strstr(mechlist,mtr);
	    *tmp ='\0';
	    strcpy(newlist, mechlist);
	    tmp++;
	    
	    tmp = strchr(tmp,' ');
	    if (tmp) {
		strcat(newlist,tmp);
	    }
	    
	    free(mtr);
	    free(mechlist);
	    mechlist = newlist;
	}
    } while(ret && mtried);

    /* xxx leak? */
    if(ret) return STAT_NO;

    /* free old isieve_t */
    sieve_dispose(obj);

    /* Copy new isieve_t into memory used by old object */
    memcpy(obj,obj_new,sizeof(isieve_t));
    free(obj_new);

    /* Destroy the string that was allocated to save the destination server */
    free(refer_to);

    return STAT_OK;
}

int isieve_logout(isieve_t **obj) 
{
    prot_printf((*obj)->pout, "LOGOUT");
    prot_flush((*obj)->pout);

    close((*obj)->sock);
    
    sieve_free_net(*obj);
    *obj = NULL;

    return STAT_OK;
}

int isieve_put_file(isieve_t *obj, char *filename, char *destname,
		    char **errstr)
{
    char *refer_to;
    int ret = installafile(obj->version,
			   obj->pout, obj->pin,
			   filename, destname,
			   &refer_to, errstr);
    if(ret == -2 && refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_put_file(obj, filename, destname, errstr);
	} else {
	    *errstr = "referral failed";
	}
    }
    return ret;
}

int isieve_put(isieve_t *obj, char *name, char *data, int len, char **errstr)
{
    char *refer_to;
    int ret = installdata(obj->version,
			  obj->pout, obj->pin,
			  name, data, len, &refer_to, errstr);
    if(ret == -2 && refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_put(obj, name, data, len, errstr);
	} else {
	    *errstr = "referral failed";
	}
    }
    return ret;
}

int isieve_delete(isieve_t *obj, char *name, char **errstr)
{
    char *refer_to;
    int ret = deleteascript(obj->version,
			    obj->pout, obj->pin,
			    name, &refer_to, errstr);
    if(ret == -2 && refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_delete(obj, name, errstr);
	} else {
	    *errstr = "referral failed";
	}
    }
    return ret;
}

int isieve_list(isieve_t *obj, isieve_listcb_t *cb,void *rock, char **errstr)
{
    char *refer_to;
    int ret = list_wcb(obj->version, obj->pout, obj->pin, cb, rock, &refer_to);
    if(ret == -2 && refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_list(obj, cb, rock, errstr);
	}
    }
    return ret;    
}

int isieve_activate(isieve_t *obj, char *name, char **errstr)
{
    char *refer_to;
    int ret = setscriptactive(obj->version,obj->pout, obj->pin, name,
			      &refer_to, errstr);
    if(ret == -2 && refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_activate(obj, name, errstr);
	} else {
	    *errstr = "referral failed";
	}
    }
    return ret;
}

int isieve_get(isieve_t *obj,char *name, char **output, char **errstr)
{
    int ret;
    char *refer_to;
    mystring_t *mystr = NULL;

    ret = getscriptvalue(obj->version,obj->pout, obj->pin,
			 name, &mystr, &refer_to, errstr);

    if(ret == -2 && *refer_to) {
	ret = do_referral(obj, refer_to);
	if(ret == STAT_OK) {
	    ret = isieve_get(obj,name,output,errstr);
	    return ret;
	} else {
	    *errstr = "referral failed";
	}
    }

    *output = string_DATAPTR(mystr);

    return ret;
}
