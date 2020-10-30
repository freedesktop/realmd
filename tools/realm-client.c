/* realmd -- Realm configuration service
 *
 * Copyright 2012 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#include "realm.h"
#include "realm-client.h"
#include "realm-dbus-constants.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>

#include <krb5/krb5.h>

#include <sys/socket.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

struct _RealmClient {
	GDBusObjectManagerClient parent;
	RealmDbusProvider *provider;
	GPid peer_pid;
};

struct _RealmClientClass {
	GDBusObjectManagerClientClass parent;
};

G_DEFINE_TYPE (RealmClient, realm_client, G_TYPE_DBUS_OBJECT_MANAGER_CLIENT);

typedef struct {
	GAsyncResult *result;
	GMainLoop *loop;
} SyncClosure;

static void
on_complete_get_result (GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
	SyncClosure *sync = user_data;
	sync->result = g_object_ref (result);
	g_main_loop_quit (sync->loop);
}

static void
realm_client_init (RealmClient *self)
{

}

static void
realm_client_finalize (GObject *obj)
{
	RealmClient *self = REALM_CLIENT (obj);

	if (self->peer_pid) {
		kill (self->peer_pid, SIGTERM);
		g_spawn_close_pid (self->peer_pid);
	}

	if (self->provider)
		g_object_unref (self->provider);

	G_OBJECT_CLASS (realm_client_parent_class)->finalize (obj);
}

static void
realm_client_class_init (RealmClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = realm_client_finalize;
}

static GType
realm_object_client_get_proxy_type (GDBusObjectManagerClient *manager,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    gpointer user_data)
{
	static gsize once_init_value = 0;
	static GHashTable *lookup_hash;
	GType ret;

	if (interface_name == NULL)
		return G_TYPE_DBUS_OBJECT_PROXY;

	if (g_once_init_enter (&once_init_value)) {
		lookup_hash = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_insert (lookup_hash, (gpointer) "org.freedesktop.realmd.Provider", GSIZE_TO_POINTER (REALM_DBUS_TYPE_PROVIDER_PROXY));
		g_hash_table_insert (lookup_hash, (gpointer) "org.freedesktop.realmd.Service", GSIZE_TO_POINTER (REALM_DBUS_TYPE_SERVICE_PROXY));
		g_hash_table_insert (lookup_hash, (gpointer) "org.freedesktop.realmd.Realm", GSIZE_TO_POINTER (REALM_DBUS_TYPE_REALM_PROXY));
		g_hash_table_insert (lookup_hash, (gpointer) "org.freedesktop.realmd.Kerberos", GSIZE_TO_POINTER (REALM_DBUS_TYPE_KERBEROS_PROXY));
		g_hash_table_insert (lookup_hash, (gpointer) "org.freedesktop.realmd.KerberosMembership", GSIZE_TO_POINTER (REALM_DBUS_TYPE_KERBEROS_MEMBERSHIP_PROXY));
		g_once_init_leave (&once_init_value, 1);
	}

	ret = GPOINTER_TO_SIZE (g_hash_table_lookup (lookup_hash, interface_name));
	if (ret == 0)
		ret = G_TYPE_DBUS_OBJECT_PROXY;
	return ret;
}

static void
on_diagnostics_signal (GDBusConnection *connection,
                       const gchar *sender_name,
                       const gchar *object_path,
                       const gchar *interface_name,
                       const gchar *signal_name,
                       GVariant *parameters,
                       gpointer user_data)
{
	gboolean verbose = GPOINTER_TO_INT (user_data);
	const gchar *operation_id;
	const gchar *data;

	g_variant_get (parameters, "(&s&s)", &data, &operation_id);

	/*
	 * Various people have been worried by installing packages
	 * quietly, so notify about what's going on.
	 *
	 * In reality *configuring* and *starting* a daemon is far
	 * more worrisome than the installation. It's realmd's job
	 * to configure, enable and start stuff. So if you're properly
	 * worried, remove realmd and do stuff manually.
	 */
	if (verbose || strstr (data, _("Installing necessary packages")))
		g_printerr ("%s", data);
}

