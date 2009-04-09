#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600
#define _ALL_SOURCE 1

#define _LARGEFILE64_SOURCE 1
#define _LARGE_FILE_API 1

/* For directio() */
#ifdef __sun
#define __EXTENSIONS__
#endif /* __sun */

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
#include <utime.h>
#ifdef __sun
#include <sys/sendfile.h>
#endif /* __sun */

/* Ugh. Solaris is being moronic by defining a wrapper function in header
   files for non-LFS stat on 32bit. Ignore it for now, all our software
   is only using the LFS interface anyway */
#if defined(__sun) && !defined(_LP64)
#define WRAPPER_STAT_NOWRAP
#endif

#ifndef MIN
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#endif     /* MIN */
#ifndef MAX
#define MAX(X, Y) ((X) < (Y) ? (Y) : (X))
#endif  /* MAX */


#include "config.h"
#include "cleanpath.c"
#include "cacheopen.c"

static const char rcsid[] = /*Add RCS version string to binary */
        "$Id: wrapper.c,v 1.17 2009/04/08 08:16:58 source Exp source $";

#ifdef USE_COPYD
typedef struct cachefdinfo_t {
    struct stat64   realst;     /* stat of real file */
    int             complete;   /* TRUE if cached file complete, FALSE otherwise */
} cachefdinfo_t;

static cachefdinfo_t cachefdinfo[CACHE_MAXFD];
#endif /* USE_COPYD */


/* Declarations for the real functions that we override */
static int (*_open)(const char *, int, ...);
static FILE *(*_fopen)(const char *, const char *);
static FILE *(*_fopen64)(const char *, const char *);
static int (*_chdir)(const char *);
static char *(*_getcwd)(char *, size_t);

#ifndef WRAPPER_STAT_NOWRAP
static int (*_stat)(const char *, struct stat *);
static int (*_lstat)(const char *, struct stat *);
static int (*_fstat)(int, struct stat *);
#endif /* WRAPPER_STAT_NOWRAP */

#ifdef _AIX
/* Ugh. There have been different type declarations for readlink()
   floating around over the years. AIX native uses the old BSD typing. */
/* FIXME: Should probably catch the internal __readlink64 too on AIX, as
          that's what the SUSV3 readlink() wrapper uses */
static int (*_readlink)(const char *, char *, size_t);
#else /* _AIX */
static ssize_t (*_readlink)(const char *, char *, size_t);
#endif /* _AIX */

static int realstat64(const char *, struct stat64 *);

#ifdef __linux
/* Ugh. Linux uses inlined wrappers for *stat64, so we need to catch
   __*xstat64 instead */
static int (*___xstat64)(int, __const char *, struct stat64 *);
static int (*___lxstat64)(int, __const char *, struct stat64 *);
#else /* __linux */
static int (*_stat64)(const char *, struct stat64 *);
static int (*_lstat64)(const char *, struct stat64 *);
#endif /* __linux */

#ifdef USE_COPYD
static ssize_t (*_read)(int, void *, size_t);
static int (*_close)(int);
static size_t (*_fread)(void *, size_t, size_t, FILE *);
static int (*_fclose)(FILE *fp);
static ssize_t (*_sendfile64)(int, int, off64_t *, size_t);
#ifdef __linux
static int (*___fxstat64)(int, int, struct stat64 *);
#else /* __linux */
static int (*_fstat64)(int, struct stat64 *);
#endif /* __linux */
static int realfstat64(int, struct stat64 *);
#ifdef _AIX
static ssize_t (*_send_file)(int *, struct sf_parms *, uint_t);
#endif /* _AIX */
#endif /* USE_COPYD */

/* The issue of which types can hold function pointers is messy. xlc complains
   on mismatch between void * and actual type of function without a cast,
   gcc dies on invalid lvalue assignment with cast ... */

#ifdef _AIX
#define DLSYM_RETURN_CAST (void)
#endif
#ifndef DLSYM_RETURN_CAST
#define DLSYM_RETURN_CAST ;
#endif

