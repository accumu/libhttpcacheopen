
static const char cacheopenrcsid[] = /*Add RCS version string to binary */
        "$Id: cacheopen.c,v 1.4 2008/05/10 11:09:49 source Exp source $";

#include <sys/types.h>
#include <utime.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>


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
                         ssize_t (*readfunc)(int fd, void *buf, size_t count),
                         int (*closefunc)(int fd))
{
    int                 destfd, modflags;
    void                *tmp;
    char                *buf;
    ssize_t             amt, wrt, done;
    copy_status         rc = COPY_OK;

    destfd = open_new_file(destfile, openfunc, statfunc);
    if(destfd < 0) {
        return(destfd);
    }

#ifdef O_DIRECT
    /* C99 strict aliasing rules forces us to dance via a temporary pointer */
    if(posix_memalign(&tmp, 512, CPBUFSIZE)) {
        buf = NULL;
    }
    else {
        buf = tmp;
    }
#else /* O_DIRECT */
    buf = malloc(CPBUFSIZE);
#endif /* O_DIRECT */
    if(buf == NULL) {
        closefunc(destfd);
        return(COPY_FAIL);
    }

    /* Remove nonblocking IO, enable direct IO */
    modflags = srcflags;
    if(srcflags & O_NONBLOCK
#ifdef O_DIRECT
            || !(srcflags & O_DIRECT) ) {
        modflags |= O_DIRECT;
#else /* O_DIRECT */
        ) {
#endif
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

    while(len > 0) {
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
            amt -= wrt;
            len -= wrt;
        }
    }

    if(len != 0) {
        /* Weird, didn't read expected amount */
#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: copy_file: len %lld left\n", len);
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
                      ssize_t (*readfunc)(int fd, void *buf, size_t count)) 
{
    struct sockaddr_un sa;
    int sock, rc;
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
