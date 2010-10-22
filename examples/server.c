/* coap -- simple implementation of the Constrained Application Protocol (CoAP)
 *         as defined in draft-ietf-core-coap-01
 *
 * Copyright (C) 2010 Olaf Bergmann <bergmann@tzi.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#include "subscribe.h"
#include "coap.h"

#define COAP_RESOURCE_CHECK_TIME 2

/* where accessible files are stored */
#define FILE_PREFIX "filestorage"
#define DATASINK_PREFIX "data-sink/"

#define GENERATE_PDU(var,t,c,i) {		\
    var = coap_new_pdu();			\
    if (var) {					\
      var->hdr->type = (t);			\
      var->hdr->code = (c);			\
      var->hdr->id = (i);			\
    }						\
  }

/* temporary storage for dynamic resource representations */
static char resource_buf[20000]; 
static int quit = 0;

/* SIGINT handler: set quit to 1 for graceful termination */
void
handle_sigint(int signum) {
  quit = 1;
}

coap_pdu_t *
new_ack( coap_context_t  *ctx, coap_queue_t *node ) {
  coap_pdu_t *pdu;
  GENERATE_PDU(pdu,COAP_MESSAGE_ACK,0,node->pdu->hdr->id);
  return pdu;
}

coap_pdu_t *
new_rst( coap_context_t  *ctx, coap_queue_t *node, unsigned int code ) {
  coap_pdu_t *pdu;
  GENERATE_PDU(pdu,COAP_MESSAGE_RST,code,node->pdu->hdr->id);
  return pdu;
}

coap_pdu_t *
new_response( coap_context_t  *ctx, coap_queue_t *node, unsigned int code ) {
  coap_pdu_t *pdu;
  GENERATE_PDU(pdu,COAP_MESSAGE_ACK,code,node->pdu->hdr->id);
  return pdu;
}

void
add_contents( coap_pdu_t *pdu, unsigned int mediatype, unsigned int len, unsigned char *data ) {
  unsigned char ct = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
  if (!pdu)
    return;
  
  /* add content-encoding */
  coap_add_option(pdu, COAP_OPTION_CONTENT_TYPE, 1, &ct);

  /* TODO: handle fragmentation (check result code) */
  coap_add_data(pdu, len, data);
}

#define INDEX "Hi there!"

coap_opt_t *
coap_next_option(coap_pdu_t *pdu, coap_opt_t *opt) {
  coap_opt_t *next;
  if ( !pdu || !opt )
    return NULL;

  next = (coap_opt_t *)( (unsigned char *)opt + COAP_OPT_SIZE(*opt) );
  return (unsigned char *)next < pdu->data && COAP_OPT_DELTA(*next) == 0 ? next : NULL;
}

int
mediatype_matches(coap_pdu_t *pdu, unsigned char mediatype) {
  coap_opt_t *accept;
  int t;

  if ( mediatype == COAP_MEDIATYPE_ANY ||
       (accept = coap_check_option(pdu, COAP_OPTION_ACCEPT)) == NULL)
    return 1;

  /* Check the byte sequence in the option value for any occurence of
   * mediatype. */
  for (t = 0; t < COAP_OPT_LENGTH(*accept); ++t) {
    debug("check type: %d == %d\n",COAP_OPT_VALUE(*accept)[t],mediatype);
    if ( COAP_OPT_VALUE(*accept)[t] == mediatype )
      return 1;
  }
  
  return 0;
}

/* Check if provided path name is a valid CoAP URI path. */
int 
is_valid(char *prefix, unsigned char *path, unsigned int length) {
  enum { START, PATH, DOT, DOTDOT } state; 

  if (!path || length < strlen(prefix) ||
      strncmp((char *)path, prefix, strlen(prefix)) != 0)
    return 0;
  
  path += strlen(prefix);
  length -= strlen(prefix);
  if ( length && *path == '/' ) {
    state = START;
    ++path;
    --length;
  } else 
    state = PATH;

  while (length) {
    switch (state) {
      case START:
	switch (path[0]) {
	case '.': state = DOT; break;
	case '/': return 0;
	default: state = PATH;
	}
	break;
      case PATH:
	if (path[0] == '/')
	  state = START;
	break;
      case DOT:
	switch (path[0]) {
	case '.': state = DOTDOT; break;
	case '/': return 0;
	default: state = PATH;
	}
	break;
      case DOTDOT:
	if (path[0] == '/')
	  return 0;
	state = PATH;
	break;
      }
    ++path;
    --length;
  }

  return state != DOT && state != DOTDOT;
}

