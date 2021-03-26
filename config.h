/*
 * Copyright 2006-2017 Niklas Edmundsson <nikke@acc.umu.se>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef _CACHE_CONFIG_H
#define _CACHE_CONFIG_H

/* Check status every N iterations while copying a file */
#define CPCHKINTERVAL           20

#define CPBUFSIZE               262144

/* Size of window to flush when writing */
#define CACHE_WRITE_FLUSH_WINDOW 8388608

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

#define SOCKPATH                "/var/run/.cachecopyd.sock"

static const char backend_root[]    = "/export/ftp/";
static const int  backend_len       = sizeof(backend_root)-1;

static const char cache_root[]      = "/httpcache/";
static const int  cache_len         = sizeof(cache_root)-1;

/* Breakpoint for large files to be put in different cache hierarchy.
   If used together with redirprg, keep in sync with minredirsize. */
#define CACHE_BF_SIZE               4194304

static const char bfcache_root[]    = "/httpcachebf/";
static const int  bfcache_len       = sizeof(bfcache_root)-1;

#endif /* _CACHE_CONFIG_H */