static gboolean
on_ctrl_c_cancel_operation (gpointer data)
{
	RealmDbusService *service = REALM_DBUS_SERVICE (data);

	if (!realm_cancelled && realm_operation_id) {
		realm_dbus_service_call_cancel (service, realm_operation_id,
		                                NULL, NULL, NULL);
		g_printerr ("Cancelling...\n");
		realm_cancelled = TRUE;
		_exit (1);
	}

	return TRUE;
}

static RealmClient *
realm_client_new_on_connection (GDBusConnection *connection,
                                gboolean verbose,
                                const gchar *bus_name)
{
	RealmDbusProvider *provider;
	RealmDbusService *service;
	GError *error = NULL;
	GInitable *ret;
	RealmClient *client = NULL;
	GDBusSignalFlags flags;

	flags = G_DBUS_SIGNAL_FLAGS_NONE;
	if (bus_name == NULL)
		flags |= G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE;

	g_dbus_connection_signal_subscribe (connection, bus_name,
	                                    REALM_DBUS_SERVICE_INTERFACE,
	                                    REALM_DBUS_DIAGNOSTICS_SIGNAL,
	                                    REALM_DBUS_SERVICE_PATH,
	                                    NULL, flags,
	                                    on_diagnostics_signal,
	                                    GINT_TO_POINTER (verbose), NULL);

	provider = realm_dbus_provider_proxy_new_sync (connection,
	                                               G_DBUS_PROXY_FLAGS_NONE,
	                                               bus_name,
	                                               REALM_DBUS_SERVICE_PATH,
	                                               NULL, &error);
	if (error != NULL) {
		realm_handle_error (error, _("Couldn't connect to realm service"));
		return NULL;
	}

	service = realm_dbus_service_proxy_new_sync (connection,
	                                             G_DBUS_PROXY_FLAGS_NONE,
	                                             bus_name,
	                                             REALM_DBUS_SERVICE_PATH,
	                                             NULL, &error);
	if (error != NULL) {
		realm_handle_error (error, _("Couldn't connect to realm service"));
		g_object_unref (provider);
		return NULL;
	}

	ret = g_initable_new (REALM_TYPE_CLIENT, NULL, &error,
	                      "flags", G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
	                      "name", bus_name,
	                      "connection", connection,
	                      "object-path", REALM_DBUS_SERVICE_PATH,
	                      "get-proxy-type-func", realm_object_client_get_proxy_type,
	                      NULL);

	if (ret != NULL) {
		client = REALM_CLIENT (ret);
		client->provider = g_object_ref (provider);
		g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (provider), G_MAXINT);

		/* On Ctrl-C send a cancel to the server */
		g_unix_signal_add_full (G_PRIORITY_HIGH, SIGINT,
		                        on_ctrl_c_cancel_operation,
		                        g_object_ref (service),
		                        g_object_unref);
	}

	g_object_unref (service);
	g_object_unref (provider);

	if (error != NULL) {
		realm_handle_error (error, _("Couldn't load the realm service"));
		return NULL;
	}


	return client;
}

static RealmClient *
realm_client_new_system (gboolean verbose)
{
	GDBusConnection *connection;
	GError *error = NULL;
	RealmClient *client = NULL;

	connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
	if (error != NULL) {
		if (verbose)
			g_printerr (" ! To run without a DBus bus use the install mode: --install=/\n");
		realm_handle_error (error, _("Couldn't connect to system bus"));
		return NULL;
	}

	client = realm_client_new_on_connection (connection, verbose, REALM_DBUS_BUS_NAME);
	g_object_unref (connection);
	return client;
}