coap_pdu_t *
handle_get(coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_pdu_t *pdu;
  coap_uri_t uri;
  coap_resource_t *resource;
  coap_opt_t *block, *tok, *sub;
  str token;
  unsigned int blklen, blk;
  int code, finished = 1;
  unsigned int ls;
  unsigned int duration;
  unsigned char enc;
  unsigned char mediatype = COAP_MEDIATYPE_ANY;
  coap_subscription_t *subscription;
  static unsigned char buf[COAP_MAX_PDU_SIZE];
  static unsigned char optbuf[4];

  if ( !coap_get_request_uri( node->pdu, &uri ) )
    return NULL;

  if ( !uri.path.length ) {
    pdu = new_response(ctx, node, COAP_RESPONSE_200);
    if ( !pdu )
      return NULL;
   
    add_contents( pdu, COAP_MEDIATYPE_TEXT_PLAIN, sizeof(INDEX) - 1, (unsigned char *)INDEX );
    goto ok;
  }

  /* any other resource */
  resource = coap_get_resource(ctx, &uri);
  if ( !resource )
    return new_response(ctx, node, COAP_RESPONSE_404);

  /* check if requested mediatypes match */
  if ( coap_check_option(node->pdu, COAP_OPTION_ACCEPT) 
       && !mediatype_matches(node->pdu, resource->mediatype) ) {
    debug("media type mismatch\n");
    return new_response(ctx, node, COAP_RESPONSE_415);
  }

  block = coap_check_option(node->pdu, COAP_OPTION_BLOCK);
  if ( block ) {
    blk = coap_decode_var_bytes(COAP_OPT_VALUE(*block), 
				COAP_OPT_LENGTH(*block));
    blklen = 16 << (blk & 0x07);
  } else {
    blklen = 512; /* default block size is set to 512 Bytes locally */
    blk = coap_fls(blklen >> 4) - 1;
  }

  /* invoke callback function to get data representation of requested
     resource */
  if ( resource->data ) {
    mediatype = resource->mediatype;
    
    code = resource->data(&uri, &mediatype, 
			  (blk & ~0x0f) << (blk & 0x07), buf, &blklen, 
			  &finished);
  } else {			
    /* no callback available, set code, blklen and finished manually
       (-> empty payload) */
    code = COAP_RESPONSE_200;
    blklen = 0;
    finished = 1;
  }

  if ( !(pdu = new_response(ctx, node, code)) )
    return NULL;
  
  if ( blklen > 0 ) { 
    /* add content-type */
    if ( mediatype != COAP_MEDIATYPE_ANY ) 
      coap_add_option(pdu, COAP_OPTION_CONTENT_TYPE, 1, &mediatype);

    /* handle subscription if requested */
    sub = coap_check_option( node->pdu, COAP_OPTION_SUBSCRIPTION );
    if ( sub ) {
      duration = COAP_PSEUDOFP_DECODE_8_4(*COAP_OPT_VALUE(*sub));
      debug("*** add subscription for %d seconds\n", duration);
      enc = COAP_PSEUDOFP_ENCODE_8_4_DOWN(duration, ls);
      coap_add_option(pdu, COAP_OPTION_SUBSCRIPTION, 1, &enc);      
      
      /* refresh only if already subscribed */
      token.length = 0;
      tok = coap_check_option(node->pdu, COAP_OPTION_TOKEN);
      if (tok) {
	COAP_SET_STR(&token, COAP_OPT_LENGTH(*tok), COAP_OPT_VALUE(*tok));
	coap_add_option(pdu, COAP_OPTION_TOKEN, 
			COAP_OPT_LENGTH(*tok),
			COAP_OPT_VALUE(*tok));
      }

      subscription = 
	coap_find_subscription(ctx, coap_uri_hash(&uri), &(node->remote), 
			       tok ? &token : NULL);

      if (subscription) {	/* refresh existing subscription */
	subscription->expires = time(NULL)+duration;
      } else {			/* add new subscription */
	subscription = coap_new_subscription(ctx, &uri, &(node->remote), 
					     time(NULL)+duration);
	if (subscription) {
	  if (token.length) {
	    /* TODO: copy token into subscription->token */
	    subscription->token.s = (unsigned char *)coap_malloc(token.length);
	    if (subscription->token.s) {
	      subscription->token.length = token.length;
	      memcpy(subscription->token.s, token.s, token.length);
	    }
	    /* FIXME: else error? */
	  }
	  coap_add_subscription(ctx, subscription);
	}
      }
    }

    /* add a block option when it has been requested explicitly or
     * there is more data available */
    if ( block || !finished ) {
      blk = (blk & ~0x08) | (!finished << 3);
      /* add block option to PDU */
      coap_add_option(pdu, COAP_OPTION_BLOCK, 
		      coap_encode_var_bytes(optbuf, blk), optbuf);
    }

    /* We will add contents only when it is not empty. This might lead
     * to problems when this is the last block of a sequence of more
     * than one block. For now, we ignore this problem as it can
     * happen only when the block sizes have changed.
     */
    if (!coap_add_data(pdu, blklen, buf)) {
      /* FIXME: handle this case -- must send 500 or something */
    }
  }

 ok:
  return pdu;
}

