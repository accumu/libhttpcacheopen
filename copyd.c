static const char rcsid[] = /*Add RCS version string to binary */
        "$Id: copyd.c,v 1.4 2008/08/11 23:25:51 source Exp source $";

#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600
#define _ALL_SOURCE 1

#define _LARGEFILE64_SOURCE 1
#define _LARGE_FILE_API 1

#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pwd.h>


#include "config.h"

#define IS_COPYD
#ifdef USE_COPYD
#error Source setup error - can not use and be copyd at the same time
#endif

#include "cleanpath.c"
#include "cacheopen.c"

static int debug=1;

void *handle_conn(void * arg) {
    int fd = (int) arg;
    char buf[PATH_MAX], cachepath[PATH_MAX];
    ssize_t amt;
    int realfd = -1, cachefd = -1, oflag;
    struct stat64 realst, cachest;

    if(debug) {
        fprintf(stderr, "copyd: handle_conn: fd=%d\n", fd);
    }

    amt = read(fd, buf, sizeof(buf)-1);
    if(amt == -1) {
        perror("handle_conn: read");
        goto err;
    }
    else if(amt == 0) {
        if(debug) {
            fprintf(stderr, "copyd: handle_conn: read: no data\n");
        }
        goto err;
    }

    /* We assume a null terminated value as input, make sure we don't travel
       outside our buffer */
    buf[sizeof(buf)-1] = '\0';

    /* Clean it from . .. // */
    cleanpath(buf);

    if(debug) {
        fprintf(stderr, "copyd: handle_conn: buf=%s\n", buf);
    }

    if(cacheopen_check(buf) == -1) {
        if(debug) {
            fprintf(stderr, "copyd: cacheopen_check fail\n");
        }
        goto err;
    }

    oflag = O_RDONLY
#ifdef O_DIRECT
        | O_DIRECT
#endif
        /* FIXME: Use directio() on solaris */
        ;
    realfd = open(buf, oflag);
    if(realfd == -1) {
        if(debug) {
            perror("open realfd");
        }
        goto err;
    }

    if(fstat64(realfd, &realst) == -1) {
        if(debug) {
            perror("fstat64 realfd");
        }
        goto err;
    }

    cacheopen_prepare(&realst, cachepath);

    cachefd = cacheopen(&cachest, &realst, oflag, cachepath, open, fstat64,
                        close);
    if(cachefd == CACHEOPEN_DECLINED) {
        if(debug) {
            fprintf(stderr, "cacheopen DECLINED\n");
        }
        goto err;
    }

    /* Write reply when we're pretty sure this will work in order not to pause
       requesting process until we're finished */
    write(fd, "OK", 3);
    close(fd);
    fd = -1;

    if(cachefd == CACHEOPEN_FAIL || cachefd == CACHEOPEN_STALE) {
        /* Either no cached file or stale cached file */
        copy_file(realfd, oflag, realst.st_size, realst.st_mtime, cachepath, 
                  open, stat64, read, close);
    }

    goto ok;

err:
    write(fd, "FAIL", 5);
ok:
    if(fd >= 0) {
        close(fd);
    }
    if(realfd >= 0) {
        close(realfd);
    }
    if(cachefd >= 0) {
        close(cachefd);
    }
    fprintf(stderr, "copyd: handle_conn: done\n");
    return NULL;
}

int main(void) {
    struct sockaddr_un sa;
    int sock, rc;
    socklen_t salen;
    pthread_attr_t attr;
    struct passwd *pw;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCLD, SIG_IGN);

    if(pthread_attr_init(&attr) != 0) {
        perror("copyd: pthread_attr_init");
        exit(1);
    }

    if(pthread_attr_setstacksize(&attr, 131072) != 0) {
        perror("copyd: pthread_attr_setstacksize");
        exit(1);
    }

    if(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        perror("copyd: pthread_attr_setdetachstate");
        exit(1);
    }

    /* Clean up old debris */
    unlink(SOCKPATH);

    /* Create the socket and stuff */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock == -1) {
        perror("copyd: socket");
        exit(1);
    }

    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCKPATH);

    if(bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("copyd: bind");
        exit(2);
    }

    if(listen(sock, 1024) != 0) {
        perror("copyd: listen");
        exit(3);
    }

    /* Get UID/GID of user to run as */
    pw = getpwnam(COPYD_USER);
    if(pw == NULL) {
        fprintf(stderr, "copyd: No such COPYD_USER %s\n", COPYD_USER);
        exit(4);
    }

    /* Change owner of the socket to our running user */
    if(chown(SOCKPATH, pw->pw_uid, pw->pw_gid) != 0) {
        perror("chown " SOCKPATH);
        exit(4);
    }
    if(chmod(SOCKPATH, 0600) != 0) {
        perror("chmod " SOCKPATH);
        exit(4);
    }

    /* Change to the user we'll run as */
    if(setregid(pw->pw_gid, pw->pw_gid) < 0) {
        perror("setregid");
        exit(5);
    }

    if(setreuid(pw->pw_uid, pw->pw_uid) < 0) {
        perror("setreuid");
        exit(6);
    }

    if(debug) {
        fprintf(stderr, "copyd: Init done\n");
    }

    do {
        salen = sizeof(sa);
        rc = accept(sock, (struct sockaddr *)&sa, &salen);
        if(debug) {
            fprintf(stderr, "copyd: accept rc=%d errno=%d\n", rc, errno);
        }
        if(rc >= 0) {
            pthread_t thr;

            if(pthread_create(&thr, &attr, handle_conn, (void *) rc) != 0) {
                perror("pthread_create");
                exit(25);
            }
        }
        else if (rc == -1 && (errno == EINTR || errno == ECONNABORTED || errno == ENOTCONN)) {
            continue;
        }

    } while(rc != -1);

    perror("copyd: accept");
    exit(99);
}
