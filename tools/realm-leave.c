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
#include "realm-dbus-constants.h"
#include "realm-dbus-generated.h"

#include <glib.h>
#include <glib/gi18n.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

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

static int
call_leave (RealmDbusKerberosMembership *membership,
            GVariant *credentials,
            GVariant *options,
            GError **error)
{
	SyncClosure sync;
	gboolean ret;

	sync.result = NULL;
	sync.loop = g_main_loop_new (NULL, FALSE);

	/* Start actual operation */
	realm_dbus_kerberos_membership_call_leave (membership, credentials, options,
	                                           NULL, on_complete_get_result, &sync);

	/* This mainloop is quit by on_complete_get_result */
	g_main_loop_run (sync.loop);

	ret = realm_dbus_kerberos_membership_call_leave_finish (membership, sync.result, error);

	g_object_unref (sync.result);
	g_main_loop_unref (sync.loop);

	return ret ? 0 : 1;
}

static int
call_deconfigure (RealmDbusRealm *realm,
                  GVariant *options,
                  GError **error)
{
	SyncClosure sync;
	gboolean ret;

	sync.result = NULL;
	sync.loop = g_main_loop_new (NULL, FALSE);

	/* Start actual operation */
	realm_dbus_realm_call_deconfigure (realm, options,
	                                   NULL, on_complete_get_result, &sync);

	/* This mainloop is quit by on_complete_get_result */
	g_main_loop_run (sync.loop);

	ret = realm_dbus_realm_call_deconfigure_finish (realm, sync.result, error);

	g_object_unref (sync.result);
	g_main_loop_unref (sync.loop);

	return ret ? 0 : 1;
}

static gboolean
match_kerberos_realm_to_detail (RealmDbusRealm *realm,
                                const gchar *field,
                                const gchar *value)
{
	GVariantIter iter;
	const gchar *vfield;
	const gchar *vvalue;
	gboolean matching = FALSE;

	/* If not set then anything matches */
	if (value == NULL)
		return TRUE;

	g_variant_iter_init (&iter, realm_dbus_realm_get_details (realm));
	while (g_variant_iter_loop (&iter, "(&s&s)", &vfield, &vvalue)) {
		if (g_str_equal (field, vfield) && g_str_equal (value, vvalue)) {
			matching = TRUE;
			break;
		}
	}

	return matching;
}

static RealmDbusRealm *
locate_configured_matching_kerberos_realm (RealmClient *client,
                                           const gchar *realm_name,
                                           const gchar *client_software,
                                           const gchar *server_software,
                                           RealmDbusKerberosMembership **membership)
{
	RealmDbusProvider *provider;
	const gchar *const *paths;
	RealmDbusRealm *realm;
	const gchar *name;
	gboolean matched;
	gint i;

	*membership = NULL;
	provider = realm_client_get_provider (client);
	paths = realm_dbus_provider_get_realms (provider);

	for (i = 0; paths && paths[i]; i++) {
		matched = FALSE;

		realm = realm_client_get_realm (client, paths[i]);
		*membership = realm_client_to_kerberos_membership (client, realm);

		if (*membership != NULL && realm_is_configured (realm)) {
			if (realm_name == NULL) {
				matched = TRUE;
			} else {
				name = realm_dbus_realm_get_name (realm);
				matched = (g_ascii_strcasecmp (name, realm_name) == 0);
			}
		}

		if (matched)
			matched = match_kerberos_realm_to_detail (realm, "client-software", client_software);
		if (matched)
			matched = match_kerberos_realm_to_detail (realm, "server-software", server_software);

		if (matched)
			return realm;

		g_clear_object (membership);
		g_object_unref (realm);
	}

	return NULL;
}

static int
perform_deconfigure (RealmClient *client,
                     RealmDbusRealm *realm)
{
	GError *error = NULL;
	GVariant *options;
	gint ret;

	options = realm_build_options(NULL, NULL);
	ret = call_deconfigure (realm, options, &error);

	if (error != NULL)
		realm_handle_error (error, _("Couldn't leave realm"));

	return ret;
}

