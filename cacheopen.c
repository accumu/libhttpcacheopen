
static const char cacheopenrcsid[] = /*Add RCS version string to binary */
        "$Id$";

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

static copy_status copy_file(int srcfd, off64_t len, time_t mtime, 
                             char *destfile,
                             int (*openfunc)(const char *, int, ...),
                             int (*statfunc)(const char *, struct stat64 *))
{
    int                 destfd, srcflags, modflags;
    char                *buf;
    ssize_t             amt, wrt, done;
    struct utimbuf      ut;
    copy_status         rc = COPY_OK;

    destfd = open_new_file(destfile, openfunc, statfunc);
    if(destfd < 0) {
        return(destfd);
    }

    if(posix_memalign((void **) &buf, 512, CPBUFSIZE)) {
        close(destfd);
        return(COPY_FAIL);
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
        modflags |= O_DIRECT;
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

    if((close(destfd)) == -1) {
#ifdef DEBUG
        perror("httpcacheopen: copy_file: close destfd");
#endif
        unlink(destfile);
        rc = COPY_FAIL;
    }
    else {
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


/* On success, fills in cachest */
static int cacheopen(struct stat64 *cachest, int realfd, struct stat64 *realst, 
                     const char *realpath, int oflag,
                     int (*openfunc)(const char *, int, ...),
                     int (*statfunc)(const char *, struct stat64 *))
{
    unsigned long long  inode, device;
    int                 cachefd;
    char                devinostr[34];
    char                cachepath[PATH_MAX];

    if(strncmp(realpath, backend_root, backend_len)) {
        /* Not a backend file, ignore */
#ifdef DEBUG
        fprintf(stderr, "cacheopen: Not a backend file: %s\n", realpath);
#endif
        return -1;
    }

    /* Only cache regular files larger than 0 bytes */
    if(!S_ISREG(realst->st_mode) || realst->st_size == 0) {
        return -1;
    }

    /* Hash on device:inode to eliminate file duplication. Since we only
       can serve plain files we don't have to bother with all the special
       cases in mod_disk_cache :) */
    device = realst->st_dev; /* Avoid ifdef-hassle with types */
    inode  = realst->st_ino;
    snprintf(devinostr, sizeof(devinostr), "%016llx:%016llx", device, inode);

    /* Calculate cachepath */
    strcpy(cachepath, cache_root);
    cache_hash(devinostr, cachepath+cache_len, DIRLEVELS, DIRLENGTH);
    strcat(cachepath, CACHE_BODY_SUFFIX);

    cachefd = openfunc(cachepath, oflag);
    if(cachefd == -1) {
        if((copy_file(realfd, realst->st_size, realst->st_mtime, cachepath, 
                      openfunc, statfunc)) == -1) 
        {
#ifdef DEBUG
            perror("cacheopen: copy_file failed");
#endif
            return -1;
        }
        /* We have either copied the file, or another process is already
         * caching the file and we need to wait for it to finish */
        cachefd = openfunc(cachepath, oflag);
        if(cachefd == -1) {
#ifdef DEBUG
            perror("cacheopen: open cachefd after copy_file 1");
#endif
            return -1;
        }
    }

    if(fstat64(cachefd, cachest) == -1) {
#ifdef DEBUG
        perror("cacheopen: Unable to fstat cachefd");
#endif
        /* Oh well, fallback to use the real file */
        close(cachefd);
        return -1;
    }

    if(realst->st_mtime > cachest->st_mtime ||
            (realst->st_size != cachest->st_size && 
               cachest->st_mtime < time(NULL) - CACHE_UPDATE_TIMEOUT)) 
    {
        /* Bollocks, the cached file is stale */
        close(cachefd);
        if((copy_file(realfd, realst->st_size, realst->st_mtime, cachepath,
                      openfunc, statfunc)) == -1) 
        {
#ifdef DEBUG
            perror("cacheopen: copy_file failed 2");
#endif
            return -1;
        }
        cachefd = openfunc(cachepath, oflag);
        if(cachefd == -1) {
#ifdef DEBUG
            perror("cacheopen: open cachefd after copy_file 2");
#endif
            return -1;
        }
        if(fstat64(cachefd, cachest) == -1) {
#ifdef DEBUG
            perror("cacheopen: Unable to fstat cachefd 2");
#endif
            close(cachefd);
            return -1;
        }
    }

#ifdef DEBUG
    fprintf(stderr, "cacheopen: Success, returning cached fd %d (%s)\n", 
            cachefd, cachepath);
#endif

    return cachefd;
}


