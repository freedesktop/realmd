XSLTPROC_FLAGS = --path $(abs_builddir):$(abs_srcdir):$(abs_builddir)/doc

man8_MANS += \
	doc/manual/realm.8
man5_MANS += \
	doc/manual/realmd.conf.5

$(man5_MANS): doc/privatedir.xml

MAN_IN_FILES = \
	$(man8_MANS:.8=.xml) \
	$(man5_MANS:.5=.xml) \
	$(NULL)

doc/privatedir.xml: doc/privatedir.xml.in
	$(V_SED) $(MKDIR_P) $(dir $@) && $(SED_SUBST) $< > $@

MANUAL_DOCBOOK = doc/manual/realmd-docs.xml

MANUAL_INCLUDES = \
	doc/manual/realmd-guide-active-directory.xml \
	doc/manual/realmd-guide-ipa.xml \
	doc/manual/realmd-guide-kerberos.xml \
	$(NULL)

MANUAL_STATIC = \
	doc/manual/static/gtk-doc.css \
	doc/manual/static/style.css \
	$(NULL)

DBUS_INTERFACE = dbus/org.freedesktop.realmd.xml
DBUS_ESCAPED = realmd-org.freedesktop.realmd.xml

DBUS_DOC_GENERATED = \
	realmd-org.freedesktop.realmd.Kerberos.xml \
	realmd-org.freedesktop.realmd.KerberosMembership.xml \
	realmd-org.freedesktop.realmd.Provider.xml \
	realmd-org.freedesktop.realmd.Realm.xml \
	realmd-org.freedesktop.realmd.Service.xml

MANUAL_XSLT = \
	doc/manual/escape-xml-to-text.xsl \
	doc/manual/gdbus-fix-bugs.xsl \
	doc/manual/gtk-doc.xsl \
	doc/manual/version-greater-or-equal.xsl \
	doc/manual/devhelp2.xsl \
	$(NULL)

EXTRA_DIST += \
	doc/privatedir.xml.in \
	$(MANUAL_DOCBOOK) \
	$(MANUAL_INCLUDES) \
	$(MAN_IN_FILES) \
	$(MANUAL_XSLT) \
	doc/manual/static \
	$(NULL)

CLEANFILES += \
	realmd-org.freedesktop.realmd.generated \
	doc/privatedir.xml \
	$(DBUS_DOC_GENERATED) \
	$(DBUS_ESCAPED) \
	$(man8_MANS) \
	$(man5_MANS) \
	manual/index.html \
	*.tmp \
	$(NULL)

.xml.8:
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) -o $@ http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<
.xml.5:
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) -o $@ http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

realmd-org.freedesktop.realmd.generated: $(DBUS_INTERFACE) doc/manual/gdbus-fix-bugs.xsl
	$(AM_V_GEN) gdbus-codegen --interface-prefix org.freedesktop.realmd. \
		--generate-docbook realmd $< && \
	(for f in $(DBUS_DOC_GENERATED); do \
		$(XSLTPROC) --output $$f.tmp $(srcdir)/doc/manual/gdbus-fix-bugs.xsl $$f && mv $$f.tmp $$f; \
	 done) && \
	touch realmd-org.freedesktop.realmd.generated

$(DBUS_DOC_GENERATED): realmd-org.freedesktop.realmd.generated

$(DBUS_ESCAPED): $(DBUS_INTERFACE) doc/manual/escape-xml-to-text.xsl
	$(AM_V_GEN) $(XSLTPROC) --nonet --novalid --output $(DBUS_ESCAPED) \
		$(srcdir)/doc/manual/escape-xml-to-text.xsl $< && \
	sed -i '/^[	 ]*$$/d' $(DBUS_ESCAPED)

manual/index.html: $(DBUS_DOC_GENERATED) $(MANUAL_DOCBOOK) $(MANUAL_INCLUDES) $(MAN_IN_FILES) $(DBUS_ESCAPED) $(MANUAL_STATIC) doc/privatedir.xml
	$(AM_V_GEN) mkdir -p manual && cp $(srcdir)/doc/manual/static/* manual/ && \
	$(XMLTO) html -m $(srcdir)/doc/manual/gtk-doc.xsl -o manual \
		--searchpath $(abs_builddir):$(abs_srcdir):$(abs_builddir)/doc \
		$(srcdir)/$(MANUAL_DOCBOOK)