static RealmClient *
realm_client_new_installer (gboolean verbose,
                            const gchar *prefix)
{
	GDBusConnection *connection;
	GSocketConnection *stream;
	RealmClient *client;
	GSocket *socket;
	GError *error = NULL;
	gchar buffer[16];
	GPid pid = 0;
	int pair[2];

	const gchar *args[] = {
		REALMD_EXECUTABLE,
		"--install", prefix,
		"--dbus-peer", buffer,
		NULL
	};

	if (socketpair (AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
		realm_handle_error (NULL, _("Couldn't create socket pair: %s"), g_strerror (errno));
		return NULL;
	}

	g_snprintf (buffer, sizeof (buffer), "%d", pair[1]);

	socket = g_socket_new_from_fd (pair[0], &error);
	if (error != NULL) {
		realm_handle_error (error, _("Couldn't create socket"));
		close(pair[0]);
		close(pair[1]);
		return NULL;
	}

	g_spawn_async (prefix ? prefix : "/", (gchar **)args, NULL,
	               G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD,
	               NULL, NULL, &pid, &error);

	close(pair[1]);

	if (error != NULL) {
		realm_handle_error (error, _("Couldn't run realmd"));
		close(pair[0]);
		return NULL;
	}

	stream = g_socket_connection_factory_create_connection (socket);
	g_return_val_if_fail (stream != NULL, NULL);
	g_object_unref (socket);

	connection = g_dbus_connection_new_sync (G_IO_STREAM (stream), NULL,
	                                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
	                                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
	                                         NULL, NULL, &error);
	g_object_unref (stream);

	if (error == NULL) {
		client = realm_client_new_on_connection (connection, verbose, NULL);
		g_object_unref (connection);
	} else {
		realm_handle_error (error, _("Couldn't create socket"));
		client = NULL;
	}

	/* Make sure the process is owned */
	if (client) {
		client->peer_pid = pid;
	} else {
		kill (pid, SIGTERM);
		g_spawn_close_pid (pid);
	}

	return client;
}

RealmClient *
realm_client_new (gboolean verbose,
                  const gchar *prefix)
{
	if (prefix)
		return realm_client_new_installer (verbose, prefix);
	else
		return realm_client_new_system (verbose);
}

RealmDbusProvider *
realm_client_get_provider (RealmClient *self)
{
	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);
	return self->provider;
}

GList *
realm_client_discover (RealmClient *self,
                       const gchar *string,
                       gboolean use_ldaps,
                       const gchar *client_software,
                       const gchar *server_software,
                       const gchar *membership_software,
                       const gchar *dbus_interface,
                       gboolean *had_mismatched,
                       GError **error)
{
	GDBusObjectManager *manager;
	GDBusInterface *iface;
	GVariant *options;
	SyncClosure sync;
	gchar **realm_paths;
	gint relevance;
	GList *realms;
	gboolean ret;
	gint i;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	if (string == NULL)
		string = "";

	sync.result = NULL;
	sync.loop = g_main_loop_new (NULL, FALSE);

	options = realm_build_options (REALM_DBUS_OPTION_CLIENT_SOFTWARE, client_software,
	                               REALM_DBUS_OPTION_SERVER_SOFTWARE, server_software,
	                               REALM_DBUS_OPTION_MEMBERSHIP_SOFTWARE, membership_software,
	                               REALM_DBUS_OPTION_USE_LDAPS, use_ldaps ? "True" : "False",
	                               NULL);

	/* Start actual operation */
	realm_dbus_provider_call_discover (self->provider, string, options,
	                                   NULL, on_complete_get_result, &sync);

	/* This mainloop is quit by on_complete_get_result */
	g_main_loop_run (sync.loop);

	ret = realm_dbus_provider_call_discover_finish (self->provider, &relevance,
	                                                &realm_paths, sync.result, error);

	g_object_unref (sync.result);
	g_main_loop_unref (sync.loop);

	if (!ret)
		return FALSE;

	realms = NULL;
	manager = G_DBUS_OBJECT_MANAGER (self);

	for (i = 0; realm_paths[i] != NULL; i++) {
		iface = g_dbus_object_manager_get_interface (manager, realm_paths[i],
		                                             dbus_interface);
		if (iface == NULL) {
			if (had_mismatched)
				*had_mismatched = TRUE;
		} else {
			g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (iface), G_MAXINT);
			realms = g_list_prepend (realms, iface);
		}
	}

	g_strfreev (realm_paths);
	return g_list_reverse (realms);
}

RealmDbusRealm *
realm_client_get_realm (RealmClient *self,
                        const gchar *object_path)
{
	GDBusInterface *iface;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	iface = g_dbus_object_manager_get_interface (G_DBUS_OBJECT_MANAGER (self),
	                                             object_path, REALM_DBUS_REALM_INTERFACE);
	if (iface)
		g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (iface), G_MAXINT);
	return REALM_DBUS_REALM (iface);
}

RealmDbusRealm *
realm_client_to_realm (RealmClient *self,
                       gpointer proxy)
{
	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);
	return realm_client_get_realm (self, g_dbus_proxy_get_object_path (proxy));
}

