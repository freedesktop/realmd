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
call_join (RealmDbusKerberosMembership *membership,
           GVariant *credentials,
           GVariant *options,
           GError **error)
{
	SyncClosure sync;
	gboolean ret;

	sync.result = NULL;
	sync.loop = g_main_loop_new (NULL, FALSE);

	/* Start actual operation */
	realm_dbus_kerberos_membership_call_join (membership, credentials, options,
	                                          NULL, on_complete_get_result, &sync);

	/* This mainloop is quit by on_complete_get_result */
	g_main_loop_run (sync.loop);

	ret = realm_dbus_kerberos_membership_call_join_finish (membership, sync.result, error);

	g_object_unref (sync.result);
	g_main_loop_unref (sync.loop);

	return ret ? 0 : 1;
}

static int
perform_otp_join (RealmClient *client,
                  RealmDbusKerberosMembership *membership,
                  const gchar *one_time_password,
                  GVariant *options)
{
	GVariant *supported;
	GVariant *credentials;
	GError *error = NULL;
	int ret;

	supported = realm_dbus_kerberos_membership_get_supported_join_credentials (membership);
	credentials = realm_client_build_otp_creds (client, supported, one_time_password, &error);
	if (credentials == NULL) {
		realm_handle_error (error, NULL);
		return 1;
	}

	ret = call_join (membership, credentials, options, &error);

	if (error != NULL)
		realm_handle_error (error, _("Couldn't join realm"));

	return ret;
}

static int
perform_automatic_join (RealmClient *client,
                        RealmDbusKerberosMembership *membership,
                        GVariant *options,
                        gboolean *try_other)
{
	RealmDbusKerberos *kerberos;
	GVariant *supported;
	GVariant *credentials;
	GError *error = NULL;
	gchar *remote;
	int ret;

	supported = realm_dbus_kerberos_membership_get_supported_join_credentials (membership);
	kerberos = realm_client_to_kerberos (client, membership);

	credentials = realm_client_build_automatic_creds (client, kerberos, supported, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		*try_other = TRUE;
		return 1;
	} else if (credentials == NULL) {
		realm_handle_error (error, NULL);
		return 1;
	}

	ret = call_join (membership, credentials, options, &error);

	if (error != NULL) {
		remote = g_dbus_error_get_remote_error (error);
		if (g_strcmp0 (remote, REALM_DBUS_ERROR_AUTH_FAILED) == 0 ||
		    g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED)) {
			*try_other = TRUE;
			g_error_free (error);
		} else {
			*try_other = FALSE;
			realm_handle_error (error, _("Couldn't join realm"));
		}
		g_free (remote);
	}

	return ret;
}

static int
perform_user_join (RealmClient *client,
                   RealmDbusKerberosMembership *membership,
                   const gchar *user_name,
                   GVariant *options)
{
	GVariant *supported;
	GVariant *credentials;
	GError *error = NULL;
	int ret;

	supported = realm_dbus_kerberos_membership_get_supported_join_credentials (membership);

	credentials = realm_client_build_principal_creds (client, membership, supported,
	                                                  user_name, &error);
	if (credentials == NULL) {
		realm_handle_error (error, NULL);
		return 1;
	}

	ret = call_join (membership, credentials, options, &error);

	if (error != NULL)
		realm_handle_error (error, _("Couldn't join realm"));

	return ret;
}

typedef struct {
	gchar *user;
	gchar *computer_ou;
	gchar *computer_name;
	gchar *os_name;
	gchar *os_version;
	gchar *client_software;
	gchar *server_software;
	gchar *membership_software;
	gboolean no_password;
	gchar *one_time_password;
	gchar *user_principal;
	gboolean automatic_id_mapping_set;
	gboolean automatic_id_mapping;
	gboolean use_ldaps;
} RealmJoinArgs;

static void
realm_join_args_clear (gpointer data)
{
	RealmJoinArgs *args = data;
	g_free (args->user);
	g_free (args->computer_ou);
	g_free (args->computer_name);
	g_free (args->client_software);
	g_free (args->server_software);
	g_free (args->user_principal);
}

static gboolean
realm_join_arg_id_mapping (const gchar *option_name,
                           const gchar *value,
                           gpointer data,
                           GError **error)
{
	RealmJoinArgs *args = data;
	args->automatic_id_mapping_set = TRUE;
	return realm_parse_boolean (option_name, value, TRUE, &args->automatic_id_mapping, error);
}

