AC_PREREQ([2.63])

AC_INIT([realmd], [0.17.0],
        [https://gitlab.freedesktop.org/realmd/realmd/-/issues],
        [realmd])

# -----------------------------------------------------------------------------
# Versions

GLIB_REQ=2.36.0
GLIB_MIN=GLIB_VERSION_2_36
GLIB_MAX=GLIB_VERSION_2_36

INTLTOOL_REQ=0.35.0

# -----------------------------------------------------------------------------

AC_CONFIG_MACRO_DIR([build/m4])
AC_CONFIG_AUX_DIR([build])
AM_INIT_AUTOMAKE([1.11 tar-ustar foreign -Wno-portability subdir-objects])
AM_SANITY_CHECK
AC_CONFIG_HEADERS(config.h)
AC_CONFIG_SRCDIR([service/realm-daemon.c])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AM_MAINTAINER_MODE([enable])

# -----------------------------------------------------------------------------
# Distro Detection

AC_ARG_WITH([distro],
            [AS_HELP_STRING([--with-distro],
                            [Configure for a specific distribution (eg: redhat)]
                           )],
            [DISTRO=$withval],
            [DISTRO=])

if test -z $DISTRO; then
	AC_CHECK_FILE(/etc/redhat-release, [DISTRO="redhat"])
	AC_CHECK_FILE(/etc/debian_version, [DISTRO="debian"])
	AC_CHECK_FILE(/etc/SuSE-release, [DISTRO="suse"])

	# Not customized for these yet
	dnl AC_CHECK_FILE(/etc/gentoo-release, [DISTRO="gentoo"])
	dnl AC_CHECK_FILE(/etc/slackware-version, [DISTRO="slackware"])

fi

if test -z $DISTRO; then
	AC_MSG_ERROR([Couldn't detect the distro to configure for. Specify one with --with-distro])
fi

AC_SUBST(DISTRO)

# -----------------------------------------------------------------------------
# Vendor error message

AC_ARG_WITH([vendor-error-message],
              [AS_HELP_STRING([--with-vendor-error-message=ARG],
                            [Add a vendor specific error message shown if a realm command fails]
                           )],
              [AS_IF([test "x$withval" != "x"],
                     [AC_DEFINE_UNQUOTED([VENDOR_MSG],
                                         ["$withval"],
                                         [Vendor specific error message])],
                     [AC_MSG_ERROR([--with-vendor-error-message requires an argument])]
                    )],
              [])

# -----------------------------------------------------------------------------
# Basic tools

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_PROG_CPP
AM_PROG_CC_C_O
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_PROG_RANLIB
IT_PROG_INTLTOOL($INTLTOOL_REQ)
AC_PATH_PROG(PKG_CONFIG, pkg-config, no)

GETTEXT_PACKAGE=realmd
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[The gettext domain name])
AM_GLIB_GNU_GETTEXT

# --------------------------------------------------------------------
# GLib

PKG_CHECK_MODULES(GLIB,
	glib-2.0 >= $GLIB_REQ
	gio-2.0
	gio-unix-2.0)
GLIB_CFLAGS="$GLIB_CFLAGS -DGLIB_VERSION_MIN_REQUIRED=$GLIB_MIN -DGLIB_VERSION_MAX_ALLOWED=$GLIB_MAX"
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

# --------------------------------------------------------------------
# PolicyKit

PKG_CHECK_MODULES(POLKIT, polkit-gobject-1)
AC_SUBST(POLKIT_CFLAGS)
AC_SUBST(POLKIT_LIBS)

# --------------------------------------------------------------------
# systemd

AC_MSG_CHECKING([systemd unit directory])
AC_ARG_WITH(systemd-unit-dir,
            AS_HELP_STRING([--with-systemd-unit-dir],
                           [Directory to install systemd service file]))

if test "$with_systemd_unit_dir" = "" -o "$with_systemd_unit_dir" = "yes"; then
	with_systemd_unit_dir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)
	if test "$with_systemd_unit_dir" = ""; then
		AC_MSG_ERROR(["Couldn't find systemd system unit directory"])
	fi

elif test "$with_systemd_unit_dir" = "no"; then
	with_systemd_unit_dir=""

fi

SYSTEMD_UNIT_DIR="$with_systemd_unit_dir"
AC_SUBST(SYSTEMD_UNIT_DIR)
AM_CONDITIONAL(WITH_SYSTEMD, [test -n "$with_systemd_unit_dir"])

dbus_systemd_service=""
if test -n "$with_systemd_unit_dir"; then
	dbus_systemd_service="SystemdService=realmd.service"
fi
AC_SUBST(dbus_systemd_service)

AC_MSG_RESULT($with_systemd_unit_dir)

AC_ARG_WITH(systemd-journal,
            AS_HELP_STRING([--with-systemd-journal],
                           [Use systemd's journal for logging]))

if test "$with_systemd_journal" != "no"; then
	AC_DEFINE_UNQUOTED(WITH_JOURNAL, 1, [Use systemd's journal])
	PKG_CHECK_MODULES(SYSTEMD_JOURNAL, [libsystemd],,
	                  [PKG_CHECK_MODULES(SYSTEMD_JOURNAL, [libsystemd-journal])])
	with_systemd_journal="yes"
fi

AC_SUBST(SYSTEMD_JOURNAL_CFLAGS)
AC_SUBST(SYSTEMD_JOURNAL_LIBS)

# -------------------------------------------------------------------
# resolv

AC_MSG_CHECKING(for which library has res_query)
for lib in "" "-lresolv"; do
	saved_LIBS="$LIBS"
	LIBS="$LIBS $lib"
	AC_LINK_IFELSE([
		AC_LANG_PROGRAM([#include <resolv.h>],
		                [res_query (0, 0, 0, 0, 0)])
	],
	[ AC_MSG_RESULT(${lib:-libc}); have_res_query="yes"; break; ],
	[ LIBS="$saved_LIBS" ])
done
if test "$have_res_query" != "yes"; then
	AC_MSG_RESULT(no)
	AC_MSG_ERROR([Couldn't find the library for the res_query function])
fi

# -------------------------------------------------------------------
# Kerberos

if test "$KRB5_LIBS" != ""; then
	KRB5_PASSED_LIBS=$KRB5_LIBS
fi

if test "$KRB5_CFLAGS" != ""; then
	KRB5_PASSED_CFLAGS=$KRB5_CFLAGS
fi

AC_PATH_TOOL(KRB5_CONFIG, krb5-config)
AC_MSG_CHECKING(for working krb5-config)
if test -x "$KRB5_CONFIG"; then
	KRB5_CFLAGS="`$KRB5_CONFIG --cflags`"
	KRB5_LIBS="`$KRB5_CONFIG --libs`"
	AC_MSG_RESULT(yes)
else
	if test x$KRB5_PASSED_LIBS = x; then
		AC_MSG_ERROR(no. Please install MIT kerberos devel package)
	fi
fi

if test x$KRB5_PASSED_LIBS != x; then
	KRB5_LIBS=$KRB5_PASSED_LIBS
fi

if test x$KRB5_PASSED_CFLAGS != x; then
	KRB5_CFLAGS=$KRB5_PASSED_CFLAGS
fi

AC_ARG_VAR([KRB5_CFLAGS], [C compiler flags for kerberos, overriding krb5-config])dnl
AC_ARG_VAR([KRB5_LIBS], [linker flags for kerberos, overriding krb5-config])dnl

AC_SUBST(KRB5_CFLAGS)
AC_SUBST(KRB5_LIBS)

# -------------------------------------------------------------------
# LDAP

AC_CHECK_LIB(ldap, ldap_search, , [ldap_invalid=yes])
AC_CHECK_LIB(lber, ber_pvt_opt_on, , [ldap_invalid=yes])
AC_CHECK_HEADERS([ldap.h], , [ldap_invalid=yes])

if test "$ldap_invalid" = "yes"; then
	AC_MSG_ERROR(["Couldn't find OpenLDAP headers or libraries"])
fi

AC_CHECK_LIB(ldap, ldap_init_fd, [true], [
	AC_MSG_ERROR([Couldn't find ldap_init_fd function in libldap])
])

LDAP_LIBS=""
AC_CHECK_LIB(resolv, res_query, LDAP_LIBS="-lresolv")
LDAP_LIBS="$LDAP_LIBS -lldap -llber"
LDAP_CFLAGS=""

AC_SUBST(LDAP_LIBS)
AC_SUBST(LDAP_CFLAGS)

# -------------------------------------------------------------------
# Directories

DBUS_SERVICES_DIR="${datadir}/dbus-1/system-services"
AC_SUBST(DBUS_SERVICES_DIR)

DBUS_POLICY_DIR="${sysconfdir}/dbus-1/system.d"
AC_SUBST(DBUS_POLICY_DIR)

POLKIT_ACTION_DIR="${datadir}/polkit-1/actions"
AC_SUBST(POLKIT_ACTION_DIR)

# --------------------------------------------------------------------
# Compilation options

AC_MSG_CHECKING([whether to build documentation])
AC_ARG_ENABLE(doc,
              AS_HELP_STRING([--enable-doc],
                             [Disable building documentation])
             )

if test "$enable_doc" = "no"; then
	AC_MSG_RESULT($enable_doc)

else
	if test "$enable_doc" = ""; then
		disable_msg="(perhaps --disable-doc)"
	fi

	enable_doc="yes"
	AC_MSG_RESULT($enable_doc)

	AC_PATH_PROG([XSLTPROC], [xsltproc], [no])
	if test "$XSLTPROC" = "no"; then
		AC_MSG_ERROR([the xsltproc command was not found $disable_msg])
	fi

	AC_PATH_PROG([XMLTO], [xmlto], [no])
	if test "$XMLTO" = "no"; then
		AC_MSG_ERROR([the xmlto command was not found $disable_msg])
	fi

	AC_SUBST(XSLTPROC)
	AC_SUBST(XMLTO)
fi

AM_CONDITIONAL([ENABLE_DOC], [test "$enable_doc" = "yes"])

AC_MSG_CHECKING([whether to build with gcov testing])
AC_ARG_ENABLE([coverage],
              AS_HELP_STRING([--enable-coverage],
                             [Whether to enable coverage testing ]),
              [], [enable_coverage=no])
AC_MSG_RESULT([$enable_coverage])

if test "$enable_coverage" = "yes"; then
	if test "$GCC" != "yes"; then
		AC_MSG_ERROR(Coverage testing requires GCC)
	fi

	AC_PATH_PROG(GCOV, gcov, no)
	if test "$GCOV" = "no" ; then
		AC_MSG_ERROR(gcov tool is not available)
	fi

	AC_PATH_PROG(LCOV, lcov, no)
	if test "$LCOV" = "no" ; then
		AC_MSG_ERROR(lcov tool is not installed)
	fi

	AC_PATH_PROG(GENHTML, genhtml, no)
	if test "$GENHTML" = "no" ; then
		AC_MSG_ERROR(lcov's genhtml tool is not installed)
	fi

	CFLAGS="$CFLAGS -O0 -g -fprofile-arcs -ftest-coverage"
	LDFLAGS="$LDFLAGS -lgcov"
fi

AM_CONDITIONAL([WITH_COVERAGE], [test "$enable_coverage" = "yes"])
AC_SUBST(LCOV)
AC_SUBST(GCOV)
AC_SUBST(GENHTML)

AC_MSG_CHECKING([for debug mode])
AC_ARG_ENABLE(debug,
              AS_HELP_STRING([--enable-debug=no/default/yes],
                             [Turn on or off debugging])
             )

if test "$enable_debug" != "no"; then
	AC_DEFINE_UNQUOTED(WITH_DEBUG, 1, [Print debug output])
	AC_DEFINE_UNQUOTED(_DEBUG, 1, [In debug mode])
	CFLAGS="$CFLAGS -g"
fi
if test "$enable_debug" = "yes"; then
	debug_status="yes"
	CFLAGS="$CFLAGS -O0"
elif test "$enable_debug" = "no"; then
	debug_status="no"
	CFLAGS="$CFLAGS -O2"
	AC_DEFINE_UNQUOTED(G_DISABLE_ASSERT, 1, [Disable glib assertions])
else
	debug_status="default"
fi

AC_MSG_RESULT($debug_status)

if test "$GCC" = "yes"; then
	CFLAGS="$CFLAGS \
		-Wall -Wmissing-declarations \
		-Wmissing-prototypes -Wnested-externs -Wpointer-arith \
		-Wdeclaration-after-statement -Wformat=2 -Winit-self \
		-Waggregate-return -Wmissing-format-attribute"

	for option in -Wmissing-include-dirs -Wundef; do
		SAVE_CFLAGS="$CFLAGS"
		CFLAGS="$CFLAGS $option"
		AC_MSG_CHECKING([whether gcc understands $option])
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
				  [has_option=yes],
				  [has_option=no])
		AC_MSG_RESULT($has_option)
		if test $has_option = no; then
			CFLAGS="$SAVE_CFLAGS"
		fi
	done

	# Only add this when optimizing is enabled (default)
	AC_MSG_CHECKING([whether optimization is enabled])
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#if __OPTIMIZE__ == 0
	                                     #error No optimization
	                                     #endif
	                                   ]], [[]])],
	                  [has_optimization=yes],
	                  [has_optimization=no])
	if test $has_optimization = yes; then
		CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2"
		AC_MSG_RESULT([yes, fortifying source])
	else
		AC_MSG_RESULT([no, not fortifying source])
	fi
fi

AC_ARG_ENABLE(strict, [
               AS_HELP_STRING([--enable-strict], [Strict code compilation])
             ])

AC_MSG_CHECKING([build strict])

if test "$enable_strict" = "yes"; then
	CFLAGS="$CFLAGS -Werror \
		-DG_DISABLE_DEPRECATED"
	TEST_MODE="thorough"
else
	CFLAGS="$CFLAGS -Wno-error"
	TEST_MODE="quick"
	enable_strict="no"
fi

AC_MSG_RESULT($enable_strict)
AC_SUBST(TEST_MODE)

# ------------------------------------------------------------------------------
# Results

privatedir='${prefix}/lib/realmd'
AC_MSG_CHECKING([private directory])
AC_ARG_WITH(private-dir,
            AS_HELP_STRING([--with-private-dir=DIR],
                           [Directory to install realmd system defaults (default: ${prefix}/lib/realmd)]))

if test -n "$with_private_dir"; then
	privatedir="$with_private_dir"
fi
AC_SUBST(privatedir)
AC_MSG_RESULT($privatedir)

cachedir='${localstatedir}/cache/realmd'
AC_SUBST(cachedir)

AC_CONFIG_FILES([
	Makefile
	doc/version.xml
	po/Makefile.in
	po/Makefile
])
AC_OUTPUT

echo
echo "CFLAGS: $CFLAGS"
echo
echo "OPTIONS:"
echo "  Debug:                $debug_status"
echo "  Coverage:             $enable_coverage"
echo "  Strict:               $enable_strict"
echo "  Documentation:        $enable_doc"
echo
echo "OPTIONAL DEPENDENCIES:"
echo "  systemd unit dir:     $with_systemd_unit_dir"
echo "  systemd journal:      $with_systemd_journal"
echo
