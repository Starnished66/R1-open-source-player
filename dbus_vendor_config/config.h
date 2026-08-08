/* Hand-written replacement for dbus's own generated config.h (normally
 * produced by its CMake build from configure-time feature detection --
 * this project doesn't run that build system at all, see Makefile's
 * DBUS_DIR section). Values below reflect what a standard Linux/musl
 * target on this mipsel-linux-musl-gcc toolchain actually has -- musl
 * implements the full set of modern POSIX/Linux calls dbus wants, with a
 * few explicit exceptions noted below (musl deliberately omits some
 * glibc-only extensions).
 *
 * Only defines what dbus's own .c/.h files actually consult -- see
 * cmake/config.h.cmake in the vendored source for the full menu this was
 * selected from. */
#ifndef _DBUS_CONFIG_H
#define _DBUS_CONFIG_H

#define DBUS_UNIX 1

/* Headers -- all standard, present in musl */
#define HAVE_ALLOCA_H 1
#define HAVE_BYTESWAP_H 1
#define HAVE_DIRENT_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_GRP_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_INOTIFY_H 1
#define HAVE_SYS_PRCTL_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1

/* musl deliberately doesn't implement these glibc-only extensions --
 * left undefined (not 0 -- dbus's own code just checks #ifdef, so an
 * explicit 0 would be wrong, plain omission is what "don't have it"
 * means here): HAVE_EXECINFO_H / HAVE_BACKTRACE (no backtrace() in
 * musl), HAVE_CLEARENV (not implemented by musl), HAVE_DDFD /
 * HAVE_CMSGCRED / HAVE_GETPEERUCRED (BSD/Solaris-only, not Linux at
 * all), HAVE_EXPAT_H (not vendoring expat -- see the Makefile comment;
 * this only disables dbus's own optional XML-introspection-from-file
 * helper, not the core client library). */

/* Symbols -- musl provides all of these */
#define HAVE_POLL 1
#define HAVE_SOCKETPAIR 1
#define HAVE_WRITEV 1
#define HAVE_SOCKLEN_T 1
#define HAVE_SETLOCALE 1
#define HAVE_LOCALECONV 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_PIPE2 1
#define HAVE_ACCEPT4 1
#define HAVE_DIRFD 1
#define HAVE_INOTIFY_INIT1 1
#define HAVE_GETRANDOM 1
#define HAVE_GETRLIMIT 1
#define HAVE_PRCTL 1
#define HAVE_PRLIMIT 1
#define HAVE_RAISE 1
#define HAVE_SETRLIMIT 1
#define HAVE_UNIX_FD_PASSING 1
#define HAVE_VASPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_GETGROUPLIST 1
#define HAVE_GETPWNAM_R 1
#define HAVE_NANOSLEEP 1
#define HAVE_SETRESUID 1
#define HAVE_GETRESUID 1

/* Use the plain poll()-based pollable-set, not epoll -- one less source
 * file to vendor/compile, and this app's own D-Bus usage (a handful of
 * fds: the bus connection, maybe a timeout) has no scale need for
 * epoll's O(1)-vs-poll()'s O(n) advantage that would matter here. */
/* #undef DBUS_HAVE_LINUX_EPOLL */

/* GCC/Clang __sync builtins -- this toolchain (mipsel-linux-musl-gcc)
 * supports them. */
#define DBUS_USE_SYNC 1

#define HAVE_DECL_ENVIRON 1
#define HAVE_DECL_LOG_PERROR 1
#define HAVE_DECL_MSG_NOSIGNAL 1

#define FD_SETSIZE 1024

/* mipsel is little-endian ("el") -- leave WORDS_BIGENDIAN undefined. */

/* Standard system default -- confirmed present on the real device via
 * `ps` (dbus-daemon --system uses this same path; bluetoothd/bluealsa
 * already connect to it successfully). */
#define DBUS_SYSTEM_BUS_DEFAULT_ADDRESS "unix:path=/var/run/dbus/system_bus_socket"
/* This app never connects to a session bus, but dbus-bus.c references
 * this unconditionally -- "autolaunch:" is upstream dbus's own standard
 * default. */
#define DBUS_SESSION_BUS_CONNECT_ADDRESS "autolaunch:"

/* Config-file/data-dir paths -- referenced by dbus-sysdeps-util-unix.c's
 * config-file-parsing helpers, which this app's own code never calls (no
 * dbus-daemon, no config parsing, no service activation), but the file
 * still needs these to compile. Standard upstream default locations. */
#define DBUS_DATADIR "/usr/share"
#define DBUS_SYSTEM_CONFIG_FILE "/etc/dbus-1/system.conf"
#define DBUS_SESSION_CONFIG_FILE "/etc/dbus-1/session.conf"
#define DBUS_MACHINE_UUID_FILE "/var/lib/dbus/machine-id"

/* Pointer size on 32-bit MIPS -- referenced by dbus-internals.c's
 * alignment assertions. */
#define DBUS_SIZEOF_VOID_P 4

/* printf/scanf length modifier for a 64-bit value, matching
 * dbus-arch-deps.h's choice of `long long` for dbus_int64_t above --
 * standard for any 32-bit (ILP32) Linux target, mipsel included. */
#define DBUS_INT64_MODIFIER "ll"

/* Matches dbus's own cmake/config.h.cmake template exactly (see
 * @DBUS_DISABLE_ASSERT@/@DBUS_DISABLE_CHECKS@ there): several struct
 * fields (DBusConnection.generation) and internal functions
 * (_dbus_message_iter_check) are compiled only `#if defined
 * (DBUS_ENABLE_CHECKS) || defined (DBUS_ENABLE_ASSERT)`, while their call
 * sites are NOT similarly guarded -- omitting this block (as an earlier
 * version of this file did) left those symbols undefined at their use
 * sites, confirmed live via mipsel-linux-musl-gcc failing on exactly
 * those two files with "has no member named generation" / implicit
 * declaration errors. */
#ifndef DBUS_DISABLE_ASSERT
#define DBUS_ENABLE_ASSERT 1
#endif
#ifndef DBUS_DISABLE_CHECKS
#define DBUS_ENABLE_CHECKS 1
#endif

/* No embedded tests, X11 autolaunch, or glib integration -- this is a
 * client-only build for one specific purpose (see bt_media_player.c),
 * not a general-purpose dbus install. */

#endif /* _DBUS_CONFIG_H */