RealmDbusKerberosMembership *
realm_client_get_kerberos_membership (RealmClient *self,
                                      const gchar *object_path)
{
	GDBusInterface *iface;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	iface = g_dbus_object_manager_get_interface (G_DBUS_OBJECT_MANAGER (self),
	                                             object_path, REALM_DBUS_KERBEROS_MEMBERSHIP_INTERFACE);
	if (iface)
		g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (iface), G_MAXINT);
	return REALM_DBUS_KERBEROS_MEMBERSHIP (iface);
}

RealmDbusKerberosMembership *
realm_client_to_kerberos_membership (RealmClient *self,
                                     gpointer proxy)
{
	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);
	return realm_client_get_kerberos_membership (self, g_dbus_proxy_get_object_path (proxy));
}

RealmDbusKerberos *
realm_client_get_kerberos (RealmClient *self,
                           const gchar *object_path)
{
	GDBusInterface *iface;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	iface = g_dbus_object_manager_get_interface (G_DBUS_OBJECT_MANAGER (self),
	                                             object_path, REALM_DBUS_KERBEROS_INTERFACE);
	if (iface)
		g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (iface), G_MAXINT);
	return REALM_DBUS_KERBEROS (iface);
}

RealmDbusKerberos *
realm_client_to_kerberos (RealmClient *self,
                          gpointer proxy)
{
	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);
	return realm_client_get_kerberos (self, g_dbus_proxy_get_object_path (proxy));
}

static const gchar *
are_credentials_supported (GVariant *supported,
                           const gchar *credential_type_1,
                           const gchar *credential_type_2,
                           const gchar **ret_owner)
{
	GVariantIter iter;
	const gchar *type;
	const gchar *owner;

	g_variant_iter_init (&iter, supported);
	while (g_variant_iter_loop (&iter, "(&s&s)", &type, &owner)) {
		if (g_strcmp0 (credential_type_1, type) == 0 ||
		    g_strcmp0 (credential_type_2, type) == 0) {
			*ret_owner = owner;
			return type;
		}
	}

	return NULL;
}

static void
propagate_krb5_error (GError **dest,
                      krb5_context context,
                      krb5_error_code code,
                      const gchar *format,
                      ...) G_GNUC_PRINTF (4, 5);

static void
propagate_krb5_error (GError **dest,
                      krb5_context context,
                      krb5_error_code code,
                      const gchar *format,
                      ...)
{
	GString *message;
	va_list va;

	message = g_string_new ("");

	if (format) {
		va_start (va, format);
		g_string_append_vprintf (message, format, va);
		va_end (va);
	}

	if (code != 0) {
		if (format)
			g_string_append (message, ": ");
		g_string_append (message, krb5_get_error_message (context, code));
	}

	g_set_error_literal (dest, g_quark_from_static_string ("krb5"),
	                     code, message->str);
	g_string_free (message, TRUE);
}

static krb5_ccache
prepare_temporary_ccache (krb5_context krb5,
                          gchar **filename,
                          GError **error)
{
	const gchar *directory;
	krb5_error_code code;
	krb5_ccache ccache;
	gchar *temp_name;
	gint temp_fd;
	int errn;

	directory = g_get_user_runtime_dir ();
	if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
		directory = g_get_tmp_dir ();

	if (g_mkdir_with_parents (directory, S_IRWXU) < 0) {
		errn = errno;
		g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errn),
		             _("Couldn't create runtime directory: %s: %s"),
		             directory, g_strerror (errn));
		return NULL;
	}

	temp_name = g_build_filename (directory, "realmd-krb5-cache.XXXXXX", NULL);
	temp_fd = g_mkstemp_full (temp_name, O_RDWR, S_IRUSR | S_IWUSR);
	if (temp_fd == -1) {
		errn = errno;
		g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errn),
		             _("Couldn't create credential cache file: %s: %s"),
		             temp_name, g_strerror (errn));
		g_free (temp_name);
		return NULL;
	}

	close (temp_fd);

	code = krb5_cc_resolve (krb5, temp_name, &ccache);
	if (code != 0) {
		propagate_krb5_error (error, krb5, code, _("Couldn't resolve credential cache"));
		g_free (temp_name);
		return NULL;
	}

	g_debug ("Temporary credential cache: %s", temp_name);
	*filename = temp_name;
	return ccache;
}

