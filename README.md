# Introduction

This preload library allows standalone applications to share a cache
with the ACC `mod_cache_disk_largefile` Apache HTTPD module, available
at https://github.com/accumu/mod_cache_disk_largefile

See `mod_cache_disk_largefile` for background, setup and other technical
information.

Developed to meet the needs of http://ftp.acc.umu.se/ - the file archive of
Academic Computer Club (ACC), Umeå University, Sweden. The archive hosts
all kinds of Open Source software ranging from small archive files to
large DVD images.

This module is used to make rsync and vsftpd leverage the cache used
to deliver http and https. The caching is cooperative, meaning that files
cached using http is seen by applications using this module, and vice versa.

It was designed to be used together with `mod_cache_disk_largefile` but can in
fact be used stand-alone as well. Just remember the housekeeping to clean
out old files from your cache!

This work eventually became a part of the Master's thesis *Scaling a Content
Delivery system for Open Source Software*, available at
http://urn.kb.se/resolve?urn=urn:nbn:se:umu:diva-109779

# Setup/config

Tailor `config.h` to match your `mod_cache_disk_largefile` setup.

All processes using the cache must run as the same user, set `COPYD_USER`
to that username.

Do note that this library allows separating small and large files into separate
cache hierarchies. If you do not want this feature, simply define the same
directory name for small and large files. The historical reason for this
feature is this library being used to serve ftp and rsync on frontend hosts
that also served small files via http, and there was a need to prevent large
rsync and ftp sessions to flush all http files out of the cache.

**NOTE** that chroot is emulated by this library, otherwise accessing
a cache outside of the chroot would be impossible!

# Building

Once upon a time built and used on AIX, Solaris and Linux.

Recently only built and used on Ubuntu Linux 64bit flavours.

`make`

# Installation

Copy libhttpcacheopen\*.so to a suitable lib directory and httpcachecopyd to a
suitable sbin directory. 

# Using

Ensure that `httpcachecopyd` is started on system startup, preferably before
any processes using `libhttpcacheopen` is started.

Ensure that all processes have access to the cache hierarchy and your
configured `SOCKPATH`.

Start the processes that should be cache-enabled with `libhttpcacheopen.so` 
preloaded. Ie, set the environment variable `LD_PRELOAD` to the complete
path of `libhttpcacheopen.so`.

This can be done in various ways depending on how your services are started,
from prepending the path of the command line with
`env LD_PRELOAD=/path/to/libhttpcacheopen.so` to an env declaration in the
xinetd configuration for that service.

# Debugging

Preload `libhttpcacheopen.debug.so` instead. It will print a lot of stuff,
depending on the service preloaded it might end up anywhere from a log file
to inline in your connection.
