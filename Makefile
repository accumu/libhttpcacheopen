#
# Copyright 2006-2019 Niklas Edmundsson <nikke@acc.umu.se>
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

uname := $(shell uname)

LIBCFLAGS = -DUSE_COPYD
OPT = -g -O2
GITVER=-DGIT_SOURCE_DESC='"$(shell git describe --tags --always --dirty)"'

# Setup variables
ifneq ($(uname),AIX)
	CC := gcc -pthread
	LIBCC := gcc
	CFLAGS := -std=c99 -W -Wall -Wextra $(EXTRACFLAGS) $(OPT) $(GITVER)
	LDFLAGS :=
	LIBFLAGS := -fPIC -shared -nostdlib -shared $(LIBCFLAGS)
	LIBS := -lgcc -lc -ldl
	LIBOBJECTS := libhttpcacheopen.so libhttpcacheopen.debug.so
else
	CC := xlc_r
	LIBCC := xlc
	CFLAGS := -qstrict_induction $(EXTRACFLAGS) $(OPT) $(GITVER)
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

BINOBJECTS := httpcachecopyd
BINDEPS := md5.c cleanpath.c cacheopen.c config.h Makefile

LIBDEPS := $(BINDEPS)

all: $(BINOBJECTS) $(LIBOBJECTS)

# Targets
$(BINOBJECTS): copyd.c $(BINDEPS)
	$(CC) $(CFLAGS) $(BINCFLAGS) $(LDFLAGS) -o $@ copyd.c

libhttpcacheopen.so: wrapper.c $(LIBDEPS)
	$(LIBCC) $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ wrapper.c $(LIBS)

libhttpcacheopen.debug.so: wrapper.c $(LIBDEPS)
	$(LIBCC) -DDEBUG $(CFLAGS) $(LIBFLAGS) $(LDFLAGS) -o $@ wrapper.c $(LIBS)

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
	rm -f $(BINOBJECTS) libhttpcacheopen*.so