static gboolean
copy_to_ccache (krb5_context krb5,
                const gchar *realm_name,
                krb5_principal principal,
                krb5_ccache ccache)
{
	krb5_principal server;
	krb5_ccache def_ccache;
	krb5_error_code code;
	krb5_creds mcred;
	krb5_creds creds;

	code = krb5_cc_default (krb5, &def_ccache);
	if (code != 0) {
		g_debug ("krb5_cc_default failed: %s", krb5_get_error_message (krb5, code));
		return FALSE;
	}

	code = krb5_build_principal (krb5, &server,
	                             strlen (realm_name), realm_name,
	                             KRB5_TGS_NAME, realm_name, NULL);
	g_return_val_if_fail (code == 0, FALSE);

	memset (&mcred, 0, sizeof (mcred));
	mcred.client = principal;
	mcred.server = server;
	mcred.times.starttime = g_get_real_time () / G_TIME_SPAN_MILLISECOND;
	mcred.times.endtime = mcred.times.starttime;

	code = krb5_cc_retrieve_cred (krb5, def_ccache, KRB5_TC_MATCH_TIMES,
	                              &mcred, &creds);

	krb5_free_principal (krb5, server);
	krb5_cc_close (krb5, def_ccache);

	if (code == KRB5_CC_NOTFOUND) {
		g_debug ("no matching principal found in %s", krb5_cc_default_name (krb5));
		return FALSE;
	} else if (code != 0) {
		g_debug ("krb5_cc_retrieve_cred failed: %s", krb5_get_error_message (krb5, code));
		return FALSE;
	}

	code = krb5_cc_store_cred (krb5, ccache, &creds);
	krb5_free_cred_contents (krb5, &creds);

	if (code != 0) {
		g_debug ("krb5_cc_store_cred failed: %s", krb5_get_error_message (krb5, code));
		return FALSE;
	}

	g_debug ("retrieved credentials from: %s", krb5_cc_default_name (krb5));
	return TRUE;
}

static gboolean
kinit_to_ccache (krb5_context krb5,
                 krb5_principal principal,
                 const gchar *name,
                 krb5_ccache ccache,
                 GError **error)
{
	krb5_get_init_creds_opt *options = NULL;
	krb5_error_code code;
	krb5_creds my_creds;

	code = krb5_get_init_creds_opt_alloc (krb5, &options);
	g_return_val_if_fail (code == 0, FALSE);

	code = krb5_get_init_creds_opt_set_out_ccache (krb5, options, ccache);
	g_return_val_if_fail (code == 0, FALSE);

	code = krb5_get_init_creds_password (krb5, &my_creds, principal, NULL,
	                                     krb5_prompter_posix, 0, 0, NULL, options);

	krb5_get_init_creds_opt_free (krb5, options);

	if (code == KRB5KDC_ERR_PREAUTH_FAILED ||
	    code == KRB5KRB_AP_ERR_BAD_INTEGRITY ||
	    code == KRB5_PREAUTH_FAILED) {
		propagate_krb5_error (error, krb5, code, _("Invalid password for %s"), name);
		return FALSE;

	} else if (code != 0) {
		propagate_krb5_error (error, krb5, code, _("Couldn't authenticate as %s"), name);
		return FALSE;
	}

	krb5_free_cred_contents (krb5, &my_creds);
	return TRUE;
}

static gboolean
copy_or_kinit_to_ccache (krb5_context krb5,
                         krb5_ccache ccache,
                         const gchar *user_name,
                         const gchar *realm_name,
                         GError **error)
{
	krb5_principal principal;
	krb5_error_code code;
	gchar *full_name = NULL;
	gboolean ret;

	if (strchr (user_name, '@') == NULL)
		user_name = full_name = g_strdup_printf ("%s@%s", user_name, realm_name);

	code = krb5_parse_name (krb5, user_name, &principal);
	if (code != 0) {
		propagate_krb5_error (error, krb5, code, _("Couldn't parse user name: %s"), user_name);
		g_free (full_name);
		return FALSE;
	}

	ret = copy_to_ccache (krb5, realm_name, principal, ccache) ||
	      kinit_to_ccache (krb5, principal, user_name, ccache, error);

	g_free (full_name);
	krb5_free_principal (krb5, principal);

	return ret;
}

