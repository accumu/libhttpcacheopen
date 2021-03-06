/*
 * Copyright 2006-2019 Niklas Edmundsson <nikke@acc.umu.se>
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


#include <sys/types.h>
#include <utime.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux
#include <linux/falloc.h>
#endif /* __linux */


#include "md5.c"

static void cache_hash(const char *it, char *val, int ndepth, int nlength)
{
    MD5_CTX context;
    char tmp[22];
    int i, k, d;
    unsigned int x;
    static const char enc_table[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_@";

    MD5Init(&context);
    MD5Update(&context, (unsigned char *) it, strlen(it));
    MD5Final(&context);

    /* encode 128 bits as 22 characters, using a modified uuencoding
     * the encoding is 3 bytes -> 4 characters* i.e. 128 bits is
     * 5 x 3 bytes + 1 byte -> 5 * 4 characters + 2 characters
     */
    for (i = 0, k = 0; i < 15; i += 3) {
        x = (context.digest[i] << 16) | (context.digest[i + 1] << 8) | context.digest[i + 2];
        tmp[k++] = enc_table[x >> 18];
        tmp[k++] = enc_table[(x >> 12) & 0x3f];
        tmp[k++] = enc_table[(x >> 6) & 0x3f];
        tmp[k++] = enc_table[x & 0x3f];
    }

    /* one byte left */
    x = context.digest[15];
    tmp[k++] = enc_table[x >> 2];    /* use up 6 bits */
    tmp[k++] = enc_table[(x << 4) & 0x3f];

    /* now split into directory levels */
    for (i = k = d = 0; d < ndepth; ++d) {
        memcpy(&val[i], &tmp[k], nlength);
        k += nlength;
        val[i + nlength] = '/';
        i += nlength + 1;
    }
    memcpy(&val[i], &tmp[k], 22 - k);
    val[i + 22 - k] = '\0';
}

#define MAX_MKDIR_RETRY 10
static int mkdir_structure(char *path) {
    char *p = path + cache_len;
    int ret, retry=0;

    p = strchr(p, '/');
    while(p && *p && retry < MAX_MKDIR_RETRY) {
        *p = '\0';
        ret = mkdir(path, 0700);
        *p = '/';
        p++;
        if(ret == -1) {
            if(errno == ENOENT) {
                /* Someone removed the directory tree while we were at it,
                   redo from start... */
                retry++;
                p = path + cache_len;
            }
            else if(errno != EEXIST) {
                return(-1);
            }
        }
        p = strchr(p, '/');
    }

    if(retry >= MAX_MKDIR_RETRY) {
        return(-1);
    }

    return(0);
}

typedef enum copy_status {
    COPY_FAIL = -1,
    COPY_EXISTS = -2,
    COPY_OK = 0
} copy_status;

static int open_new_file(char *destfile, 
                         int (*openfunc)(const char *, int, ...),
                         int (*statfunc)(const char *, struct stat64 *))
{
    int fd;
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE;
    mode_t mode = S_IRUSR | S_IWUSR;

    while(1) {
        fd = openfunc(destfile, flags, mode);
        if(fd != -1) {
#ifdef DEBUG
            fprintf(stderr, "httpcacheopen: open_new_file: Opened %s\n",
                    destfile);
#endif
            return(fd);
        }
#ifdef DEBUG
        perror("httpcacheopen: open_new_file");
#endif
        if(errno == EEXIST) {
            struct stat64 st;

            if((statfunc(destfile, &st)) == -1) {
                if(errno == ENOENT) {
                    /* Already removed, try again */
                    continue;
                }
                else {
                    return COPY_FAIL;
                }
            }

            if(st.st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT) {
                /* Something stale */
                if((unlink(destfile)) == -1) {
                    if(errno == ENOENT) {
                        /* Already removed, try again */
                        continue;
                    }
                    else {
                        return COPY_FAIL;
                    }
                }

            }
            else {
                /* Someone else beat us to this */
                return COPY_EXISTS;
            }
        }
        else if(errno == ENOENT) {
            /* Directory missing, create and try again */
            if((mkdir_structure(destfile)) == -1) {
                return COPY_FAIL;
            }
        }
        else {
            return COPY_FAIL;
        }
    }

    errno = EFAULT;
    return COPY_FAIL;
}

static copy_status copy_file(int srcfd, int srcflags, off64_t len, 
                         time_t mtime, char *destfile,
                         int (*openfunc)(const char *, int, ...),
                         int (*statfunc)(const char *, struct stat64 *),
                         int (*fstat64func)(int filedes, struct stat64 *buf),
                         ssize_t (*readfunc)(int fd, void *buf, size_t count),
                         int (*closefunc)(int fd))
{
    int                 destfd, modflags, i, err;
    char                *buf;
    ssize_t             amt, wrt, done;
    off64_t             srcoff, destoff, flushoff;
    copy_status         rc = COPY_OK;

    destfd = open_new_file(destfile, openfunc, statfunc);
    if(destfd < 0) {
        return(destfd);
    }

    buf = malloc(CPBUFSIZE);
    if(buf == NULL) {
        closefunc(destfd);
        return(COPY_FAIL);
    }

    /* Remove nonblocking IO */
    modflags = srcflags;
    if(srcflags & O_NONBLOCK
        ) {
        modflags &= ~O_NONBLOCK;
        if((fcntl(srcfd, F_SETFL, modflags)) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: copy_file: Failed changing fileflags");
#endif
            modflags = srcflags;
        }
#ifdef DEBUG
        else {
            fprintf(stderr, "httpcacheopen: copy_file: Modified file flags\n");
        }
#endif
    }

    /* We expect sequential IO */
    err=posix_fadvise(srcfd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if(err) {
#ifdef DEBUG
        fprintf(stderr, "posix_fadvise: %s\n", strerror(err));
#endif
    }

#ifdef __linux
    /* Use Linux fallocate() to preallocate file */
    if(len > 0) {
        if(fallocate(destfd, FALLOC_FL_KEEP_SIZE, 0, len) != 0) {
#ifdef DEBUG
                perror("copy_file: fallocate");
#endif
                /* Only choke on relevant errors */
                if(errno == EBADF || errno == EIO || errno == ENOSPC) {
                    rc = COPY_FAIL;
                    goto exit;
                }
        }
    }
#endif /* __linux */

    srcoff=0;
    destoff=0;
    flushoff=0;
    i=0;
    while(len > 0) {
        if(i++ >= CPCHKINTERVAL) {
            struct stat64 st;

            i=0;
            if(fstat64func(destfd, &st) == -1) {
                /* Shouldn't happen, really */
#ifdef DEBUG
                perror("copy_file: Unable to fstat destfd");
#endif
                rc = COPY_FAIL;
                goto exit;
            }
            if(st.st_nlink == 0) {
                /* Destination file deleted, no use to continue */
#ifdef DEBUG
                fprintf(stderr, "httpcacheopen: copy_file: destfd unlinked\n");
#endif
                rc = COPY_FAIL;
                goto exit;
            }
        }
        amt = readfunc(srcfd, buf, CPBUFSIZE);
        if(amt == -1) {
            if(errno == EINTR) {
                continue;
            }
#ifdef DEBUG
            perror("httpcacheopen: copy_file: read");
#endif
            rc = COPY_FAIL;
            goto exit;
        }
        if(amt == 0) {
            break;
        }
        /* We will never need this segment again */
        err=posix_fadvise(srcfd, srcoff, amt, POSIX_FADV_DONTNEED);
#ifdef DEBUG
        if(err) {
            fprintf(stderr, "posix_fadvise: %s\n", strerror(err));
        }
#endif
        srcoff += amt;
        if(len-amt > 0) {
            /* Tell kernel that we'll need more segments soon */
            err=posix_fadvise(srcfd, srcoff, 2*CPBUFSIZE, POSIX_FADV_WILLNEED);
#ifdef DEBUG
            if(err) {
                fprintf(stderr, "posix_fadvise: %s\n", strerror(err));
            }
#endif
        }
        done = 0;
        while(amt > 0) {
            wrt = write(destfd, buf+done, amt);
            if(amt == -1) {
                if(errno == EINTR) {
                    continue;
                }
#ifdef DEBUG
                perror("httpcacheopen: copy_file: write");
#endif
                rc = COPY_FAIL;
                goto exit;
            }
            done += wrt;
            destoff += wrt;
            amt -= wrt;
            len -= wrt;
        }
        if(destoff - flushoff >= CACHE_WRITE_FLUSH_WINDOW) {
            /* Start flushing the current write window */
            if(sync_file_range(destfd, flushoff, destoff - flushoff,
                        SYNC_FILE_RANGE_WRITE) != 0)
            {   
                rc = COPY_FAIL;
                goto exit;
            }
            /* Wait for the previous window to be written to disk before
               continuing. This is to prevent the disk write queues to be
               chock full if incoming data rate is higher than the disks can
               handle, which will cause horrible read latencies for other
               requests while handling writes for this one */
            if(flushoff >= CACHE_WRITE_FLUSH_WINDOW) {
                if(sync_file_range(destfd, flushoff-CACHE_WRITE_FLUSH_WINDOW,
                            CACHE_WRITE_FLUSH_WINDOW,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER)
                        != 0)
                {   
                    rc = COPY_FAIL;
                    goto exit;
                }
            }

            flushoff = destoff;
        }
    }

    if(len != 0) {
        /* Weird, didn't read expected amount */
#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: copy_file: len %lld left\n", 
                (long long)len);
#endif
        rc = COPY_FAIL;
        goto exit;
    }


exit:
    free(buf);

    if((closefunc(destfd)) == -1) {
#ifdef DEBUG
        perror("httpcacheopen: copy_file: close destfd");
#endif
        unlink(destfile);
        rc = COPY_FAIL;
    }
    else {
        struct utimbuf      ut;
        /* Set mtime on file to same as source */
        ut.actime = time(NULL);
        ut.modtime = mtime;
        utime(destfile, &ut);
    }

    lseek64(srcfd, 0, SEEK_SET);

#ifdef __sun
    /* OK, we're being lame assuming file was opened with default
       behaviour */
    directio(srcfd, DIRECTIO_OFF);
#endif /* __sun */

    if(srcflags != modflags) {
        fcntl(srcfd, F_SETFL, srcflags);
    }

    return(rc);
}


static int cacheopen_check(const char *path) {

    if(strncmp(path, backend_root, backend_len)) {
        /* Not a backend file, ignore */
#ifdef DEBUG
        fprintf(stderr, "cacheopen: Not a backend file: %s\n", path);
#endif
        return -1;
    }

    return 0;
}


/* Given realst, constructs cachepath. cachepath is assumed to be
   large enough */
static void cacheopen_prepare(struct stat64 *realst, char *cachepath) 
{
    unsigned long long  inode, device;
    char                devinostr[34];
    int                 len;

    /* Hash on device:inode to eliminate file duplication. Since we only
       can serve plain files we don't have to bother with all the special
       cases in mod_disk_cache :) */
    device = realst->st_dev; /* Avoid ifdef-hassle with types */
    inode  = realst->st_ino;
    snprintf(devinostr, sizeof(devinostr), "%016llx:%016llx", device, inode);

    /* Calculate cachepath. We simply put large files in a separate path
       intended to separate the contention point for large and small files */
    if(realst->st_size < CACHE_BF_SIZE) {
        strcpy(cachepath, cache_root);
        len = cache_len;
    }
    else {
        strcpy(cachepath, bfcache_root);
        len = bfcache_len;
    }
    cache_hash(devinostr, cachepath+len, DIRLEVELS, DIRLENGTH);
    strcat(cachepath, CACHE_BODY_SUFFIX);
}


typedef enum cacheopen_status {
    CACHEOPEN_FAIL = -1,
    CACHEOPEN_DECLINED = -2,
    CACHEOPEN_STALE = -3
} cacheopen_status;


/* On success, fills in cachest */
static int cacheopen(struct stat64 *cachest, struct stat64 *realst, 
                     int oflag, char *cachepath,
                     int (*openfunc)(const char *, int, ...),
                     int (*fstat64func)(int filedes, struct stat64 *buf),
                     int (*closefunc)(int fd))
{
    int                 cachefd;

    /* Only cache regular files larger than 0 bytes */
    if(!S_ISREG(realst->st_mode) || realst->st_size == 0) {
        return CACHEOPEN_DECLINED;
    }

    cachefd = openfunc(cachepath, oflag);
    if(cachefd == -1) {
#ifdef DEBUG
        perror("cacheopen: Unable to open cachepath");
#endif
        return CACHEOPEN_FAIL;
    }

    if(fstat64func(cachefd, cachest) == -1) {
#ifdef DEBUG
        perror("cacheopen: Unable to fstat cachefd");
#endif
        /* Shouldn't fail, really... */
        closefunc(cachefd);
        return CACHEOPEN_FAIL;
    }

    /* FIXME: Use ctime != mtime too */
    if(realst->st_mtime > cachest->st_mtime ||
            (realst->st_size != cachest->st_size && 
               cachest->st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT)) 
    {
        /* Bollocks, the cached file is stale */
        closefunc(cachefd);
#ifdef DEBUG
        fprintf(stderr, "cacheopen: cached file stale\n");
#endif
        return CACHEOPEN_STALE;
    }

#ifdef DEBUG
    fprintf(stderr, "cacheopen: Success, returning cached fd %d (%s)\n", 
            cachefd, cachepath);
#endif

    return cachefd;
}


#ifdef USE_COPYD
static int copyd_file(char *file,
                      ssize_t (*readfunc)(int fd, void *buf, size_t count),
                      int (*closefunc)(int fd))
{
    struct sockaddr_un sa;
    int rc;
    int sock=-1;
    struct sigaction oldsig;
    char buf[10]; /* Should only get "OK\0" or "FAIL\0" */
    ssize_t amt;

#ifdef DEBUG
    fprintf(stderr, "copyd_file: file=%s\n", file);
#endif

    if(sigaction(SIGPIPE, NULL, &oldsig) == -1) {
#ifdef DEBUG
        perror("copyd_file: sigaction get");
#endif
        return -1;
    }

    if(oldsig.sa_flags & SA_SIGINFO || oldsig.sa_handler != SIG_IGN) {
        struct sigaction newsig;

        /* Ignore those pesky SIGPIPE:s */
        newsig.sa_handler = SIG_IGN;
        newsig.sa_sigaction = NULL;
        sigemptyset(&newsig.sa_mask);
        newsig.sa_flags = 0;
        if(sigaction(SIGPIPE, &newsig, &oldsig) == -1) {
#ifdef DEBUG
            perror("copyd_file: sigaction set");
#endif
            return -1;
        }
    }

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock == -1) {
#ifdef DEBUG
        perror("copyd_file: socket");
#endif
        rc = -1;
        goto err;
    }

    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCKPATH);

    if(connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
#ifdef DEBUG
        perror("copyd_file: connect");
#endif
        rc = -1;
        goto err;
    }

    amt = strlen(file)+1;
    rc=write(sock, file, amt);
    if(rc != amt) {
#ifdef DEBUG
        perror("copyd_file: write");
#endif
        goto err;
    }

#ifdef DEBUG
    fprintf(stderr, "copyd_file: wrote '%s' rc=%d\n", file, rc);
#endif

    rc = readfunc(sock, buf, sizeof(buf));
    if(rc == -1) {
#ifdef DEBUG
        perror("copyd_file: read");
#endif
        goto err;
    }
    else if(rc < 1) {
#ifdef DEBUG
        fprintf(stderr, "copyd_file: short read\n");
#endif
        rc = -1;
        goto err;
    }
    buf[rc-1] = '\0';

#ifdef DEBUG
    fprintf(stderr, "copyd_file: read '%s' rc=%d\n", buf, rc);
#endif

    if(!strcmp(buf, "OK")) {
        rc = 0;
    }
    else {
        rc = -1;
    }

err:
    if(sock >= 0) {
        closefunc(sock);
    }
    if(oldsig.sa_flags & SA_SIGINFO || oldsig.sa_handler != SIG_IGN) {
        if(sigaction(SIGPIPE, &oldsig, NULL) == -1) {
#ifdef DEBUG
            perror("copyd_file: restore sigaction");
#endif
        }
    }

    return rc;
}
#endif /* USE_COPYD */
