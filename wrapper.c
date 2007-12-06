#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600
#define _ALL_SOURCE 1

#define _LARGEFILE64_SOURCE 1
#define _LARGE_FILE_API 1

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

#include "config.h"
#include "cleanpath.c"
#include "cacheopen.c"

static const char rcsid[] = /*Add RCS version string to binary */
        "$Id: httpcacheopen.c,v 1.7 2007/04/26 10:40:29 source Exp source $";


/* Declarations for the real functions that we override */
static int (*_open)(const char *, int, ...);
static FILE *(*_fopen)(const char *, const char *);
static FILE *(*_fopen64)(const char *, const char *);
static int (*_chdir)(const char *);
static char *(*_getcwd)(char *, size_t);
static int (*_stat)(const char *, struct stat *);
static int (*_stat64)(const char *, struct stat64 *);
static int (*_lstat)(const char *, struct stat *);
static int (*_lstat64)(const char *, struct stat64 *);
static int (*_readlink)(const char *, char *, size_t);

#define GET_REAL_SYMBOL(a) \
if(! _##a) { \
    (void *) _##a = dlsym( RTLD_NEXT, #a ); \
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
    struct timespec     delay;
    char                realpath[PATH_MAX];


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

    if(fstat64(realfd, &realst) == -1) {
#ifdef DEBUG
            perror("Unable to fstat realfd");
#endif
        /* Somewhere, something went very wrong */
        return -1;
    }

    if(realst.st_size > MAX_COPY_SIZE) {
#ifdef DEBUG
        fprintf(stderr, "open: Filesize over copy sizelimit\n");
#endif
        return(realfd);
    }

    /* If we get here, there are possibillities to use a cached copy of the
       file in the httpcache instead */

    GET_REAL_SYMBOL(stat64);
    cachefd = cacheopen(&cachest, realfd, &realst, realpath, oflag, _open, _stat64);
    if(cachefd == -1) {
        return(realfd);
    }

    /* cacheopen() fills in cachest for us */
    while(realst.st_size != cachest.st_size) {
        if(cachest.st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT) {
#ifdef DEBUG
            fprintf(stderr, 
                    "open: Timed out waiting for cached file\n");
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
            perror("open: Unable to fstat cachefd 3");
#endif
            close(cachefd);
            return(realfd);
        }
    }

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
    GET_REAL_SYMBOL(stat64);
    if((_stat64(newdir, &st)) == -1) {
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

#ifdef DEBUG
    fprintf(stderr, "httpcacheopen: chroot: chrootdir=%s\n", chrootdir);
#endif

    return(0);
}


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


int stat64(const char *path, struct stat64 *buffer) {
    char realpath[PATH_MAX];

    GET_REAL_SYMBOL(stat64);

    /* Do it the quick way if not chroot */
    if(chrootdir == NULL) {
        return _stat64(path, buffer);
    }

    if(strlen(path) +1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(get_full_path(realpath, path) == -1) {
        return -1;
    }

    return _stat64(realpath, buffer);
}


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


int readlink(const char *path, char *buffer, size_t buffersize) {
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


/* Stuff som behöver lagas om man ska chroot-emulera:
   chroot - lagra virtuella roten
   open*, fopen*, stat*, lstat*, chdir, getcwd, readlink: prepend:a virtuella roten
   access, accessx: behövs?
   pathconf: behövs??
   rename, unlink, mkdir, symlink, link, *chown, chmod - behövs bara fixas vid rw-access?
   popen?

done:
chroot, chdir, getcwd
open, open64, fopen, fopen64
stat, stat64, lstat, lstat64, readlink

*/

/* chrootdir == vart vi har chroot:at
   cachedwd  == cwd (utan chroot-biten)
   */
