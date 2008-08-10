/* $Id$ */

#ifndef _CACHE_CONFIG_H
#define _CACHE_CONFIG_H

#define CPBUFSIZE               262144
#define CACHE_UPDATE_TIMEOUT    30      /* Note! In seconds! */
#define MAX_COPY_SIZE           10000000
#define DIRLENGTH               1
#define DIRLEVELS               2
#define CACHE_BODY_SUFFIX       ".body"

#define CACHE_LOOP_SLEEP        200 /* in ms, lower than 1s */

#ifdef USE_COPYD
#define CACHE_MAXFD             32768
#endif

#define COPYD_USER              "www-ftp"

#define SOCKPATH                "/tmp/.cachecopyd.sock"

static const char backend_root[]    = "/export/ftp/";
static const int  backend_len       = sizeof(backend_root)-1;

static const char cache_root[]      = "/httpcache/";
static const int  cache_len         = sizeof(cache_root)-1;

/* Breakpoint for large files to be put in different cache hierarchy */
#define CACHE_BF_SIZE               (250*1024*1024)

static const char bfcache_root[]    = "/httpcachebf/";
static const int  bfcache_len       = sizeof(bfcache_root)-1;

#endif /* _CACHE_CONFIG_H */