static int
perform_join (RealmClient *client,
              const gchar *string,
              RealmJoinArgs *args)
{
	RealmDbusKerberosMembership *membership;
	gboolean had_mismatched = FALSE;
	gboolean try_other = FALSE;
	RealmDbusRealm *realm;
	GError *error = NULL;
	GVariant *options;
	GList *realms;
	gint ret;

	realms = realm_client_discover (client, string, args->use_ldaps, args->client_software,
	                                args->server_software, args->membership_software,
	                                REALM_DBUS_KERBEROS_MEMBERSHIP_INTERFACE,
	                                &had_mismatched, &error);

	if (error != NULL) {
		realm_handle_error(error, NULL);
		return 1;
	} else if (realms == NULL) {
		if (had_mismatched)
			realm_handle_error (NULL, _("Cannot join this realm"));
		else
			realm_handle_error(NULL, _("No such realm found"));
		return 1;
	}

	membership = realms->data;
	realm = realm_client_to_realm (client, membership);
	if (realm_is_configured (realm)) {
		realm_handle_error (NULL, _("Already joined to this domain"));
		return 1;
	}

	options = realm_build_options (REALM_DBUS_OPTION_COMPUTER_OU, args->computer_ou,
	                               REALM_DBUS_OPTION_COMPUTER_NAME, args->computer_name,
	                               REALM_DBUS_OPTION_OS_NAME, args->os_name,
	                               REALM_DBUS_OPTION_OS_VERSION, args->os_version,
	                               REALM_DBUS_OPTION_MEMBERSHIP_SOFTWARE, args->membership_software,
	                               REALM_DBUS_OPTION_USER_PRINCIPAL, args->user_principal,
	                               REALM_DBUS_OPTION_USE_LDAPS, args->use_ldaps ? "True" : "False",
	                               args->automatic_id_mapping_set ?
	                                   REALM_DBUS_OPTION_AUTOMATIC_ID_MAPPING : NULL,
	                                   args->automatic_id_mapping,
	                               NULL);
	g_variant_ref_sink (options);

	if (args->no_password) {
		ret = perform_automatic_join (client, membership, options, &try_other);

	} else if (args->one_time_password) {
		ret = perform_otp_join (client, membership, args->one_time_password, options);

	} else if (args->user) {
		ret = perform_user_join (client, membership, args->user, options);

	} else {
		ret = perform_automatic_join (client, membership, options, &try_other);
		if (try_other)
			ret = perform_user_join (client, membership, args->user, options);
	}

	g_variant_unref (options);
	g_list_free_full (realms, g_object_unref);
	return ret;
}

int
realm_join (RealmClient *client,
            int argc,
            char *argv[])
{
	GOptionContext *context;
	GError *error = NULL;
	const gchar *realm_name;
	RealmJoinArgs args;
	GOptionGroup *group;
	gint ret = 0;

	GOptionEntry option_entries[] = {
		{ "automatic-id-mapping", 0, G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK,
		  realm_join_arg_id_mapping, N_("Turn off automatic id mapping"), "no" },
		{ "client-software", 0, 0, G_OPTION_ARG_STRING, &args.client_software,
		  N_("Use specific client software"), NULL },
		{ "computer-name", 0, 0, G_OPTION_ARG_STRING, &args.computer_name,
                  N_("Use specific computer name instead of hostname"), NULL },
		{ "computer-ou", 0, 0, G_OPTION_ARG_STRING, &args.computer_ou,
		  N_("Computer OU DN to join"), NULL },
		{ "membership-software", 0, 0, G_OPTION_ARG_STRING, &args.membership_software,
		  N_("Use specific membership software"), NULL },
		{ "no-password", 0, 0, G_OPTION_ARG_NONE, &args.no_password,
		  N_("Join automatically without a password"), NULL },
		{ "one-time-password", 0, 0, G_OPTION_ARG_STRING, &args.one_time_password,
		  N_("Join using a preset one time password"), NULL },
		{ "os-name", 0, 0, G_OPTION_ARG_STRING, &args.os_name,
                  N_("Use specific operation system name"), NULL },
		{ "os-version", 0, 0, G_OPTION_ARG_STRING, &args.os_version,
                  N_("Use specific operation system version"), NULL },
		{ "server-software", 0, 0, G_OPTION_ARG_STRING, &args.server_software,
		  N_("Use specific server software"), NULL },
		{ "user", 'U', 0, G_OPTION_ARG_STRING, &args.user,
		  N_("User name to use for enrollment"), NULL },
		{ "user-principal", 0, 0, G_OPTION_ARG_STRING, &args.user_principal,
		  N_("Set the user principal for the computer account"), NULL },
		{ "use-ldaps", 0, 0, G_OPTION_ARG_NONE, &args.use_ldaps,
		  N_("Use ldaps to connect to LDAP"), NULL },
		{ NULL, }
	};

	memset (&args, 0, sizeof (args));

	context = g_option_context_new ("join REALM");
	g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

	group = g_option_group_new (NULL, NULL, NULL, &args, realm_join_args_clear);
	g_option_group_add_entries (group, option_entries);
	g_option_group_add_entries (group, realm_global_options);
	g_option_context_set_main_group (context, group);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_printerr ("%s: %s\n", g_get_prgname (), error->message);
		g_error_free (error);
		ret = 2;

	} else if (argc > 2) {
		g_printerr ("%s: %s\n", g_get_prgname (), _("Specify one realm to join"));
		ret = 2;

	} else if (args.no_password && (args.one_time_password || args.user)) {
		g_printerr ("%s: %s\n", g_get_prgname (),
		            _("The --no-password argument cannot be used with --one-time-password or --user"));
		ret = 2;

	} else if (args.one_time_password && args.user) {
		g_printerr ("%s: %s\n", g_get_prgname (),
		            _("The --one-time-password argument cannot be used with --user"));
		ret = 2;

	} else {
		realm_name = argc < 2 ? "" : argv[1];
		ret = perform_join (client, realm_name, &args);
	}

	g_option_context_free (context);
	return ret;
}
