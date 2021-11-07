# Copyright 2021  Rinat Ibragimov
# SPDX-License-Identifier: MIT

project('readdir-precache', 'c')

cc = meson.get_compiler('c')

dep_libdl = cc.find_library('dl')
dep_threads = dependency('threads')

src = [ 'libprecache.c', 'encfs_mapper.c', 'intercepted_functions.c' ]

c_args =  ['-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wimplicit-fallthrough',
           '-Wno-unused-parameter']
c_args += ['-fvisibility=hidden']  # Hides implementation details.
c_args += ['-U_FILE_OFFSET_BITS']  # Prevents macros from renaming readdir to readdir64.

library('precache', src,
        dependencies: [dep_libdl, dep_threads],
        c_args: c_args)
