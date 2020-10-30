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

#include "realm-adcli-enroll.h"
#include "realm-command.h"
#include "realm-daemon.h"
#include "realm-diagnostics.h"
#include "realm-dn-util.h"
#include "realm-errors.h"
#include "realm-ini-config.h"
#include "realm-options.h"
#include "realm-settings.h"

static void
on_join_process (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GError *error = NULL;
	GString *output = NULL;
	gint status;

	status = realm_command_run_finish (result, &output, &error);
	if (error == NULL && status != 0) {
		switch (status) {
		case 2: /* ADCLI_ERR_UNEXPECTED */
			g_set_error (&error, REALM_ERROR, REALM_ERROR_INTERNAL,
			             "Internal unexpected error joining the domain");
			break;
		case 6: /* ADCLI_ERR_CREDENTIALS */
			g_set_error (&error, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
			             "Insufficient permissions to join the domain");
			break;
		default:
			g_set_error (&error, REALM_ERROR, REALM_ERROR_FAILED,
			             "Failed to join the domain");
			break;
		}
	}

	if (error == NULL) {
		g_task_return_boolean (task, TRUE);

	} else {
		g_task_return_error (task, error);
	}

	if (output)
		g_string_free (output, TRUE);
	g_object_unref (task);
}

void
realm_adcli_enroll_join_async (RealmDisco *disco,
                               RealmCredential *cred,
                               GVariant *options,
                               gboolean use_ldaps,
                               GDBusMethodInvocation *invocation,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	gchar *environ[] = { "LANG=C", NULL };
	GInetAddress *address;
	const gchar *computer_ou;
	GTask *task;
	GBytes *input = NULL;
	const gchar *upn;
	GPtrArray *args;
	const gchar *os_n = NULL;
	const gchar *os_v = NULL;
	gchar *ccache_arg = NULL;
	gchar *upn_arg = NULL;
	gchar *server_arg = NULL;
	gchar *ou_arg = NULL;
	const gchar *computer_name = NULL;

	g_return_if_fail (cred != NULL);
	g_return_if_fail (disco != NULL);
	g_return_if_fail (invocation != NULL);

	task = g_task_new (NULL, NULL, callback, user_data);
	args = g_ptr_array_new ();

	/* Use our custom smb.conf */
	g_ptr_array_add (args, (gpointer)realm_settings_path ("adcli"));
	g_ptr_array_add (args, "join");
	g_ptr_array_add (args, "--verbose");
	g_ptr_array_add (args, "--domain");
	g_ptr_array_add (args, (gpointer)disco->domain_name);
	g_ptr_array_add (args, "--domain-realm");
	g_ptr_array_add (args, (gpointer)disco->kerberos_realm);

	if (use_ldaps) {
		g_ptr_array_add (args, "--use-ldaps");
	}

	if (G_IS_INET_SOCKET_ADDRESS (disco->server_address)) {
		address = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (disco->server_address));
		server_arg = g_inet_address_to_string (address);
		if (server_arg) {
			g_ptr_array_add (args, "--domain-controller");
			g_ptr_array_add (args, server_arg);
		}

	} else if (disco->explicit_server) {
		g_ptr_array_add (args, "--domain-controller");
		g_ptr_array_add (args, (gpointer)disco->explicit_server);
	}

		/* Pass manually configured or truncated computer name to adcli */
		computer_name = realm_options_computer_name (options, disco->domain_name);
		if (computer_name != NULL) {
			realm_diagnostics_info (invocation, "Joining using a manual netbios name: %s",
			                        computer_name);
			g_ptr_array_add (args, "--computer-name");
			g_ptr_array_add (args, (gpointer)computer_name);
		} else if (disco->explicit_netbios) {
		realm_diagnostics_info (invocation, "Joining using a truncated netbios name: %s",
		                        disco->explicit_netbios);
		g_ptr_array_add (args, "--computer-name");
		g_ptr_array_add (args, disco->explicit_netbios);
	}

	computer_ou = realm_options_computer_ou (options, disco->domain_name);
	if (computer_ou != NULL) {
		ou_arg = realm_dn_util_build_qualified (computer_ou, disco->domain_name);
		g_ptr_array_add (args, "--computer-ou");
		if (ou_arg)
			g_ptr_array_add (args, ou_arg);
		else
			g_ptr_array_add (args, (gpointer)computer_ou);
	}

	os_n = realm_options_ad_specific (options, "os-name");
	if (os_n != NULL && !g_str_equal (os_n, "")) {
		g_ptr_array_add (args, "--os-name");
		g_ptr_array_add (args, (gpointer)os_n);
	}

	os_v = realm_options_ad_specific (options, "os-version");
	if (os_v != NULL && !g_str_equal (os_v, "")) {
		g_ptr_array_add (args, "--os-version");
		g_ptr_array_add (args, (gpointer)os_v);
	}

	switch (cred->type) {
	case REALM_CREDENTIAL_AUTOMATIC:
		g_ptr_array_add (args, "--login-type");
		g_ptr_array_add (args, "computer");
		g_ptr_array_add (args, "--no-password");
		break;
	case REALM_CREDENTIAL_CCACHE:
		g_ptr_array_add (args, "--login-type");
		g_ptr_array_add (args, "user");
		ccache_arg = g_strdup_printf ("--login-ccache=%s", cred->x.ccache.file);
		g_ptr_array_add (args, ccache_arg);
		break;
	case REALM_CREDENTIAL_PASSWORD:
		input = g_bytes_ref (cred->x.password.value);
		g_ptr_array_add (args, "--login-type");
		g_ptr_array_add (args, "user");
		g_ptr_array_add (args, "--login-user");
		g_ptr_array_add (args, cred->x.password.name);
		g_ptr_array_add (args, "--stdin-password");
		break;
	case REALM_CREDENTIAL_SECRET:
		input = g_bytes_ref (cred->x.secret.value);
		g_ptr_array_add (args, "--login-type");
		g_ptr_array_add (args, "computer");
		g_ptr_array_add (args, "--stdin-password");
		break;
	}

	upn = realm_options_user_principal (options, disco->domain_name);
	if (upn) {
		if (g_str_equal (upn, "")) {
			g_ptr_array_add (args, "--user-principal");
		} else {
			upn_arg = g_strdup_printf ("--user-principal=%s", upn);
			g_ptr_array_add (args, upn_arg);
		}
	}

	g_ptr_array_add (args, NULL);

	realm_command_runv_async ((gchar **)args->pdata, environ, input,
	                          invocation, on_join_process,
	                          g_object_ref (task));

	g_ptr_array_free (args, TRUE);
	g_object_unref (task);

	if (input)
		g_bytes_unref (input);
	free (ccache_arg);
	free (upn_arg);
	free (server_arg);
	free (ou_arg);
}

