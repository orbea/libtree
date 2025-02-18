# v3.0.0-dev
- Rewritten in C99 without 0 external dependencies.
- Significantly faster & smaller (~50KB statically compiled with musl libc, or
  even smaller than the source file with diet libc).
- Improved search path printing when libraries cannot be located
- Improved rpath search: shows `[rpath of ...]` when lib is located by parent
  of parent ... of parent's rpath.
- `fd` inspired highlight of filename when printing paths
- Caches files by inode instead of soname, which is quite useful in the sense
  that this allows you to find broken libraries that only work because of a
  particular search order of the tree. (Consider an executable A and libraries
  B, C and D, where A depends on B and C, and B and C depend on D:
  
  ```
    B
   / \
  A   D
   \ /
    C
  ```

  It may happen that D *can* be located through B's rpath, but not through C's.
  Then, depending on whether A - B - D is traversed first, or A - C - D, glibc
  will complain about missing libraries or not. `libtree` on the other hand
  will always tell you that D can't be located through C.
- More verbosity levels `-v`, `-vv`, `-vvv` instead of `-a` and `-v` flags
- Skip fewer libraries by default (only libc / libstdc++ type of libs)
- `PLATFORM` rpath interpolation now uses uname, this is not always the same as
  `AT_PLATFORM`, but unlikely to be different, and in fact the feature is
  rarely used.
- support `NODEFLIB` flag
- Better FreeBSD support (`OSREL`, `OSNAME` interpolation in rpaths and
  `/etc/ld-elf.so.conf` config file support)

TODO list:
- Bundling

# v2.0.0

- No changes to the libtree API
- Provide static executables for ease of use on distros with an old glibc or
  musl.
- Dropped the dependency on `cppglob`, use posix `glob` instead.
- No more vendored dependencies, rely on `find_package` to find `cxxopts`,
  `elfio`, and `termcolor`.