int 
write_file(char *filename, unsigned char *text, int length) {
  FILE *file;
  ssize_t written;

  file = fopen(filename, "w");
  if ( !file ) {
    perror("write_file: fopen");
    return 0;
  }

  written = fwrite(text, 1, length, file);
  fclose(file);

  return written;
}

coap_pdu_t *
handle_put(coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_uri_t uri;
  coap_opt_t *tok;
  coap_pdu_t *pdu;
  coap_resource_t *resource;
  ssize_t written, length;
  static char filename[FILENAME_MAX+1];

  if ( !coap_get_request_uri( node->pdu, &uri ) )
    return NULL;

  /* we do not want to create the resource if not available */
  if ( !(resource = coap_get_resource(ctx, &uri)) )
    return new_response(ctx, node, COAP_RESPONSE_404);

  if (!resource->writable)
    return new_response(ctx, node, COAP_RESPONSE_400);

  if ( !(pdu = new_response(ctx, node, COAP_RESPONSE_200)) )
    return NULL;
  
  /* create a zero-terminated string */
  memcpy(filename, uri.path.s, uri.path.length);
  filename[uri.path.length] = '\0';

  length = (unsigned char *)node->pdu->hdr + node->pdu->length - node->pdu->data;
  written = write_file(filename, node->pdu->data, length);
  
  if (written < length)
    return new_response(ctx, node, COAP_RESPONSE_500);

  tok = coap_check_option(node->pdu, COAP_OPTION_CONTENT_TYPE);
  resource->mediatype = tok ? *COAP_OPT_VALUE(*tok) : COAP_MEDIATYPE_ANY;
  resource->dirty = 1;		/* mark for notification of observers */
  return pdu;
}

int 
resource_from_file(coap_uri_t *uri, 
		   unsigned char *mediatype, unsigned int offset, 
		   unsigned char *buf, unsigned int *buflen,
		   int *finished);

coap_pdu_t *
handle_post(coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_uri_t uri;
  coap_pdu_t *pdu;
  coap_resource_t *r;
  coap_opt_t *tok;
  ssize_t written, length;
  int namelen;
  char name[60];

  if ( !coap_get_request_uri( node->pdu, &uri ) )
    return NULL;

  /* existing stuff can be handled using put for now */
  if (coap_get_resource(ctx, &uri))
    return handle_put(ctx, node, data);

  /* create new resource */
  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return new_response(ctx, node, COAP_RESPONSE_500);

  tok = coap_check_option(node->pdu, COAP_OPTION_CONTENT_TYPE);

  /* Create a new resource to store the given contents in the local
   * file system. We restrict the storage area to DATASINK_PREFIX.
   */
  memset(name, 0, sizeof(name));
  namelen = snprintf(name, sizeof(name)-1, DATASINK_PREFIX "%lu", 
		     coap_uri_hash(&uri));
  
  r->uri = coap_new_uri((unsigned char *)name, namelen);
  r->mediatype = tok ? *COAP_OPT_VALUE(*tok) : COAP_MEDIATYPE_ANY;
  r->dirty = 1;
  r->writable = 1;
  r->data = resource_from_file;

  /* we know that r->uri.s is zero-terminated */
  length = 
    (unsigned char *)node->pdu->hdr + node->pdu->length - node->pdu->data;
  written = write_file((char *)r->uri->path.s, node->pdu->data, length);
  
  if (written < length) {
    coap_free(r);
    return new_response(ctx, node, COAP_RESPONSE_500);
  } else
    coap_add_resource(ctx, r);
  
  /* create the response */
  pdu = new_response(ctx, node, COAP_RESPONSE_201);

  /* add location header */
  coap_add_option(pdu, COAP_OPTION_LOCATION, 
		  namelen, (unsigned char *)name);

  /* we do not need the request URI, only a token, if specified */

  tok = coap_check_option(node->pdu, COAP_OPTION_TOKEN);
  if (tok) 
    coap_add_option(pdu, COAP_OPTION_TOKEN, 
		    COAP_OPT_LENGTH(*tok), COAP_OPT_VALUE(*tok));

  return pdu;
}

