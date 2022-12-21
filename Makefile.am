if HAVE_PIPEWIRE
pkglib_LTLIBRARIES = ddb_out_pw.la
ddb_out_pw_la_SOURCES = pw.c
ddb_out_pw_la_LDFLAGS = -module -avoid-version
ddb_out_pw_la_CFLAGS = $(CFLAGS) $(PIPEWIRE_DEPS_CFLAGS) -std=c99
ddb_out_pw_la_LIBADD = $(LDADD) $(PIPEWIRE_DEPS_LIBS)
endif

