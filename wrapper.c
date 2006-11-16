#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600

#define _LARGEFILE64_SOURCE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/param.h>
#include <dlfcn.h>
#include <stdio.h>

#include "md5.c"

#define DEBUG

#define CPBUFSIZE               262144
#define CACHE_UPDATE_TIMEOUT    30      /* Note! In seconds! */
#define MAX_COPY_SIZE           (150*1024*1024)
#define DIRLENGTH               1
#define DIRLEVELS               2
#define CACHE_BODY_SUFFIX       ".body"

#define CACHE_LOOP_SLEEP        200 /* in ms, lower than 1s */

static const char rcsid[] = /*Add RCS version string to binary */
        "$Id$";


static const char backend_root[]    = "/export/ftp/";
static const char cache_root[]      = "/httpcache/";

static const int  backend_len       = sizeof(backend_root)-1;
static const int  cache_len         = sizeof(cache_root)-1;

#ifdef linux
static int (*_open)(const char *, int, ...);
static FILE *(*_fopen)(const char *, const char *);
static FILE *(*_fopen64)(const char *, const char *);
#endif


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

static int mkdir_structure(char *path) {
    char *p = path + cache_len;
    int ret, retry=0;

    p = strchr(p, '/');
    while(p && *p && retry < 10) {
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

    return(0);
}

typedef enum copy_status {
    COPY_FAIL = -1,
    COPY_EXISTS = -2,
    COPY_OK = 0
} copy_status;

static int open_new_file(char *destfile) {
    int fd;
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE;
    mode_t mode = S_IRUSR | S_IWUSR;

    while(1) {
        fd = _open(destfile, flags, mode);
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

            if((stat64(destfile, &st)) == -1) {
                if(errno == ENOENT) {
                    /* Already removed, try again */
                    continue;
                }
                else {
                    return(-1);
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
                        return(-1);
                    }
                }

            }
            else {
                /* Someone else beat us to this */
                return(COPY_EXISTS);
            }
        }
        else if(errno == ENOENT) {
            /* Directory missing, create and try again */
            if((mkdir_structure(destfile)) == -1) {
                return(-1);
            }
        }
        else {
            return(-1);
        }
    }

    errno = EFAULT;
    return(-1);
}

static copy_status copy_file(int srcfd, off64_t len, char *destfile) {
    int      destfd, srcflags, modflags;
    char    *buf;
    ssize_t amt, wrt, done;

    if(posix_memalign((void **) &buf, 512, CPBUFSIZE)) {
        return(COPY_FAIL);
    }

    destfd = open_new_file(destfile);
    if(destfd < 0) {
        free(buf);
        return(destfd);
    }

    /* Remove nonblocking IO if present... */
    srcflags = modflags = fcntl(srcfd, F_GETFL);
#ifdef DEBUG
    if(srcflags == -1) {
        perror("fcntl");
    }
#endif
    if( srcflags != -1 && (srcflags & O_NONBLOCK || !(srcflags & O_DIRECT)) ) {
        modflags = srcflags & ~O_NONBLOCK;
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
        amt = read(srcfd, buf, CPBUFSIZE);
        if(amt == -1) {
            if(errno == EINTR) {
                continue;
            }
#ifdef DEBUG
            perror("httpcacheopen: copy_file: read");
#endif
            goto error_exit;
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
                goto error_exit;
            }
            done += wrt;
            amt -= wrt;
            len -= wrt;
        }
    }

    if(len != 0) {
#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: copy_file: len %lld left\n", len);
#endif
        /* Weird, didn't read expected amount */
        goto error_exit;
    }

    if((close(destfd)) == -1) {
#ifdef DEBUG
        perror("httpcacheopen: copy_file: close destfd");
#endif
        goto error_exit_noclose;
    }

    lseek64(srcfd, 0, SEEK_SET);
    if(srcflags != modflags) {
        fcntl(srcfd, F_SETFL, srcflags);
    }

    /* Success! */
    return(COPY_OK);

error_exit:
    close(destfd);
error_exit_noclose:
    unlink(destfile);
    lseek64(srcfd, 0, SEEK_SET);
    if(srcflags != modflags) {
        fcntl(srcfd, F_SETFL, srcflags);
    }
    return(COPY_FAIL);
}

