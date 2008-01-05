static const char rcsid[] = /*Add RCS version string to binary */
        "$Id$";

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


#include "config.h"
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

    oflag = O_RDONLY | O_DIRECT;
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

    cachefd = cacheopen(&cachest, &realst, oflag, cachepath, open);
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
                  open, stat64);
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
                exit(4);
            }
        }
        else if (rc == -1 && (errno == EINTR || errno == ECONNABORTED || errno == ENOTCONN)) {
            continue;
        }

    } while(rc != -1);

    perror("copyd: accept");
    exit(5);
}
