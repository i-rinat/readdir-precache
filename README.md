readdir-precache
================

Readdir-precache is a helper for serializing HDD access. It may help reduce
noise and increase speeds when copying a large number of small files from HDDs.

Usually, copying a large number of files is performed in order, one file at a
time. Even if `file_3` can be adjacent to `file_1` on disk, a copying software
will try to copy `file_2` before it even considers accesing `file_3`. That
causes a lot of HDD head movements and a lot of access latency, which translate
to lower speeds and higher noise levels.

Readdir-precache intercepts libc functions and tries to guess when a software
copies files in bulk. It then peeks into a list of next to be copied files,
determines where they are located on disk and reads them in sorted order. As
Linux keeps just read data in its cache, when a software does read files,
actuall data is served from the already populated cache in RAM.

Implemented as a library that needs to preloaded (`LD_PRELOAD`) into a
process. Targeting primarily Midnight Commander and its way of accessing
files. Should work for files on ext4 filesystems, but may work with other
filesystems which have FIEMAP ioctl implemented.
