AC_INIT(dv4l, 1.0, dv4l-1.0.tar.gz)


AC_CHECK_HEADER([libraw1394/raw1394.h],
    [AC_DEFINE(HAVE_RAW1394, 1,
	[Define to 1 if you have libraw1394/raw1394.h])],
	[AC_MSG_ERROR([libraw1394 missing])])

AC_CHECK_HEADER([libiec61883/iec61883.h],
    [AC_DEFINE(HAVE_LIBIEC, 1,
	[Define to 1 if you have libiec61883/iec61883.h])],
	[AC_MSG_ERROR([libiec61883 missing])])

AC_CHECK_HEADER([libdv/dv.h],
    [AC_DEFINE(HAVE_LIBDV, 1,
	[Define to 1 if you have libdv/dv.h])],
	[AC_MSG_ERROR([libfv missing])])

AC_CHECK_HEADER([sys/xattr.h],
    [AC_DEFINE(HAVE_XATTR, 1,
	[Define to 1 if you have sys/xattr.h])],
	[AC_MSG_WARN([xattr missing, expect non-critical error messages])])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile version.h])
AC_OUTPUT

