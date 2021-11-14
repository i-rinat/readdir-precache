# Copyright 2021  Rinat Ibragimov
# SPDX-License-Identifier: MIT

project('readdir-precache', 'c')

cc = meson.get_compiler('c')

dep_libdl = cc.find_library('dl')
dep_threads = dependency('threads')

common_c_args =  ['-Wall', '-Wextra', '-Werror', '-Wshadow',
                  '-Wimplicit-fallthrough', '-Wno-unused-parameter']

libprecache_c_args = ['-fvisibility=hidden']  # Hides implementation details.
libprecache_c_args += ['-U_FILE_OFFSET_BITS']  # Prevents macros from renaming readdir to readdir64.

library('precache',
        ['libprecache.c', 'encfs_mapper.c', 'intercepted_functions.c'],
        dependencies: [dep_libdl, dep_threads],
        c_args: common_c_args + libprecache_c_args)

executable('precache',
           ['precache.c', 'encfs_mapper.c', 'intercepted_functions.c',
            'segments.c', 'progress.c'],
           dependencies: [dep_libdl, dep_threads],
           c_args: common_c_args)

executable('precache-dir',
           ['precache_dir.c', 'encfs_mapper.c', 'intercepted_functions.c',
            'segments.c', 'progress.c'],
           dependencies: [dep_libdl, dep_threads],
           c_args: common_c_args)
