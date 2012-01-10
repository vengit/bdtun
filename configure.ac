#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.67])
AC_INIT([bdtun], [0.0.1], [Fábián Tamás László <tamas.fabian@vengit.com>])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(include/config.h)

# CFLAGS="-Wall -O2 -DBDTUN_DEBUG"
AC_ARG_ENABLE(debug,[  --enable-debug          Compile with debugging enabled ],)

if test "$enable_debug" == "yes"; then
    CFLAGS="-Wall -g -O -DBDTUN_DEBUG"
else
    CFLAGS="-Wall -O2"
fi

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h inttypes.h stdlib.h unistd.h string.h argp.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_REALLOC

#AC_CONFIG_SUBDIRS([example
#                   include
#                   kernel
#                   lib
#                   util])

AC_CONFIG_FILES([Makefile include/Makefile lib/Makefile util/Makefile example/Makefile kernel/Makefile])

AC_OUTPUT