static GVariant *
read_file_into_variant (const gchar *filename)
{
	GVariant *variant;
	GError *error = NULL;
	gchar *contents;
	gsize length;

	g_file_get_contents (filename, &contents, &length, &error);
	if (error != NULL) {
		realm_handle_error (error, _("Couldn't read credential cache"));
		return NULL;
	}

	variant = g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
	                                   contents, length,
	                                   TRUE, g_free, contents);

	return g_variant_ref_sink (variant);
}

static GVariant *
build_ccache_credential (const gchar *user_name,
                         const gchar *realm_name,
                         const gchar *credential_owner,
                         GError **error)
{
	krb5_error_code code;
	krb5_context krb5;
	gboolean ret = FALSE;
	krb5_ccache ccache;
	gchar *filename;
	GVariant *result;

	code = krb5_init_context (&krb5);
	if (code != 0) {
		propagate_krb5_error (error, NULL, code, _("Couldn't initialize kerberos"));
		return NULL;
	}

	ccache = prepare_temporary_ccache (krb5, &filename, error);
	if (ccache) {
		ret = copy_or_kinit_to_ccache (krb5, ccache, user_name, realm_name, error);
		krb5_cc_close (krb5, ccache);
		krb5_free_context (krb5);
	}

	if (!ret)
		return NULL;

	result = read_file_into_variant (filename);

	g_unlink (filename);
	g_free (filename);

	if (result)
		result = g_variant_new ("(ssv)", "ccache", credential_owner, result);

	return result;
}

static gchar *
prompt_stdin (const gchar *prompt)
{
	static const gsize pass_max = 8192;
	gchar *password;
	gsize len;

	g_printf ("%s", prompt);
	fflush (stdout);

	password = malloc (pass_max);
	if (!fgets (password, pass_max, stdin))
		password[0] = '\0';

	g_printf ("\n");

	len = strlen (password);
	if (len > 0 && password[len - 1] == '\n')
		password[len - 1] = '\0';

	return password;
}

static GVariant *
build_password_credential (const gchar *user_name,
                           const gchar *credential_owner,
                           GError **error)
{
	const gchar *password;
	GVariant *result;
	gchar *alloced;
	gchar *prompt;
	int istty;

	istty = isatty (0);

	if (istty && realm_unattended) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		             _("Cannot prompt for a password when running in unattended mode"));
		return NULL;
	}

	prompt = g_strdup_printf (_("Password for %s: "), user_name);

	/*
	 * Yeah, getpass is obselete. Have fun trying to recreate it even
	 * semi-portably.
	 */
	if (istty) {
		password = getpass (prompt);
		alloced = NULL;
	} else {
		alloced = prompt_stdin (prompt);
		password = alloced;
	}

	g_free (prompt);

	if (password == NULL) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             _("Couldn't prompt for password: %s"), g_strerror (errno));
		return NULL;
	}

	result = g_variant_new ("(ssv)", "password", credential_owner,
	                        g_variant_new ("(ss)", user_name, password));

	if (password)
		memset ((char *)password, 0, strlen (password));
	free (alloced);

	return result;
}

GVariant *
realm_client_build_principal_creds (RealmClient *self,
                                    RealmDbusKerberosMembership *membership,
                                    GVariant *supported,
                                    const gchar *user_name,
                                    GError **error)
{
	RealmDbusKerberos *kerberos;
	const gchar *realm_name;
	GVariant *creds;
	const gchar *credential_type;
	const gchar *credential_owner;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	credential_type = are_credentials_supported (supported,
	                                             "ccache", "password",
	                                             &credential_owner);

	if (credential_type == NULL) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		             _("Realm does not support membership using a password"));
		return NULL;
	}

	g_debug ("Using credential type: %s/%s", credential_type, credential_owner);

	if (user_name == NULL)
		user_name = realm_dbus_kerberos_membership_get_suggested_administrator (membership);
	if (user_name == NULL || g_str_equal (user_name, ""))
		user_name = g_get_user_name ();

	g_debug ("Using user: %s", user_name);

	/* A credential cache? */
	if (g_str_equal (credential_type, "ccache")) {
		kerberos = realm_client_to_kerberos (self, membership);
		realm_name = realm_dbus_kerberos_get_realm_name (kerberos);
		creds = build_ccache_credential (user_name, realm_name, credential_owner, error);

	/* A plain ol password */
	} else {
		creds = build_password_credential (user_name, credential_owner, error);
	}

	return creds;
}

