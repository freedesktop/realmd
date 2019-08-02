NULL =

ACLOCAL_AMFLAGS = -I build/m4 ${ACLOCAL_FLAGS}

SUBDIRS = po

DISTCHECK_CONFIGURE_FLAGS = \
	--enable-debug=yes \
	--disable-coverage \
	--disable-silent-rules \
	--with-systemd-unit-dir=$(abs_srcdir)/$(top_distdir)/_inst/ \
	--enable-doc

dist-hook: dist-doc-hook
	@if test -d "$(srcdir)/.git"; \
	then \
		echo Creating ChangeLog && \
		( cd "$(top_srcdir)" && \
		  echo '# Generate automatically. Do not edit.'; echo; \
		  $(top_srcdir)/missing --run git log --stat --date=short ) > ChangeLog.tmp \
		&& mv -f ChangeLog.tmp $(top_distdir)/ChangeLog \
		|| ( rm -f ChangeLog.tmp ; \
		     echo Failed to generate ChangeLog >&2 ); \
	else \
		echo A git clone is required to generate a ChangeLog >&2; \
	fi

upload-release: $(DIST_ARCHIVES)
	gpg --detach-sign --local-user 'stef@thewalter.net' $(DIST_ARCHIVES)
	scp $(DIST_ARCHIVES) $(DIST_ARCHIVES).sig anarchy.freedesktop.org:/srv/www.freedesktop.org/www/software/realmd/releases

transifex: po/realmd.pot
	sh $(srcdir)/build/tx-update $<

V_SED = $(V_SED_$(V))
V_SED_ = $(V_SED_$(AM_DEFAULT_VERBOSITY))
V_SED_0 = @echo "  SED   " $@;

SED_SUBST = sed \
	-e 's,[@]datadir[@],$(datadir),g' \
	-e 's,[@]libexecdir[@],$(libexecdir),g' \
	-e 's,[@]libdir[@],$(libdir),g' \
	-e 's,[@]includedir[@],$(includedir),g' \
	-e 's,[@]datarootdir[@],$(datarootdir),g' \
	-e 's,[@]sysconfdir[@],$(sysconfdir),g' \
	-e 's,[@]bindir[@],$(bindir),g' \
	-e 's,[@]exec_prefix[@],$(exec_prefix),g' \
	-e 's,[@]prefix[@],$(prefix),g' \
	-e 's,[@]abs_srcdir[@],$(abs_srcdir),g' \
	-e 's,[@]srcdir[@],$(srcdir),g' \
	-e 's,[@]privatedir[@],$(privatedir),g' \
	-e 's,[@]dbus_systemd_service[@],$(dbus_systemd_service),g' \
	-e 's,[@]PACKAGE[@],$(PACKAGE),g' \
	-e 's,[@]VERSION[@],$(VERSION),g' \
	$(NULL)

.desktop.in.in.desktop.in:
	$(V_SED) $(MKDIR_P) $(dir $@) && $(SED_SUBST) $< > $@

@INTLTOOL_DESKTOP_RULE@

.service.in.service:
	$(V_SED) $(MKDIR_P) $(dir $@) && $(SED_SUBST) $< > $@

.conf.in.conf:
	$(V_SED) $(MKDIR_P) $(dir $@) && $(SED_SUBST) $< > $@

SUFFIXES = .conf .conf.in .desktop.in .desktop.in.in .service .service.in

BUILT_SOURCES =
DISTCLEANFILES =
TESTS =

check_PROGRAMS =
man5_MANS =
man8_MANS =
noinst_LIBRARIES =
noinst_PROGRAMS =
private_PROGRAMS =
private_DATA =
sbin_PROGRAMS =

CLEANFILES = $(BUILT_SOURCES)

EXTRA_DIST =

include dbus/Makefile.am
include service/Makefile.am
include tests/Makefile.am
include tools/Makefile.am

install-data-local: install-service install-doc
	@true
uninstall-local: uninstall-service uninstall-doc
	@true
clean-local: clean-doc
	find $(builddir) -name '*.gc??' -delete

# ------------------------------------------------------------
# Docs
#

if ENABLE_DOC

include doc/manual/Makefile.am
include doc/internals/Makefile.am
include doc/website/Makefile.am

EXTRA_DIST += \
	doc/version.xml.in \
	doc/version.xml

DISTCLEANFILES += \
	doc/website/Makefile

upload-website: website manual/index.html internals/realmd-internals.html
	rsync -Hvax manual/./ anarchy.freedesktop.org:/srv/www.freedesktop.org/www/software/realmd/docs/./
	rsync -Hvax internals/./ anarchy.freedesktop.org:/srv/www.freedesktop.org/www/software/realmd/internals/./
	rsync -Hvax --exclude docs --exclude internals --exclude releases \
		html/* anarchy.freedesktop.org:/srv/www.freedesktop.org/www/software/realmd/

dist-doc-hook: manual/index.html
	@mkdir $(distdir)/manual
	@cp ./manual/* $(distdir)/manual

all-local: manual/index.html internals/realmd-internals.html

clean-doc:
	@rm -rf *.tmp manual/ internals/

install-doc:
	$(MKDIR_P) $(DESTDIR)$(htmldir)
	$(INSTALL_DATA) $(builddir)/manual/* $(DESTDIR)$(htmldir)

uninstall-doc:
	rm -rf $(DESTDIR)$(htmldir)

distcheck-hook:
	@true

else
upload:
	@echo "*** doc must be enabled  (and --enable-doc) in order to make upload"
	@false
distcheck-hook:
	@echo "*** doc must be enabled (and --enable-doc) in order to make distcheck"
	@false
dist-doc-hook:
	@echo "*** doc must be enabled (and --enable-doc) in order to make dist"
	@false
clean-doc:
	@true
install-doc:
	@true
uninstall-doc:
	@true
endif

# ------------------------------------------------------------
# Testing
#

LOG_DRIVER = $(top_srcdir)/build/tap-driver
LOG_COMPILER = sh -c '"$$0" "$$@" --tap'

VALGRIND_ARGS = --trace-children=no --quiet --error-exitcode=33 \
	--suppressions=valgrind-suppressions --gen-suppressions=all \
	--num-callers=16

VALGRIND_SUPPRESSIONS = \
	build/glib.supp \
	$(NULL)

valgrind-suppressions: $(VALGRIND_SUPPRESSIONS)
	$(AM_V_GEN) cat $^ > $@

check-memory: valgrind-suppressions
	$(MAKE) LOG_FLAGS="-- libtool --mode=execute valgrind $(VALGRIND_ARGS)" \
		$(AM_MAKEFLAGS) check
recheck-memory: valgrind-suppressions
	$(MAKE) LOG_FLAGS="-- libtool --mode=execute valgrind $(VALGRIND_ARGS)" \
		$(AM_MAKEFLAGS) recheck

EXTRA_DIST += \
	$(LOG_DRIVER) \
	$(VALGRIND_SUPPRESSIONS) \
	$(NULL)

if WITH_COVERAGE
coverage:
	mkdir -p $(top_builddir)/build/coverage
	$(LCOV) --directory . --capture --output-file $(top_builddir)/build/coverage.info
	$(GENHTML) --output-directory $(top_builddir)/build/coverage $(top_builddir)/build/coverage.info
	$(LCOV) --directory . --zerocounters
	@echo "file://$(abs_top_builddir)/build/coverage/index.html"

clear-coverage:
	$(LCOV) --directory . --zerocounters
endif