#define GET_REAL_SYMBOL(a) \
if(! _##a) { \
    DLSYM_RETURN_CAST _##a = dlsym( RTLD_NEXT, #a ); \
    if(!_##a) { \
        perror("httpcacheopen: " #a "(): Init failed"); \
        exit(1); \
    } \
}

static char *cachedwd;          /* Cached working directory */
static char *chrootdir;         /* chroot():ed directory, if any */


/* Returns the cleaned path with an eventual chroot prepended */
/* Assumes buf is PATH_MAX in size */
static int get_full_path(char *buf, const char *path) {
    char pathcleaned[PATH_MAX];

    /* Copy the absolute path (ie. the one inside the current chroot
       and clean it from . .. // */
    if(path[0] == '/') {
        /* New path is absolute */
        if(strlen(path) + 1 > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(pathcleaned, path);
    }
    else if(cachedwd) {
        /* Yikes, relative path */
        if(strlen(cachedwd) + strlen(path) + 2 > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(pathcleaned, cachedwd);
        strcat(pathcleaned, "/");
        strcat(pathcleaned, path);
    }
    else {
        strcpy(pathcleaned, path);
    }
    cleanpath(pathcleaned);

    /* Prepend current chroot, if any */
    if(chrootdir != NULL) {
        if(strlen(chrootdir) + strlen(pathcleaned) + 1 > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(buf, chrootdir);
    }
    else {
        buf[0] = '\0';
    }

    strcat(buf, pathcleaned);

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: get_full_path: path=%s buf=%s\n", path,buf);
#endif

    return 0;
}


int open(const char *path, int oflag, /* mode_t mode */...) {
    va_list             ap;
    int                 realfd, cachefd;
    mode_t              mode;
    struct stat64       realst, cachest;
    char                realpath[PATH_MAX], cachepath[PATH_MAX];
    time_t              starttime=0;


    if(!path) {
        errno = ENOENT;
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "open: path=%s\n", path);
#endif

    GET_REAL_SYMBOL(open);

    if(get_full_path(realpath, path) == -1) {
#ifdef DEBUG
        perror("open: get_full_path failed");
#endif
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "open: realpath=%s\n", realpath);
#endif

    if(oflag & O_CREAT) {
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        realfd = _open(realpath, oflag, mode);
    }
    else {
        realfd = _open(realpath, oflag);
    }

    if(geteuid() == 0) {
#ifdef DEBUG
        fprintf(stderr, "open: euid == 0, skipping\n");
#endif
        return(realfd);
    }

    if(cacheopen_check(realpath) == -1) {
#ifdef DEBUG
        fprintf(stderr, "open: cacheopen_check failed\n");
#endif
        return(realfd);
    }

    if(realfd == -1 || oflag & (O_WRONLY | O_RDWR)) {
#ifdef DEBUG
        if(realfd == -1) {
            perror("open realfd");
        }
        else {
            fprintf(stderr, "open: Not read-only, oflag=%x\n", oflag);
        }
#endif
        return(realfd);
    }

    if(realfstat64(realfd, &realst) == -1) {
#ifdef DEBUG
            perror("Unable to fstat realfd");
#endif
        /* Somewhere, something went very wrong */
        return -1;
    }

    /* If we get here, there are possibillities to use a cached copy of the
       file in the httpcache instead */

    cacheopen_prepare(&realst, cachepath);

    GET_REAL_SYMBOL(close);
    cachefd = cacheopen(&cachest, &realst, oflag, cachepath, _open, realfstat64,
                        _close);

    if(cachefd == CACHEOPEN_FAIL || cachefd == CACHEOPEN_STALE) {
        /* Either no cached file or stale cached file, initiate
           file copy once */
        GET_REAL_SYMBOL(read);
        if(realst.st_size > MAX_COPY_SIZE) {
#ifdef DEBUG
            fprintf(stderr, "open: Filesize over copy sizelimit\n");
#endif

#ifdef USE_COPYD
            if(copyd_file(realpath, _read, _close) == -1) {
#ifdef DEBUG
                fprintf(stderr, "open: copyd_file failed\n");
#endif
                return realfd;
            }
#else /* USE_COPYD */
            return realfd;
#endif /* USE_COPYD */
        }
        else if(copy_file(realfd, oflag, realst.st_size, realst.st_mtime,
                          cachepath, _open, realstat64, realfstat64, 
                          _read, _close) 
                == COPY_FAIL)
        {
#ifdef DEBUG
            perror("open: copy_file COPY_FAIL");
#endif
            return realfd;
        }
        cachefd = cacheopen(&cachest, &realst, oflag, cachepath, _open, 
                            realfstat64, _close);
    }

    /* Loop until we've got either a file with contents or a timeout */
    while(1) {
        if(cachefd == CACHEOPEN_DECLINED) {
            return realfd;
        }

        if(cachefd < 0 && !starttime) {
            starttime = time(NULL);
        }

        /* cacheopen() fills in cachest for us */
#ifdef USE_COPYD
        if(cachefd < 0 || cachest.st_size == 0) {
#else /* USE_COPYD */
        if(cachefd < 0 || cachest.st_size != realst.st_size) {
#endif /* USE_COPYD */
            struct timespec     delay;
            time_t timeout = time(NULL) - CACHE_UPDATE_TIMEOUT;

            if(cachefd >= 0) {
                _close(cachefd);
            }

            if( (cachefd < 0 && starttime < timeout) ||
                (cachefd >= 0 && cachest.st_mtime < timeout) )
            {
#ifdef DEBUG
                fprintf(stderr, 
                        "open: Timed out waiting for cached file\n");
#endif
                /* Caching timed out */
                return realfd;
            }
            delay.tv_sec = 0;
            delay.tv_nsec = CACHE_LOOP_SLEEP*1000000;
            nanosleep(&delay, NULL);
            /* And again! */
            cachefd = cacheopen(&cachest, &realst, oflag, cachepath, _open,
                                realfstat64, _close);
            continue;
        }

        /* We're done */
        break;
    }

#ifdef USE_COPYD
        if(cachefd < CACHE_MAXFD) {
            /* Save information needed when doing read-while-caching */
            if(realst.st_size == cachest.st_size) {
                cachefdinfo[cachefd].complete=1;
            }
            else {
                cachefdinfo[cachefd].complete=0;
            }
            memcpy(&cachefdinfo[cachefd].realst, &realst, sizeof(realst));
        }
        else {
            /* No place in struct, do the best of the situation */
            if(realst.st_size != cachest.st_size) {
                _close(cachefd);
                return(realfd);
            }
        }
#endif /* USE_COPYD */

    /* Victory! */
    _close(realfd);
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
        int fd;

        fd = open64(filename, O_RDONLY);
        if(fd == -1) {
            return NULL;
        }
        return fdopen(fd, mode);
    }

    GET_REAL_SYMBOL(fopen);
    if(chrootdir) {
        char fullpath[PATH_MAX];

        if(get_full_path(fullpath, filename) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: fopen: get_full_path failed");
#endif
            return NULL;
        }

        return _fopen(fullpath, mode);
    }

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
        int fd;

        fd = open64(filename, O_RDONLY);
        if(fd == -1) {
            return NULL;
        }
        return fdopen(fd, mode);
    }

    GET_REAL_SYMBOL(fopen64);
    if(chrootdir) {
        char fullpath[PATH_MAX];

        if(get_full_path(fullpath, filename) == -1) {
#ifdef DEBUG
            perror("httpcacheopen: fopen64: get_full_path failed");
#endif
            return NULL;
        }

        return _fopen64(fullpath, mode);
    }

    return _fopen64(filename, mode);
}


int chdir(const char *path) {
    char fullpath[PATH_MAX];
    int rc;

    GET_REAL_SYMBOL(chdir);

    if(get_full_path(fullpath, path) == -1) {
#ifdef DEBUG
        perror("httpcacheopen: chdir: get_full_path failed");
#endif
        return -1;
    }

    rc = _chdir(fullpath);
    if(rc == -1) {
        return -1;
    }

    if(!cachedwd) {
        cachedwd = malloc(PATH_MAX*2);
        if(cachedwd == NULL) {
            return -1;
        }
        cachedwd[0] = '\0';
    }

    /* Rely on _chdir having done max path length checking for us */
    if(path[0] == '/') {
        strcpy(cachedwd, path);
    }
    else {
        strcat(cachedwd, "/");
        strcat(cachedwd, path);
    }

    /* Remove . .. // */
    cleanpath(cachedwd);

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: chdir: Cached %s\n", cachedwd);
#endif

    return(rc);
}


char *getcwd(char *buffer, size_t size) {
    size_t cwdlen;

    /* Check if we have cached the working directory, if not do it the
       lazy way by calling our chdir that caches it :) */
    if(!cachedwd) {
        char mycwd[PATH_MAX];

        GET_REAL_SYMBOL(getcwd);
        if(_getcwd(mycwd, PATH_MAX) == NULL) {
            return NULL;
        }
        if(chdir(mycwd) == -1) {
            return NULL;
        }
    }

    cwdlen = strlen(cachedwd) + 1;

    if(!(buffer == NULL && size == 0) && cwdlen > size) {
        errno = ERANGE;
        return NULL;
    }

    /* Handle linux extension of allocating when buffer == NULL */
    if(buffer == NULL) {
        if(size == 0) {
            size = cwdlen;
        }
        buffer = malloc(size);
        if(buffer == NULL) {
            return NULL;
        }
    }
    strcpy(buffer, cachedwd);

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: getcwd: %s\n", buffer);
#endif

    return buffer;
}


/* We emulate chroot in order to support FTP daemons and other thingies that
   implements a virtual root by doing chroot. */
int chroot(const char *path) {
    struct stat64 st;
    char newdir[PATH_MAX];

    if(!chrootdir) {
        chrootdir = malloc(PATH_MAX);
        if(chrootdir == NULL) {
            return -1;
        }
        chrootdir[0] = '\0';
    }

    if(get_full_path(newdir, path) == -1) {
        return(-1);
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: chroot: newdir=%s\n", newdir);
#endif

    /* Do (very) basic checks */
    if((realstat64(newdir, &st)) == -1) {
        /* Failure, when your best just isn't good enough */
#ifdef DEBUG
        perror("httpcacheopen: chroot: stat");
#endif
        return(-1);
    }
    if(!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
#ifdef DEBUG
        perror("httpcacheopen: chroot");
#endif
        return(-1);
    }

    /* Success, set the new chrootdir and go! */
    strcpy(chrootdir, newdir);

    /* Need to update the cwd so it's correct. */
    /* FIXME: In practice we only need to do this when chdir:ing to "."
       since when chdir:ing to some other directory you have to do
       "chdir /" directly afterwards. So we cheat. */
    if(!strcmp(path, ".")) {
        chdir("/");
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: chroot: chrootdir=%s\n", chrootdir);
#endif

    return(0);
}


#ifndef WRAPPER_STAT_NOWRAP
int stat(const char *path, struct stat *buffer) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(stat);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return _stat(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return _stat(realpath, buffer);
}
#endif /* WRAPPER_STAT_NOWRAP */


#ifdef __linux
/* Ugh. Linux uses inlined wrappers for *stat64, so we need to catch
   __*xstat64 instead. Code duplication for the win */
int __xstat64 (int __ver, __const char *path, struct stat64 *buffer) {
    char realpath[PATH_MAX];

    /* Check for ABI mismatch */
    if(__ver != _STAT_VER) {
        fprintf(stderr, "httpcacheopen: __xstat64(): stat version mismatch\n");
        exit(2);
    }

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return realstat64(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return realstat64(realpath, buffer);
}


/* Provide a wrapper for the rest of our code */
static int realstat64(const char *path, struct stat64 *buffer) {
    GET_REAL_SYMBOL(__xstat64);
    return ___xstat64 (_STAT_VER, path, buffer);
}


int __lxstat64 (int __ver, __const char *path, struct stat64 *buffer) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(__lxstat64);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return ___lxstat64(__ver, path, buffer);
    }

    /* Check for ABI mismatch */
    if(__ver != _STAT_VER) {
        fprintf(stderr, "httpcacheopen: __lxstat64(): stat version mismatch\n");
        exit(2);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return ___lxstat64(__ver, realpath, buffer);
}

#else /* __linux */

int stat64(const char *path, struct stat64 *buffer) {
    char realpath[PATH_MAX];

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return realstat64(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return realstat64(realpath, buffer);
}


/* Provide a wrapper for the rest of our code */
static int realstat64(const char *path, struct stat64 *buffer) {
    GET_REAL_SYMBOL(stat64);
    return _stat64(path, buffer);
}


int lstat64(const char *path, struct stat64 *buffer) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(lstat64);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return _lstat64(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return _lstat64(realpath, buffer);
}
#endif /* __linux */


#ifndef WRAPPER_STAT_NOWRAP
int lstat(const char *path, struct stat *buffer) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(lstat);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return _lstat(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return _lstat(realpath, buffer);
}
#endif /* WRAPPER_STAT_NOWRAP */


#ifdef _AIX
/* AIX native uses the old BSD typing. See comment at beginning of this file */
int
#else
ssize_t 
#endif
readlink(const char *path, char *buffer, size_t buffersize) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(readlink);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return _readlink(path, buffer, buffersize);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return _readlink(path, buffer, buffersize);
}


#ifdef USE_COPYD


/* Check if cachefd is complete.
   Returns
           -1 on error.
            0 if file is NOT complete.
            1 if file complete.
 */
static int cache_file_complete(int fd, struct stat64 *st) {
    /* fd's that don't fit into the array are always complete */
    if(fd >= CACHE_MAXFD || fd < 0) {
        return 1;
    }

    /* Zero size means not a cachefd */
    /* FIXME: BUG: Borde inte det vara st_size == 0 här ? eller <= 0 ?? */
    if(cachefdinfo[fd].realst.st_size < 0) {
        return 1;
    }

    if(cachefdinfo[fd].complete) {
        return 1;
    }

    if(realfstat64(fd, st)) {
#ifdef DEBUG
        perror("cache_file_complete: fstat64");
#endif
        return -1;
    }

    if(st->st_size >= cachefdinfo[fd].realst.st_size) {
        cachefdinfo[fd].complete = 1;
        return 1;
    }

    return 0;
}


/* -1 == error, 0 == timeout, 1 == data */
int wait_for_io(int fd, off64_t off, struct stat64 *st) {

    while(1) {
        if(realfstat64(fd, st) < 0) {
#ifdef DEBUG
            perror("httpcacheopen: wait_for_io: fstat64");
#endif
            return -1;
        }
        if(st->st_size <= off) {
            struct timespec     delay;

            /* Check if file has gone stale */
            if(st->st_nlink == 0 || st->st_mtime != st->st_ctime ||
                    st->st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT) 
            {
#ifdef DEBUG
                fprintf(stderr, "httpcacheopen: wait_for_io: timeout\n");
#endif
                return 0;
            }

            delay.tv_sec = 0;
            delay.tv_nsec = CACHE_LOOP_SLEEP*1000000;
            nanosleep(&delay, NULL);
            continue;
        }
        break;
    }

    return 1;
}


ssize_t read(int fd, void *buf, size_t count) {
    ssize_t         amt;
    int             flags, rc;
    struct stat64   st;
    off64_t         off;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: read fd=%d\n", fd);
#endif

    GET_REAL_SYMBOL(read);

    amt = _read(fd, buf, count);

    /* Nothing fancy needed if we got data or error :) */
    if(amt != 0) {
        return amt;
    }

    rc = cache_file_complete(fd, &st);
    if(rc == 1) {
        /* File complete, we have really hit EOF */
        return amt;
    }
    else if(rc < 0) {
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: read fd=%d: Hit EOF but file not complete\n", fd);
#endif

    /* OK, there will be more data soon. First check if we are non-blocking */
    /* FIXME: Is this really smart? The fd will return as readable by select
              which will probably create a nice ping-pong effect. Kludge it
              by waiting a bit before we return?
     */
    flags = fcntl(fd, F_GETFL);
    if(flags < 0) {
        return -1;
    }
    if(flags & O_NONBLOCK) {
        errno = EAGAIN;
        return -1;
    }

    /* OK. Let's wait for some action then... */
    off = lseek64(fd, 0, SEEK_CUR);
    if(off == -1) {
#ifdef DEBUG
        perror("httpcacheopen: read: lseek64");
#endif
        return -1;
    }
    rc = wait_for_io(fd, off, &st);
    if(rc == -1) {
        return -1;
    }
    else if(rc == 0) {
        errno = EIO;
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: read fd=%d: Got data, now size=%lld\n",
            fd, (long long)st.st_size);
#endif

    /* Assume read will succeed now (assuming makes an ass out of u and me) */
    return _read(fd, buf, count);
}


int close(int fd) {

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: close fd=%d\n", fd);
#endif

    GET_REAL_SYMBOL(close);

    if(fd < CACHE_MAXFD) {
        /* Clear the entire struct to return it to default state */
        memset(&cachefdinfo[fd].realst, sizeof(struct stat64), 0);
    }

    return _close(fd);
}


size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    int fd = fileno(stream), rc;
    struct stat64 st;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fread\n");
#endif

    GET_REAL_SYMBOL(fread);

    if(fd == -1) {
        return 0;
    }
    rc = cache_file_complete(fd, &st);
    if(rc == -1) {
        return 0;
    }
    else if(rc == 0) {
        off64_t pos = ftello64(stream);

#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: fread: File not complete\n");
#endif

        if(pos < 0) {
            /* Shouldn't happen, but just in case... */
            pos = 0;
        }

        if(st.st_size < pos + (off_t) (size*nmemb)) {
#ifdef DEBUG
            fprintf(stderr, "httpcacheopen: fread: Wait for data\n");
#endif
            rc = wait_for_io(fd, pos + size*nmemb, &st);
            if(rc == -1) {
                return 0;
            }
            else if(rc == 0) {
                /* FIXME: No way to set persistent error? */
                return 0;
            }
        }
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fread: read %zu bytes\n", size*nmemb);
#endif

    return _fread(ptr, size, nmemb, stream);
}


int fclose(FILE *fp) {
    int fd = fileno(fp);

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fclose\n");
#endif

    GET_REAL_SYMBOL(fclose);

    if(fd < CACHE_MAXFD) {
        /* Clear the entire struct to return it to default state */
        memset(&cachefdinfo[fd].realst, sizeof(struct stat64), 0);
    }

    return _fclose(fp);
}


#ifdef __linux
int __fxstat64(int __ver, int fd, struct stat64 *buf) {

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fstat64 fd=%d\n", fd);
#endif

    /* Check ABI version */
    if(__ver != _STAT_VER) {
        fprintf(stderr, "httpcacheopen: __fxstat64(): stat version mismatch\n");
        exit(2);
    }

    if(fd < CACHE_MAXFD && cachefdinfo[fd].realst.st_size > 0) {
        /* Copy the struct for the real file straight off */
        memcpy(buf, &cachefdinfo[fd].realst, sizeof(struct stat64));
        return 0;
    }

    return realfstat64(fd, buf);
}


static int realfstat64(int fd, struct stat64 *buf) {
    GET_REAL_SYMBOL(__fxstat64);
    return ___fxstat64(_STAT_VER, fd, buf);
}
#else /* __linux */
int fstat64(int fd, struct stat64 *buf) {

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fstat64 fd=%d\n", fd);
#endif

    if(fd < CACHE_MAXFD && cachefdinfo[fd].realst.st_size > 0) {
        /* Copy the struct for the real file straight off */
        memcpy(buf, &cachefdinfo[fd].realst, sizeof(struct stat64));
        return 0;
    }

    return realfstat64(fd, buf);
}


static int realfstat64(int fd, struct stat64 *buf) {
    GET_REAL_SYMBOL(fstat64);
    return _fstat64(fd, buf);
}
#endif /* __linux */


#ifndef WRAPPER_STAT_NOWRAP
int fstat(int fd, struct stat *buf) {

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: fstat fd=%d\n", fd);
#endif

    GET_REAL_SYMBOL(fstat);

    if(fd < CACHE_MAXFD && cachefdinfo[fd].realst.st_size > 0) {
        /* We can't memcpy the real file stat struct, different types */
        buf->st_dev = cachefdinfo[fd].realst.st_dev;
        buf->st_ino = cachefdinfo[fd].realst.st_ino;
        buf->st_mode = cachefdinfo[fd].realst.st_mode;
        buf->st_nlink = cachefdinfo[fd].realst.st_nlink;
        buf->st_uid = cachefdinfo[fd].realst.st_uid;
        buf->st_gid = cachefdinfo[fd].realst.st_gid;
        buf->st_rdev = cachefdinfo[fd].realst.st_rdev;
        buf->st_size = cachefdinfo[fd].realst.st_size;
        buf->st_blksize = cachefdinfo[fd].realst.st_blksize;
        buf->st_blocks = cachefdinfo[fd].realst.st_blocks;
        buf->st_atime = cachefdinfo[fd].realst.st_atime;
        buf->st_mtime = cachefdinfo[fd].realst.st_mtime;
        buf->st_ctime = cachefdinfo[fd].realst.st_ctime;
        return 0;
    }

    return _fstat(fd, buf);
}
#endif /* WRAPPER_STAT_NOWRAP */

#if defined(__sun) || defined(__linux)
ssize_t sendfile64(int out_fd, int in_fd, off64_t *off, size_t len) {
    struct stat64 st;
    off64_t realoff, avail;
    ssize_t amt, tot=0;
    int complete;


    GET_REAL_SYMBOL(sendfile64);

    if(off) {
        realoff = *off;
    }
    else {
        realoff = lseek64(in_fd, 0, SEEK_CUR);
        if(realoff == -1) {
            return -1;
        }
    }

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: sendfile64 outfd=%d infd=%d off=%lld "
                    "size=%zu\n", out_fd, in_fd, (long long)realoff, len);
#endif

    do {
        complete = cache_file_complete(in_fd, &st);
        if(complete == -1) {
            tot = -1;
            goto out;
        }
        else if(complete == 0) {
            avail = st.st_size - realoff;
            if(avail <= 0) {
#ifdef DEBUG
                fprintf(stderr, "httpcacheopen: sendfile64 outfd=%d infd=%d "
                                "off=%lld size=%zu: No data available\n", 
                                out_fd, in_fd, (long long)realoff, len);
#endif
                /* FIXME: Non-blocking based on the input file is probably
                   a really bad idea since it will probably spin like
                   crazy unless we catch poll/select... */
                int rc, flags = fcntl(out_fd, F_GETFL);
                if(flags < 0) {
                    return -1;
                }
                if(flags & O_NONBLOCK) {
#ifdef DEBUG
                    fprintf(stderr, "httpcacheopen: sendfile64 outfd=%d infd=%d"
                                    ": Would block\n", out_fd, in_fd);
#endif
                    errno = EAGAIN;
                    tot = -1;
                    goto out;
                }

                rc = wait_for_io(in_fd, realoff, &st);
                if(rc == -1) {
                    tot = -1;
                    goto out;
                }
                else if(rc == 0) {
                    errno = EIO;
                    tot = -1;
                    goto out;
                }
                avail = st.st_size - realoff;
            }
        }
        else {
            avail = len;
        }
        amt = _sendfile64(out_fd, in_fd, &realoff, MIN((off64_t)len,avail));
        if(amt == -1) {
            tot = -1;
            goto out;
        }
#ifdef DEBUG
        fprintf(stderr, "httpcacheopen: sendfile64 outfd=%d infd=%d off=%lld"
                ": sent %zd\n", out_fd, in_fd, (long long)realoff, amt);
#endif
        len -= amt;
        tot += amt;
        if(complete == 1) {
            /* If file is complete, we don't have to do extra stuff that
               sendfile doesn't */
            break;
        }
    } while(len > 0);

out:
#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: sendfile64 outfd=%d infd=%d: Done\n",
            out_fd, in_fd);
#endif
    if(off) {
        *off = realoff;
    }
    return tot;
}

#ifndef sendfile64
ssize_t sendfile(int out_fd, int in_fd, off_t *off, size_t len) {
    ssize_t rc;
    off64_t off64;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: sendfile outfd=%d infd=%d size=%zu\n",
            out_fd, in_fd, len);
#endif

    if(off == NULL) {
        return sendfile64(out_fd, in_fd, NULL, len);
    }

    off64 = *off;
    rc = sendfile64(out_fd, in_fd, &off64, len);
    *off = off64;
    return rc;
}
#endif
#endif /* defined(__sun) || defined(__linux) */

#ifdef __sun
ssize_t sendfilev64(int out_fd, const struct sendfilevec64 *sfv, int sfvcnt, 
                  size_t *xferred)
{
    ssize_t     amt=0;
    int         i;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: sendfilev64 outfd=%d sfvcnt=%d\n",
            out_fd, sfvcnt);
#endif

    *xferred = 0;

    for(i=0; i < sfvcnt; i++) {
        off64_t   off=sfv[i].sfv_off;

        amt = sendfile64(out_fd, sfv[i].sfv_fd, &off, sfv[i].sfv_len);
        if(amt == -1) {
            break;
        }
        *xferred += amt;
    }

    if(amt < 0) {
        return amt;
    }

    return *xferred;
}


#ifndef sendfilev64
ssize_t sendfilev(int out_fd, const struct sendfilevec *sfv, int sfvcnt, 
                  size_t *xferred)
{
    ssize_t     amt=0;
    int         i;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: sendfilev outfd=%d sfvcnt=%d\n",
            out_fd, sfvcnt);
#endif

    *xferred = 0;

    for(i=0; i < sfvcnt; i++) {
        off64_t   off=sfv[i].sfv_off;

        amt = sendfile64(out_fd, sfv[i].sfv_fd, &off, sfv[i].sfv_len);
        if(amt == -1) {
            break;
        }
        *xferred += amt;
    }

    if(amt < 0) {
        return amt;
    }

    return *xferred;
}
#endif /* sendfilev64 */
#endif /* __sun */

#ifdef _AIX
ssize_t send_file(int *out_fd, struct sf_parms *sf_iobuf, uint_t flags)
{
    struct stat64 st;
    off64_t avail;
    int complete;
    SF_INT64(file_bytes);
    SF_INT64(tosend);
    SF_UINT64(bytes_sent);
    void    *trailer_data;
    uint_t   trailer_length, tmpflags = flags;
    ssize_t ret = 0;

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: send_file outfd=%d\n", *out_fd);
#endif

    GET_REAL_SYMBOL(send_file);

    complete = cache_file_complete(sf_iobuf->file_descriptor, &st);
    if(complete == -1) {
        return -1;
    }
    else if(complete == 1) {
        /* File complete, take the easy way out */
        return _send_file(out_fd, sf_iobuf, flags);
    }

    /* File not complete. We need to do this in two steps:
       - First send header + file data.
       - When file complete, add back the trailer
     */

    trailer_data = sf_iobuf->trailer_data;
    trailer_length = sf_iobuf->trailer_length;
    trailer_data = NULL;
    trailer_length = 0;
    tmpflags &= ~SF_CLOSE;
    bytes_sent = 0;
    file_bytes = tosend = sf_iobuf->file_bytes;
    if(tosend == -1) {
        struct stat64 realst;
        if(fstat64(sf_iobuf->file_descriptor, &realst) < 0) {
            return -1;
        }
        tosend = realst.st_size - sf_iobuf->file_offset;
    }

    do {
        avail = st.st_size - sf_iobuf->file_offset;
        if(avail <= 0) {
            int rc = wait_for_io(sf_iobuf->file_descriptor,
                                 sf_iobuf->file_offset, &st);
            if(rc == -1) {
                ret = -1;
                goto out;
            }
            else if(rc == 0) {
                errno = EIO;
                ret = -1;
                goto out;
            }
            avail = st.st_size - sf_iobuf->file_offset;
        }
        avail = MIN(avail, tosend);
        sf_iobuf->file_bytes = avail;
        do {
            ret = _send_file(out_fd, sf_iobuf, tmpflags);
            bytes_sent += sf_iobuf->bytes_sent;
        } while(ret == 1);
        if(ret == -1) {
            goto out;
        }
        tosend -= avail;

#ifdef DEBUG
        fprintf(stderr,"httpcacheopen: send_file outfd=%d off=%llu sent=%llu: tosend=%lld\n", *out_fd, sf_iobuf->file_offset, bytes_sent, tosend);
#endif

        complete = cache_file_complete(sf_iobuf->file_descriptor, &st);
        if(complete == -1) {
            ret = -1;
            goto out;
        }
    } while(complete != 1);


out:
    /* Restore the trailer */
    sf_iobuf->trailer_data = trailer_data;
    sf_iobuf->trailer_length = trailer_length;

    if(file_bytes == -1) {
        sf_iobuf->file_bytes = file_bytes;
    }
    else {
        sf_iobuf->file_bytes = tosend;
    }

    if(complete == 1) {
        ret = send_file(out_fd, sf_iobuf, flags);
        bytes_sent += sf_iobuf->bytes_sent;
    }

#ifdef DEBUG
    fprintf(stderr,"httpcacheopen: send_file outfd=%d sent=%llu: done\n", 
                    *out_fd, bytes_sent);
#endif

    sf_iobuf->bytes_sent = bytes_sent;
    return ret;
}
#endif /* _AIX */

/* FIXME: 
    Ignored for now, doesn't seem to be used by vsftpd/rsync:
        - fget* 
        - mmap*
        - access, accessx
        - pathconf
        - popen
    Ignored, only needed for rw-access:
        - rename
        - unlink
        - mkdir
        - symlink
        - link
        - *chown
        - chmod
 */
#endif /* USE_COPYD */


/*
vim:ts=4:et:sw=4:
*/