GVariant *
realm_client_build_otp_creds (RealmClient *self,
                              GVariant *supported,
                              const gchar *one_time_password,
                              GError **error)
{
	const gchar *owner;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	if (!are_credentials_supported (supported, "secret", NULL, &owner)) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		             _("Realm does not support membership using a one time password"));
		return NULL;
	}

	return g_variant_new ("(ssv)", "secret", owner,
	                      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
	                                                 one_time_password,
	                                                 strlen (one_time_password),
	                                                 sizeof (unsigned char)));
}

static krb5_error_code
copy_credential_cache (krb5_context krb5,
                       krb5_ccache src,
                       krb5_ccache dst)
{
	krb5_error_code code = 0;
	krb5_principal princ = NULL;

	code = krb5_cc_get_principal (krb5, src, &princ);
	if (!code)
		code = krb5_cc_initialize (krb5, dst, princ);
	if (code)
		return code;

	code = krb5_cc_copy_creds (krb5, src, dst);
	if (princ)
		krb5_free_principal (krb5, princ);

	return code;
}

static GVariant *
lookup_ccache_credential (const gchar *realm_name,
                          const gchar *credential_owner,
                          GError **error)
{
	GVariant *result = NULL;
	krb5_error_code code;
	krb5_context krb5;
	krb5_ccache origin = NULL;
	krb5_ccache ccache = NULL;
	krb5_principal principal;
	krb5_principal server;
	gchar *filename;

	code = krb5_init_context (&krb5);
	if (code != 0) {
		propagate_krb5_error (error, NULL, code, _("Couldn't initialize kerberos"));
		return NULL;
	}

	code = krb5_build_principal (krb5, &server,
	                             strlen (realm_name), realm_name,
	                             KRB5_TGS_NAME, realm_name, NULL);
	g_return_val_if_fail (code == 0, FALSE);

	code = krb5_cc_select (krb5, server, &origin, &principal);

	krb5_free_principal (krb5, server);
	if (principal)
		krb5_free_principal (krb5, principal);

	if (code == KRB5_CC_NOTFOUND) {
		g_debug ("No ccache credentials found for: %s", realm_name);
		origin = NULL;

	} else if (code != 0) {
		propagate_krb5_error (error, krb5, code, _("Couldn't select kerberos credentials"));
		origin = NULL;
	}

	if (origin) {
		ccache = prepare_temporary_ccache (krb5, &filename, error);
		if (ccache) {
			g_debug ("Copying credential cache");
			code = copy_credential_cache (krb5, origin, ccache);
			krb5_cc_close (krb5, ccache);

			if (code == 0)
				result = read_file_into_variant (filename);
			else
				propagate_krb5_error (error, krb5, code, _("Couldn't read kerberos credentials"));
			if (result)
				result = g_variant_new ("(ssv)", "ccache", credential_owner, result);

			g_unlink (filename);
			g_free (filename);
		}

		krb5_cc_close (krb5, origin);
	}

	krb5_free_context (krb5);

	return result;
}


GVariant *
realm_client_build_automatic_creds (RealmClient *self,
                                    RealmDbusKerberos *realm,
                                    GVariant *supported,
                                    GError **error)
{
	const gchar *credential_owner;
	const gchar *realm_name;
	GVariant *result;
	GError *erra = NULL;

	g_return_val_if_fail (REALM_IS_CLIENT (self), NULL);

	/* So first check if we have a kerberos ccache that matches */
	if (are_credentials_supported (supported, "ccache", NULL, &credential_owner)) {
		realm_name = realm_dbus_kerberos_get_realm_name (realm);
		result = lookup_ccache_credential (realm_name, credential_owner, &erra);

		/* If no credentials, then fall through to below */
		if (erra != NULL) {
			g_propagate_error (error, erra);
			return NULL;
		} else if (result != NULL) {
			return result;
		}
	}

	if (are_credentials_supported (supported, "automatic", NULL, &credential_owner)) {
		return g_variant_new ("(ssv)", "automatic", credential_owner,
		                      g_variant_new_string (""));
	}

	g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	             _("Realm does not support automatic membership"));
	return NULL;
}
