NULL=
TESTS =	\
	defncopy$(EXEEXT) \
	$(NULL)

check_PROGRAMS	=	$(TESTS)

defncopy_SOURCES	=	defncopy.c

AM_CPPFLAGS	=	-I$(top_srcdir)/include -I$(srcdir)/.. -I../ -DFREETDS_TOPDIR=\"$(top_srcdir)\"
LDADD		=	../../utils/unittests/libtds_test_base.a \
			../../replacements/libreplacements.la $(LTLIBICONV) \
			$(NETWORK_LIBS)
EXTRA_DIST	=	CMakeLists.txt