coap_pdu_t *
handle_delete(coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_uri_t uri;
  coap_pdu_t *pdu;
  coap_resource_t *r;
  coap_opt_t *tok;
  static char filename[FILENAME_MAX+1];

  if ( !coap_get_request_uri( node->pdu, &uri ) )
    return NULL;

  /* TODO: send redirect with new URI when path.s == "data-sink" */

  if ( !is_valid(DATASINK_PREFIX, uri.path.s, uri.path.length) )
    return new_response(ctx, node, COAP_RESPONSE_400);

  /* existing stuff can be handled using put for now */
  r = coap_get_resource(ctx, &uri);
  if (!r)
    return new_response(ctx, node, COAP_RESPONSE_404);
  
  if (!r->writable)
    return new_response(ctx, node, COAP_RESPONSE_400);

  if (FILENAME_MAX < uri.path.length)
    return new_response(ctx, node, COAP_RESPONSE_500);
    
  memcpy(filename, uri.path.s, uri.path.length);
  filename[uri.path.length] = '\0';

  debug("unlink %s\n", filename);
  unlink(filename);

  /* create the response */  
  pdu = new_response(ctx, node, COAP_RESPONSE_200);

  if (uri.scheme.length)
    coap_add_option(pdu, COAP_OPTION_URI_SCHEME, 
		    uri.scheme.length, uri.scheme.s);

  if (uri.na.length)
    coap_add_option(pdu, COAP_OPTION_URI_AUTHORITY, 
		    uri.na.length, uri.na.s);

  if (uri.path.length)
    coap_add_option(pdu, COAP_OPTION_URI_PATH, 
		    uri.path.length, uri.path.s);
    
  tok = coap_check_option(node->pdu, COAP_OPTION_TOKEN);
  if (tok) 
    coap_add_option(pdu, COAP_OPTION_TOKEN, 
		    COAP_OPT_LENGTH(*tok), COAP_OPT_VALUE(*tok));

  coap_delete_resource(ctx, coap_uri_hash(&uri));

  return pdu;
}

void 
message_handler(coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_pdu_t *pdu = NULL;

#ifndef NDEBUG
  debug("** process pdu: ");
  coap_show_pdu( node->pdu );
#endif

  if ( node->pdu->hdr->version != COAP_DEFAULT_VERSION ) {
    debug("dropped packet with unknown version %u\n", node->pdu->hdr->version);
    return;
  }
    
  switch (node->pdu->hdr->code) {
  case COAP_REQUEST_GET :
    pdu = handle_get(ctx, node, data);

    if ( !pdu && node->pdu->hdr->type == COAP_MESSAGE_CON )
      pdu = new_rst( ctx, node, COAP_RESPONSE_500 );
    break;
  case COAP_REQUEST_PUT:
    pdu = handle_put(ctx, node, data);
    if ( !pdu && node->pdu->hdr->type == COAP_MESSAGE_CON )
      pdu = new_response( ctx, node, COAP_RESPONSE_400 );
    break;
  case COAP_REQUEST_POST:
    pdu = handle_post(ctx, node, data);
    if ( !pdu && node->pdu->hdr->type == COAP_MESSAGE_CON )
      pdu = new_response( ctx, node, COAP_RESPONSE_400 );
    break;
  case COAP_REQUEST_DELETE:
    pdu = handle_delete(ctx, node, data);
    if ( !pdu && node->pdu->hdr->type == COAP_MESSAGE_CON )
      pdu = new_response( ctx, node, COAP_RESPONSE_400 );
    break;
  default:
    if ( node->pdu->hdr->type == COAP_MESSAGE_CON ) {
      if ( node->pdu->hdr->code >= COAP_RESPONSE_100 )
	pdu = new_rst( ctx, node, COAP_RESPONSE_500 );
      else {
	debug("request method not implemented: %u\n", node->pdu->hdr->code);
	pdu = new_rst( ctx, node, COAP_RESPONSE_405 );
      }
    }
  }

  if ( pdu && coap_send( ctx, &node->remote, pdu ) == COAP_INVALID_TID ) {
    debug("message_handler: error sending reponse");
    coap_delete_pdu(pdu);
  }

}