static int
perform_user_leave (RealmClient *client,
                    gboolean use_ldaps,
                    RealmDbusKerberosMembership *membership,
                    const gchar *user_name)
{
	GError *error = NULL;
	GVariant *supported;
	GVariant *credentials;
	GVariant *options;
	gint ret;

	supported = realm_dbus_kerberos_membership_get_supported_leave_credentials (membership);
	credentials = realm_client_build_principal_creds (client, membership, supported, user_name, &error);
	if (!credentials) {
		realm_handle_error (error, NULL);
		return 1;
	}

	options = realm_build_options (REALM_DBUS_OPTION_USE_LDAPS, use_ldaps ? "True" : "False",
	                               NULL);
	ret = call_leave (membership, credentials, options, &error);

	if (error != NULL)
		realm_handle_error (error, _("Couldn't leave realm"));

	return ret;
}

static int
perform_leave (RealmClient *client,
               const gchar *realm_name,
               gboolean use_ldaps,
               gboolean remove,
               const gchar *user_name,
               const gchar *client_software,
               const gchar *server_software)
{
	RealmDbusKerberosMembership *membership;
	RealmDbusRealm *realm;
	gint ret;

	realm = locate_configured_matching_kerberos_realm (client, realm_name, client_software,
	                                                   server_software, &membership);
	if (realm == NULL) {
		if (!realm_name && !client_software && !server_software)
			realm_handle_error (NULL, "Couldn't find a configured realm");
		else
			realm_handle_error (NULL, "Couldn't find a matching realm");
		return 1;
	}

	/* Specifying a user name implies remov */
	if (user_name && !remove)
		remove = TRUE;

	if (!remove)
		ret = perform_deconfigure (client, realm);
	else
		ret = perform_user_leave (client, use_ldaps, membership,
		                          user_name);

	g_object_unref (membership);
	g_object_unref (realm);

	return ret;
}

int
realm_leave (RealmClient *client,
             int argc,
             char *argv[])
{
	GOptionContext *context;
	gchar *arg_user = NULL;
	gboolean arg_remove = FALSE;
	gchar *arg_client_software = NULL;
	gchar *arg_server_software = NULL;
	GError *error = NULL;
	const gchar *realm_name;
	gboolean arg_use_ldaps = FALSE;
	gint ret = 0;

	GOptionEntry option_entries[] = {
		{ "client-software", 0, 0, G_OPTION_ARG_STRING, &arg_client_software,
		  N_("Use specific client software"), NULL },
		{ "remove", 'r', 0, G_OPTION_ARG_NONE, &arg_remove, N_("Remove computer from realm"), NULL, },
		{ "server-software", 0, 0, G_OPTION_ARG_STRING, &arg_server_software,
		  N_("Use specific server software"), NULL },
		{ "user", 'U', 0, G_OPTION_ARG_STRING, &arg_user, N_("User name to use for removal"), NULL },
		{ "use-ldaps", 0, 0, G_OPTION_ARG_NONE, &arg_use_ldaps, N_("Use ldaps to connect to LDAP"), NULL },
		{ NULL, }
	};

	context = g_option_context_new ("realm");
	g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
	g_option_context_add_main_entries (context, option_entries, NULL);
	g_option_context_add_main_entries (context, realm_global_options, NULL);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_printerr ("%s: %s\n", g_get_prgname (), error->message);
		g_error_free (error);
		ret = 2;

	} else {
		realm_name = argc < 2 ? NULL : argv[1];
		ret = perform_leave (client, realm_name, arg_use_ldaps,
		                     arg_remove, arg_user, arg_client_software,
		                     arg_server_software);
	}

	g_free (arg_user);
	g_free (arg_client_software);
	g_free (arg_server_software);
	g_option_context_free (context);
	return ret;
}
