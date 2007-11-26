AC_INIT(dv4l, 0.95, dv4l-0.95.tar.gz)

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

AC_CONFIG_FILES([Makefile config.h])
AC_OUTPUT