gboolean
realm_adcli_enroll_join_finish (GAsyncResult *result,
                                GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}

void
realm_adcli_enroll_delete_async (RealmDisco *disco,
                                 RealmCredential *cred,
                                 GVariant *options,
                                 gboolean use_ldaps,
                                 GDBusMethodInvocation *invocation,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
	gchar *environ[] = { "LANG=C", NULL };
	GInetAddress *address;
	GTask *task;
	GBytes *input = NULL;
	GPtrArray *args;
	gchar *ccache_arg = NULL;
	gchar *server_arg = NULL;

	g_return_if_fail (cred != NULL);
	g_return_if_fail (disco != NULL);
	g_return_if_fail (invocation != NULL);

	task = g_task_new (NULL, NULL, callback, user_data);
	args = g_ptr_array_new ();

	/* Use our custom smb.conf */
	g_ptr_array_add (args, (gpointer)realm_settings_path ("adcli"));
	g_ptr_array_add (args, "delete-computer");
	g_ptr_array_add (args, "--verbose");
	g_ptr_array_add (args, "--domain");
	g_ptr_array_add (args, (gpointer)disco->domain_name);
	g_ptr_array_add (args, "--domain-realm");
	g_ptr_array_add (args, (gpointer)disco->kerberos_realm);

	if (use_ldaps) {
		g_ptr_array_add (args, "--use-ldaps");
	}

	if (G_IS_INET_SOCKET_ADDRESS (disco->server_address)) {
		address = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (disco->server_address));
		server_arg = g_inet_address_to_string (address);
		if (server_arg) {
			g_ptr_array_add (args, "--domain-controller");
			g_ptr_array_add (args, server_arg);
		}

	} else if (disco->explicit_server) {
		g_ptr_array_add (args, "--domain-controller");
		g_ptr_array_add (args, (gpointer)disco->explicit_server);
	}

	switch (cred->type) {
	case REALM_CREDENTIAL_AUTOMATIC:
	case REALM_CREDENTIAL_SECRET:
		g_return_if_reached ();
		break;
	case REALM_CREDENTIAL_CCACHE:
		ccache_arg = g_strdup_printf ("--login-ccache=%s", cred->x.ccache.file);
		g_ptr_array_add (args, ccache_arg);
		break;
	case REALM_CREDENTIAL_PASSWORD:
		input = g_bytes_ref (cred->x.password.value);
		g_ptr_array_add (args, "--login-user");
		g_ptr_array_add (args, cred->x.password.name);
		g_ptr_array_add (args, "--stdin-password");
		break;
	}

	g_ptr_array_add (args, NULL);

	realm_command_runv_async ((gchar **)args->pdata, environ, input,
	                          invocation, on_join_process,
	                          g_object_ref (task));

	g_ptr_array_free (args, TRUE);
	g_object_unref (task);

	if (input)
		g_bytes_unref (input);

	free (ccache_arg);
	g_free (server_arg);
}

gboolean
realm_adcli_enroll_delete_finish (GAsyncResult *result,
                                  GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}