void 
usage( const char *program, const char *version) {
  const char *p;

  p = strrchr( program, '/' );
  if ( p )
    program = ++p;

  fprintf( stderr, "%s v%s -- a small CoAP implementation\n"
	   "(c) 2010 Olaf Bergmann <bergmann@tzi.org>\n\n"
	   "usage: %s [-g group] [-p port] URI\n\n"
	   "\tURI can be an absolute or relative coap URI,\n"
	   "\t-g group\tjoin the given multicast group\n"
	   "\t-p port\t\tlisten on specified port\n",
	   program, version, program );
}

int
join( coap_context_t *ctx, char *group_name ){
  struct ipv6_mreq mreq;
  struct addrinfo   *reslocal = NULL, *resmulti = NULL, hints, *ainfo;
  int result = -1;

  /* we have to resolve the link-local interface to get the interface id */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  result = getaddrinfo("::", NULL, &hints, &reslocal);
  if ( result < 0 ) {
    perror("join: cannot resolve link-local interface");
    goto finish;
  }

  /* get the first suitable interface identifier */
  for (ainfo = reslocal; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_interface = 
	      ((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_scope_id;
      break;
    }
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  /* resolve the multicast group address */
  result = getaddrinfo(group_name, NULL, &hints, &resmulti);

  if ( result < 0 ) {
    perror("join: cannot resolve multicast address");
    goto finish;
  }

  for (ainfo = resmulti; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_multiaddr = 
	((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_addr;
      break;
    }
  }
  
  result = setsockopt( ctx->sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
		       (char *)&mreq, sizeof(mreq) );
  if ( result < 0 ) 
    perror("join: setsockopt");

 finish:
  freeaddrinfo(resmulti);
  freeaddrinfo(reslocal);
  
  return result;
}

int 
resource_wellknown(coap_uri_t *uri, 
		   unsigned char *mediatype, unsigned int offset, 
		   unsigned char *buf, unsigned int *buflen,
		   int *finished) {
  static unsigned char resources[] = 
    "</lipsum>;ct=1;n=\"some large text to test buffer sizes (<EOT> marks its end)\","
    "</time>;ct=0,1,41;n=\"server's local time and date\","
    "</filestorage>;n=\"you can PUT things here\","
    "</data-sink/>;n=\"POST files here or DELETE them when created\"";
  size_t maxlen = sizeof(resources) - 1;
  
  *finished = 1;

  switch (*mediatype) {
  case COAP_MEDIATYPE_ANY :
  case COAP_MEDIATYPE_APPLICATION_LINK_FORMAT :
    *mediatype = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
    break;
  default :
    *buflen = 0;    
    return COAP_RESPONSE_415;
  }

  if ( offset > maxlen ) {
    *buflen = 0;
    return COAP_RESPONSE_400;
  } else if ( offset + *buflen > maxlen )
    *buflen = maxlen - offset;

  memcpy(buf, resources + offset, *buflen);

  *finished = offset + *buflen == maxlen;
  return COAP_RESPONSE_200;
}

int 
resource_time(coap_uri_t *uri, 
	      unsigned char *mediatype, unsigned int offset, 
	      unsigned char *buf, unsigned int *buflen,
	      int *finished) {
  static unsigned char b[400];
  size_t maxlen;
  time_t now;
  struct tm *tlocal;

  time(&now);
  tlocal = localtime(&now);

  *finished = 1;

  if ( !tlocal ) {
    *buflen = 0;
    return COAP_RESPONSE_500;
  }

  switch (*mediatype) {
  case COAP_MEDIATYPE_ANY :
  case COAP_MEDIATYPE_TEXT_PLAIN :
    *mediatype = COAP_MEDIATYPE_TEXT_PLAIN;
    maxlen = strftime(resource_buf, sizeof(b), "%b %d %H:%M:%S", tlocal);
    break;
  case COAP_MEDIATYPE_TEXT_XML :
  case COAP_MEDIATYPE_APPLICATION_XML :
    maxlen = strftime(resource_buf, sizeof(b), "<datetime>\n  <date>%Y-%m-%d</date>\n  <time>%H:%M:%S</time>\n  </tz>%S</tz>\n</datetime>", tlocal);
    break;
  default :
    *buflen = 0;
    return COAP_RESPONSE_415;
  }

  if ( offset > maxlen ) {
    *buflen = 0;
    return COAP_RESPONSE_400;
  } else if ( offset + *buflen > maxlen )
    *buflen = maxlen - offset;

  memcpy(buf, resource_buf + offset, *buflen);

  *finished =offset + *buflen == maxlen;
  return COAP_RESPONSE_200;
}

int 
resource_lipsum(coap_uri_t *uri, 
		unsigned char *mediatype, unsigned int offset, 
		unsigned char *buf, unsigned int *buflen,
		int *finished) {
  static unsigned char verylargebuf[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Mauris fermentum, lacus elementum venenatis aliquet, tortor risus laoreet sapien, a vulputate libero dolor ut odio. Vivamus congue elementum fringilla. Suspendisse porttitor, lectus sed gravida volutpat, dolor magna gravida massa, id fermentum lectus mi quis erat. Suspendisse lacinia, libero in euismod bibendum, magna nisi tempus lacus, eu suscipit augue nisi vel nulla. Praesent gravida lacus nec elit vestibulum sit amet rhoncus dui fringilla. Quisque diam lacus, ullamcorper non consectetur vitae, pellentesque eget lectus. Vestibulum velit nulla, venenatis vel mattis at, scelerisque nec mauris. Nulla facilisi. Mauris vel erat mi. Morbi et nulla nibh, vitae cursus eros. In convallis, magna egestas dictum porttitor, diam magna sagittis nisi, rhoncus tincidunt ligula felis sed mauris. Pellentesque pulvinar ante id velit convallis in porttitor justo imperdiet. Curabitur viverra placerat tincidunt. Vestibulum justo lacus, sollicitudin in facilisis vel, tempus nec erat. Duis varius viverra aliquet. In tempor varius elit vel pharetra. Sed mattis, quam in pulvinar ullamcorper, est ipsum tempor dui, at fringilla magna sem in sapien. Phasellus sollicitudin ornare sem, nec porta libero tempus vitae. Maecenas posuere pulvinar dictum. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Cras eros mauris, pulvinar tempor facilisis ut, condimentum in magna. Nullam eget ipsum sit amet lacus massa nunc.<EOT>";
  unsigned int maxlen = sizeof(verylargebuf) - 1;

  switch (*mediatype) {
  case COAP_MEDIATYPE_ANY :
  case COAP_MEDIATYPE_TEXT_PLAIN :
    *mediatype = COAP_MEDIATYPE_TEXT_PLAIN;
    break;
  default :
    *buflen = 0;
    *finished = 1;
    return COAP_RESPONSE_415;
  }

  if ( offset > maxlen ) {
    *buflen = 0;
    return COAP_RESPONSE_400;
  } else if ( offset + *buflen > maxlen )
    *buflen = maxlen - offset;

  memcpy(buf, verylargebuf + offset, *buflen);

  *finished = offset + *buflen == maxlen;
  return COAP_RESPONSE_200;
}

int 
_resource_from_dir(char *filename, 
		   unsigned char *mediatype, unsigned int offset, 
		   unsigned char *buf, unsigned int *buflen,
		   int *finished) {
  DIR *dir;
  struct dirent *dirent;
  size_t namelen, overhead;
  int pos = 0;

  *finished = 1;

  switch (*mediatype) {
  case COAP_MEDIATYPE_ANY :
  case COAP_MEDIATYPE_APPLICATION_LINK_FORMAT:
    *mediatype = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
    break;
  /* case COAP_MEDIATYPE_TEXT_PLAIN: */
  /*   break; */
  default:
    *buflen = 0;
    return COAP_RESPONSE_415;
  }

  if ( (dir = opendir(filename)) == NULL ) {
    perror("_resource_from_dir: opendir");
    *buflen = 0;
    return COAP_RESPONSE_404;
  }

  overhead = strlen(filename) + 10;

  errno = 0;
  while ( (dirent = readdir(dir)) ) {
    namelen = strlen(dirent->d_name);

    /* skip '.' and '..' as they are not allowed in CoAP URIs */
    if ( dirent->d_name[0] == '.' ) {
      if ( namelen == 1 || (namelen == 2 && dirent->d_name[1] == '.') )
	continue;
    }

    if (pos + overhead + namelen * 2 > sizeof(resource_buf) - 1) 
      break;			/* broken */

    if ( pos + overhead + namelen * 2 < offset) {
      offset -= overhead + namelen * 2;
    } else {
      pos += sprintf(resource_buf + pos, "</%s/%s>;n=\"%s\",",
		     filename, dirent->d_name, dirent->d_name);
    }
    
    if ( pos > offset + *buflen ) 
      break;
  }
  
  if (errno != 0)
    goto error;

  closedir(dir);

  if ( pos <= offset ) {
    *buflen = 0;
    return COAP_RESPONSE_400;
  }

  if ( (offset < pos) && (pos <= offset + *buflen) ) {
    *buflen = pos - offset - 1;
    *finished = 1;
  } else 
    *finished = 0;

  memcpy(buf, resource_buf + offset, *buflen);

  return COAP_RESPONSE_200;

 error:
  perror("_resource_from_dir: readdir");
  closedir(dir);

  return COAP_RESPONSE_500;
}

int 
resource_from_file(coap_uri_t *uri, 
		   unsigned char *mediatype, unsigned int offset, 
		   unsigned char *buf, unsigned int *buflen,
		   int *finished) {
  static char filename[FILENAME_MAX+1];
  struct stat statbuf;
  FILE *file;
  int code = COAP_RESPONSE_500;	/* result code */

  if (uri) {
    memcpy(filename, uri->path.s, uri->path.length);
    filename[uri->path.length] = '\0';

    if (!is_valid("", (unsigned char *)filename, uri->path.length) ) {
      fprintf(stderr, "dropped invalid URI '%s'\n", filename);
      code = COAP_RESPONSE_404;
      goto error;
    }
  } else {
    fprintf(stderr, "dropped NULL URI\n");
    code = COAP_RESPONSE_404;
    goto error;
  }

  if (stat(filename, &statbuf) < 0) {
    perror("resource_from_file: stat");
    code = COAP_RESPONSE_404;
    goto error;
  }

  if ( S_ISDIR(statbuf.st_mode) ) {
    /* handle directory if mediatype allows */
    return _resource_from_dir(filename, mediatype, offset, buf, buflen, finished);
  }

  if ( !S_ISREG(statbuf.st_mode) ) {
    fprintf(stderr,"%s not a regular file, skipped\n", filename);
    code = COAP_RESPONSE_404;
    goto error;
  }
  
  if ( offset > statbuf.st_size ) {
    code = COAP_RESPONSE_400;
    goto error;
  } else if ( offset + *buflen > statbuf.st_size )
    *buflen = statbuf.st_size - offset;

  file = fopen(filename, "r");
  if ( !file ) {
    perror("resource_from_file: fopen");
    code = COAP_RESPONSE_500;
    goto error;
  }

  if ( fseek(file, offset, SEEK_SET) < 0 ) {
    perror("resource_from_file: fseek");
    code = COAP_RESPONSE_500;
    goto error;
  }
    
  *buflen = fread(buf, 1, *buflen, file);
  fclose(file);
  
  *finished = offset + *buflen >= statbuf.st_size;

  return COAP_RESPONSE_200;

 error:  
  *buflen = 0;    
  *finished = 1;
  return code;
}

void 
init_resources(coap_context_t *ctx) {
  coap_resource_t *r;

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *)"/" COAP_DEFAULT_URI_WELLKNOWN,
			sizeof(COAP_DEFAULT_URI_WELLKNOWN));
  r->mediatype = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
  r->dirty = 0;
  r->writable = 0;
  r->data = resource_wellknown;
  coap_add_resource( ctx, r );

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *) "/lipsum",
			sizeof("/lipsum") - 1);
  r->mediatype = COAP_MEDIATYPE_TEXT_PLAIN;
  r->dirty = 1;
  r->writable = 0;
  r->data = resource_lipsum;
  coap_add_resource( ctx, r );

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *) "/time",
			sizeof("/time") - 1);
  r->mediatype = COAP_MEDIATYPE_ANY;
  r->dirty = 0;
  r->writable = 0;
  r->data = resource_time;
  coap_add_resource( ctx, r );

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *) "/filestorage",
			sizeof("/filestorage") - 1);
  r->mediatype = COAP_MEDIATYPE_ANY;
  r->dirty = 0;
  r->writable = 1;
  r->data = resource_from_file;
  write_file("filestorage",(unsigned char *)"initial text", 12);
  coap_add_resource( ctx, r );

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *) "/data-sink",
			sizeof("/data-sink") - 1);
  r->mediatype = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
  r->dirty = 0;
  r->writable = 1;
  r->data = resource_from_file;
  coap_add_resource(ctx, r);

