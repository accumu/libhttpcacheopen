# $Id: Makefile,v 1.9 2008/05/10 11:11:01 source Exp $
uname := $(shell uname)

LIBCFLAGS = -DUSE_COPYD
OPT = -g -O2

# Setup variables
ifneq ($(uname),AIX)
	CC := gcc -pthread
	LIBCC := gcc
	CFLAGS := -std=c99 -W -Wall $(EXTRACFLAGS) $(OPT)
	LDFLAGS :=
	LIBFLAGS := -fPIC -shared -nostdlib -shared $(LIBCFLAGS)
	LIBS := -lgcc -lc -ldl
	LIBOBJECTS := libhttpcacheopen.so libhttpcacheopen.debug.so
else
	CC := xlc_r
	LIBCC := xlc
	CFLAGS := -qstrict_induction $(EXTRACFLAGS) $(OPT)
	LDFLAGS := -brtl
	LIBFLAGS := -bexpall -bM:SRE -bnoentry $(LIBCFLAGS)
	LIBS :=
	LIBOBJECTS := libhttpcacheopen.64.so libhttpcacheopen.debug.64.so libhttpcacheopen.32.so libhttpcacheopen.debug.32.so
endif

ifeq ($(uname),SunOS)
	# Solaris specific libs
	LDFLAGS := $(LDFLAGS) -lrt # for nanosleep
	LDFLAGS := $(LDFLAGS) -lsocket # for socket stuff
	LDFLAGS := $(LDFLAGS) -lsendfile # for sendfile*()
endif

LIBDEPS := md5.c cleanpath.c cacheopen.c config.h Makefile.new

all: httpcachecopyd $(LIBOBJECTS)

# Targets
httpcachecopyd: copyd.c cacheopen.c md5.c config.h Makefile
	$(CC) $(CFLAGS) $(BINCFLAGS) $(LDFLAGS) -o $@ copyd.c

libhttpcacheopen.so: wrapper.c $(LIBDEPS)
	$(LIBCC) $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c

libhttpcacheopen.debug.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -DDEBUG $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c

# 32/64bit variants (currently only AIX)
libhttpcacheopen.32.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -q32 $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c

libhttpcacheopen.debug.32.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -q32 -DDEBUG $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c

libhttpcacheopen.64.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -q64 $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c

libhttpcacheopen.debug.64.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -q64 -DDEBUG $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ $(LIBS) wrapper.c


clean:
	rm -f httpcachecopyd $(LIBOBJECTS)