int open(const char *path, int oflag, /* mode_t mode */...) {
    va_list         ap;
    int             realfd, cachefd;
    mode_t          mode;
    struct stat64   realst, cachest;
    struct timespec delay;
    char            cachepath[PATH_MAX];

    if(!path) {
        errno = ENOENT;
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: open %s\n", path);
#endif

#ifdef linux
    if(!_open) {
        _open = dlsym( RTLD_NEXT, "open" );
        if(!_open) {
            fprintf(stderr, "httpcacheopen: open(): Init failed\n");
        }
    }
#endif

    if(oflag & O_CREAT) {
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        realfd = _open(path, oflag, mode);
    }
    else {
        realfd = _open(path, oflag);
    }

    if(realfd == -1 || oflag & (O_WRONLY | O_RDWR)) {
        return(realfd);
    }

    /* If we get here, there are possibillities to use a cached copy of the
       file in the httpcache instead */

    if(strncmp(path, backend_root, backend_len)) {
        /* Not a backend file, ignore */
#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: Not a backend file\n");
#endif
        return(realfd);
    }

    /* Calculate cachepath */
    strcpy(cachepath, cache_root);
    cache_hash(path, cachepath+cache_len, DIRLEVELS, DIRLENGTH);
    strcat(cachepath, CACHE_BODY_SUFFIX);

    if(fstat64(realfd, &realst) == -1) {
#ifdef DEBUG
            perror("Unable to fstat realfd");
#endif
        /* Somewhere, something went very wrong */
        return(realfd);
    }

    cachefd = _open(cachepath, oflag);
    if(cachefd == -1) {
        if(realst.st_size > MAX_COPY_SIZE) {
#ifdef DEBUG
            fprintf(stderr, "httpcacheopen: File over copy sizelimit\n");
#endif
            return(realfd);
        }
        if((copy_file(realfd, realst.st_size, cachepath)) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: copy_file failed");
#endif
            return(realfd);
        }
        /* We have either copied the file, or another process is already
         * caching the file and we need to wait for it to finish */
        cachefd = _open(cachepath, oflag);
        if(cachefd == -1) {
#ifdef DEBUG
            perror("httpcacheopen: open cachefd after copy_file 1");
#endif
            return(realfd);
        }
    }

    if(fstat64(cachefd, &cachest) == -1) {
#ifdef DEBUG
        perror("httpcacheopen: Unable to fstat cachefd");
#endif
        /* Oh well, fallback to use the real file */
        close(cachefd);
        return(realfd);
    }

    if(realst.st_mtime > cachest.st_mtime) {
        /* Bollocks, the cached file is stale */
        close(cachefd);
        if((copy_file(realfd, realst.st_size, cachepath)) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: copy_file failed 2");
#endif
            return(realfd);
        }
        cachefd = _open(cachepath, oflag);
        if(cachefd == -1) {
#ifdef DEBUG
            perror("httpcacheopen: open cachefd after copy_file 2");
#endif
            return(realfd);
        }
        if(fstat64(cachefd, &cachest) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: Unable to fstat cachefd 2");
#endif
            close(cachefd);
            return(realfd);
        }
    }

    while(realst.st_size != cachest.st_size) {
        if(cachest.st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT) {
#ifdef DEBUG
            fprintf(stderr, 
                    "httpcacheopen: Timed out waiting for cached file\n");
#endif
            /* Caching timed out */
            close(cachefd);
            return(realfd);
        }
        delay.tv_sec = 0;
        delay.tv_nsec = CACHE_LOOP_SLEEP*1000000;
        nanosleep(&delay, NULL);
        if(fstat64(cachefd, &cachest) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: Unable to fstat cachefd 3");
#endif
            close(cachefd);
            return(realfd);
        }
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: Success, returning cached fd %s\n", 
            cachepath);
#endif

    /* Victory! */
    close(realfd);
    return(cachefd);
}

int open64(const char *path, int oflag, /* mode_t mode */...){
    int tmp;
    va_list ap;
    mode_t mode;

    oflag |= O_LARGEFILE;

    if(oflag & O_CREAT) {
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        tmp = open(path, oflag, mode);
    }
    else {
        tmp = open(path, oflag);
    }

    return tmp;
}

FILE *fopen(const char *filename, const char *mode) {

    if(!filename || !mode) {
        errno = ENOENT;
        return NULL;
    }

    /* The only readonly-mode is r and rb */
    if(strncmp(mode,"r+", 2) && strncmp(mode,"rb+", 3) && 
            !strncmp(mode, "r", 1)) 
    {
        int fd = open(filename, O_RDONLY);
        if(fd == -1) {
            return NULL;
        }
        return fdopen(fd, mode);
    }


#ifdef linux
    if(!_fopen) {
        _fopen = dlsym( RTLD_NEXT, "fopen" );
        if(!_fopen) {
            fprintf(stderr, "httpcacheopen: fopen(): Init failed\n");
            exit(1);
        }
    }
#endif

    return _fopen(filename, mode);
}


FILE *fopen64(const char *filename, const char *mode) {

    if(!filename || !mode) {
        errno = ENOENT;
        return NULL;
    }

    /* The only readonly-mode is r and rb */
    if(strncmp(mode,"r+", 2) && strncmp(mode,"rb+", 3) && 
            !strncmp(mode, "r", 1)) 
    {
        int fd = open64(filename, O_RDONLY);
        if(fd == -1) {
            return NULL;
        }
        return fdopen(fd, mode);
    }


#ifdef linux
    if(!_fopen64) {
        _fopen64 = dlsym( RTLD_NEXT, "fopen64" );
        if(!_fopen64) {
            fprintf(stderr, "httpcacheopen: fopen64(): Init failed\n");
            exit(1);
        }
    }
#endif

    return _fopen64(filename, mode);
}


