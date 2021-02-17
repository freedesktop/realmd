sbin_PROGRAMS += \
	realm

realm_SOURCES = \
	tools/realm.c \
	tools/realm.h \
	tools/realm-client.h \
	tools/realm-client.c \
	tools/realm-discover.c \
	tools/realm-join.c \
	tools/realm-leave.c \
	tools/realm-logins.c \
	$(NULL)

realm_CFLAGS = \
	-I$(top_srcdir)/dbus \
	-I$(top_srcdir)/build \
	-DLOCALEDIR=\""$(datadir)/locale"\" \
	-DREALMD_EXECUTABLE=\""$(libexecdir)/realmd"\" \
	$(GLIB_CFLAGS) \
	$(KRB5_CFLAGS) \
	$(NULL)

realm_LDADD = \
	librealm-dbus.a \
	$(GLIB_LIBS) \
	$(KRB5_LIBS) \
	$(NULL)
