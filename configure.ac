dnl Process this file with autoconf (or run "autoreconf -i") to produce
dnl a configure script.
AC_PREREQ([2.69])
AC_INIT([vsplit], [2.0])
AC_CONFIG_SRCDIR([vsplit.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall])

AC_PROG_CC

dnl Record sides are a couple hundred MB, but let's not fall over on
dnl bigger files on 32-bit systems.
AC_SYS_LARGEFILE

AC_CHECK_HEADERS([getopt.h], [],
  [AC_MSG_ERROR([vsplit needs getopt.h])])
AC_CHECK_FUNCS([getopt_long], [],
  [AC_MSG_ERROR([vsplit needs getopt_long])])
AC_SEARCH_LIBS([log10], [m], [],
  [AC_MSG_ERROR([vsplit needs the math library])])

dnl The input is read as native-endian 16-bit samples, and audio files
dnl are little-endian.
AC_C_BIGENDIAN(
  [AC_MSG_WARN([big-endian host: vsplit expects samples in host byte order])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
