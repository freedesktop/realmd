libexec_PROGRAMS += \
	realmd

REALMD_CONFIGS = \
	service/realmd-defaults.conf \
	service/realmd-redhat.conf \
	service/realmd-debian.conf \
	service/realmd-suse.conf \
	$(NULL)

private_DATA += \
	service/realmd-defaults.conf

polkit_policydir = $(POLKIT_ACTION_DIR)
polkit_policy_in_files = service/org.freedesktop.realmd.policy.in
polkit_policy_DATA = $(polkit_policy_in_files:.policy.in=.policy)
service/%.policy: service/%.policy.in
	$(INTLTOOL_V_MERGE) mkdir -p $(builddir)/service && \
		LC_ALL=C $(INTLTOOL_MERGE) $(INTLTOOL_V_MERGE_OPTIONS) -x -u \
			-c $(builddir)/po/.intltool-merge-cache \
			$(srcdir)/po $< $@

realmd_SOURCES = \
	service/realm-adcli-enroll.c \
	service/realm-adcli-enroll.h \
	service/realm-all-provider.c \
	service/realm-all-provider.h \
	service/realm-command.c \
	service/realm-command.h \
	service/realm-credential.c \
	service/realm-credential.h \
	service/realm-daemon.c \
	service/realm-daemon.h \
	service/realm-diagnostics.c \
	service/realm-diagnostics.h \
	service/realm-disco.c \
	service/realm-disco.h \
	service/realm-disco-dns.c \
	service/realm-disco-dns.h \
	service/realm-disco-domain.c \
	service/realm-disco-domain.h \
	service/realm-disco-mscldap.c \
	service/realm-disco-mscldap.h \
	service/realm-disco-rootdse.c \
	service/realm-disco-rootdse.h \
	service/realm-dn-util.c \
	service/realm-dn-util.h \
	service/realm-errors.c \
	service/realm-errors.h \
	service/realm-example.c \
	service/realm-example.h \
	service/realm-example-provider.c \
	service/realm-example-provider.h \
	service/realm-ini-config.c \
	service/realm-ini-config.h \
	service/realm-invocation.c \
	service/realm-invocation.h \
	service/realm-kerberos.c \
	service/realm-kerberos.h \
	service/realm-kerberos-config.c \
	service/realm-kerberos-config.h \
	service/realm-kerberos-membership.c \
	service/realm-kerberos-membership.h \
	service/realm-kerberos-provider.c \
	service/realm-kerberos-provider.h \
	service/realm-ldap.c \
	service/realm-ldap.h \
	service/realm-login-name.c \
	service/realm-login-name.h \
	service/realm-network.c \
	service/realm-network.h \
	service/realm-options.c \
	service/realm-options.h \
	service/realm-packages.c \
	service/realm-packages.h \
	service/realm-provider.c \
	service/realm-provider.h \
	service/realm-samba.c \
	service/realm-samba.h \
	service/realm-samba-config.c \
	service/realm-samba-config.h \
	service/realm-samba-enroll.c \
	service/realm-samba-enroll.h \
	service/realm-samba-provider.c \
	service/realm-samba-provider.h \
	service/realm-samba-winbind.c \
	service/realm-samba-winbind.h \
	service/realm-service.c \
	service/realm-service.h \
	service/realm-settings.c \
	service/realm-settings.h \
	service/realm-sssd.c \
	service/realm-sssd.h \
	service/realm-sssd-ad.c \
	service/realm-sssd-ad.h \
	service/realm-sssd-provider.c \
	service/realm-sssd-provider.h \
	service/realm-sssd-config.c \
	service/realm-sssd-config.h \
	service/realm-sssd-ipa.c \
	service/realm-sssd-ipa.h \
	service/realm-usleep-async.c \
	service/realm-usleep-async.h \
	service/safe-format-string.c \
	service/safe-format-string.h \
	$(NULL)

realmd_CFLAGS = \
	-I$(top_srcdir)/dbus \
	-I$(top_srcdir)/build \
	-DPRIVATE_DIR="\"$(privatedir)\"" \
	-DSYSCONF_DIR="\"$(sysconfdir)\"" \
	-DPROVIDER_DIR="\"$(privatedir)/provider.d\"" \
	-DLOCALEDIR=\""$(datadir)/locale"\" \
	-DSTATE_DIR="\"$(localstatedir)/lib/realmd\"" \
	-DCACHEDIR="\"$(cachedir)\"" \
	$(PACKAGEKIT_CFLAGS) \
	$(POLKIT_CFLAGS) \
	$(SYSTEMD_JOURNAL_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(KRB5_CFLAGS) \
	$(LDAP_CFLAGS) \
	$(NULL)

realmd_LDADD = \
	librealm-dbus.a \
	$(PACKAGEKIT_LIBS) \
	$(POLKIT_LIBS) \
	$(SYSTEMD_JOURNAL_LIBS) \
	$(GLIB_LIBS) \
	$(KRB5_LIBS) \
	$(LDAP_LIBS) \
	$(NULL)

# Install and uninstall the config for this distro
install-service:
	$(INSTALL_PROGRAM) -d $(DESTDIR)$(privatedir)
	$(INSTALL_PROGRAM) -d $(DESTDIR)$(localstatedir)/lib/realmd
	$(INSTALL_PROGRAM) -d $(DESTDIR)$(cachedir)
	$(INSTALL_DATA) $(srcdir)/service/realmd-$(DISTRO).conf $(DESTDIR)$(privatedir)/realmd-distro.conf
uninstall-service:
	rm -f $(DESTDIR)$(privatedir)/realmd-distro.conf

# ------------------------------------------------------------------

EXTRA_DIST += \
	$(polkit_policy_in_files) \
	$(REALMD_CONFIGS) \
	$(private_DATA) \
	$(provider_DATA) \
	$(NULL)

CLEANFILES += \
	$(polkit_policy_DATA) \
	$(NULL)