#if 0
  r->uri = coap_new_uri((const unsigned char *) "/filestorage");
  r->mediatype = COAP_MEDIATYPE_APPLICATION_LINK_FORMAT;
  r->dirty = 0;
  r->data = resource_from_file;
  coap_add_resource( ctx, r );

  if ( !(r = coap_malloc( sizeof(coap_resource_t) ))) 
    return;

  r->uri = coap_new_uri((const unsigned char *) "/filestorage/foo");
  r->mediatype = COAP_MEDIATYPE_ANY;
  r->dirty = 0;
  r->data = resource_from_file;
  coap_add_resource( ctx, r );
#endif
}

int 
main(int argc, char **argv) {
  coap_context_t  *ctx;
  fd_set readfds;
  struct timeval tv, *timeout;
  int result;
  time_t now;
  coap_queue_t *nextpdu;
  unsigned short port = COAP_DEFAULT_PORT;
  int opt;
  char *group = NULL;

  while ((opt = getopt(argc, argv, "g:p:")) != -1) {
    switch (opt) {
    case 'g' :
      group = optarg;
      break;
    case 'p' :
      port = atoi(optarg);
      break;
    default:
      usage( argv[0], PACKAGE_VERSION );
      exit( 1 );
    }
  }

  ctx = coap_new_context( port );
  if ( !ctx )
    return -1;

  if ( group )
    join( ctx, group );

  coap_register_message_handler( ctx, message_handler );
  init_resources(ctx);

  signal(SIGINT, handle_sigint);

  while ( !quit ) {
    FD_ZERO(&readfds); 
    FD_SET( ctx->sockfd, &readfds );
    
    nextpdu = coap_peek_next( ctx );

    time(&now);
    while ( nextpdu && nextpdu->t <= now ) {
      coap_retransmit( ctx, coap_pop_next( ctx ) );
      nextpdu = coap_peek_next( ctx );
    }

    if ( nextpdu && nextpdu->t <= now + COAP_RESOURCE_CHECK_TIME ) {
      /* set timeout if there is a pdu to send before our automatic timeout occurs */
      tv.tv_usec = 0;
      tv.tv_sec = nextpdu->t - now;
      timeout = &tv;
    } else {
      tv.tv_usec = 0;
      tv.tv_sec = COAP_RESOURCE_CHECK_TIME;
      timeout = &tv;
    }
    result = select( FD_SETSIZE, &readfds, 0, 0, timeout );
    
    if ( result < 0 ) {		/* error */
      if (errno != EINTR)
	perror("select");
    } else if ( result > 0 ) {	/* read from socket */
      if ( FD_ISSET( ctx->sockfd, &readfds ) ) {
	coap_read( ctx );	/* read received data */
	coap_dispatch( ctx );	/* and dispatch PDUs from receivequeue */
      } 
    } else {			/* timeout */
      coap_check_resource_list( ctx );
      coap_check_subscriptions( ctx );
    }
  }

  coap_free_context( ctx );

  return 0;
}
