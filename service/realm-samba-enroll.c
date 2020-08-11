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

#include "realm-command.h"
#include "realm-daemon.h"
#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-dn-util.h"
#include "realm-errors.h"
#include "realm-options.h"
#include "realm-samba-config.h"
#include "realm-samba-enroll.h"
#include "realm-samba-provider.h"
#include "realm-settings.h"

#include <glib/gstdio.h>

#include <ldap.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

typedef struct {
	GDBusMethodInvocation *invocation;
	gchar *join_args[8];
	RealmDisco *disco;
	gchar *user_name;
	GBytes *password_input;
	RealmIniConfig *config;
	gchar *custom_smb_conf;
	gchar *envvar;
} JoinClosure;

static void
join_closure_free (gpointer data)
{
	JoinClosure *join = data;
	int i;

	g_bytes_unref (join->password_input);
	g_free (join->user_name);
	for (i = 0; i < G_N_ELEMENTS (join->join_args); i++)
		g_free (join->join_args[i]);
	realm_disco_unref (join->disco);
	g_free (join->envvar);
	g_clear_object (&join->invocation);
	g_clear_object (&join->config);

	if (join->custom_smb_conf) {
		if (!realm_daemon_has_debug_flag ())
			g_unlink (join->custom_smb_conf);
		g_free (join->custom_smb_conf);
	}

	g_free (join);
}

gchar *
fallback_workgroup (const gchar *realm)
{
	const gchar *pos;

	pos = strchr (realm, '.');
	if (pos == NULL)
		return g_utf8_strup (realm, -1);
	else
		return g_utf8_strup (realm, pos - realm);
}

static JoinClosure *
join_closure_init (GTask *task,
                   RealmDisco *disco,
                   GVariant *options,
                   GDBusMethodInvocation *invocation,
                   gboolean do_join)
{
	JoinClosure *join;
	gchar *workgroup;
	GError *error = NULL;
	int temp_fd;
	const gchar *explicit_computer_name = NULL;
	const gchar *authid = NULL;
	gchar *name_from_keytab = NULL;

	join = g_new0 (JoinClosure, 1);
	join->disco = realm_disco_ref (disco);
	join->invocation = invocation ? g_object_ref (invocation) : NULL;
	g_task_set_task_data (task, join, join_closure_free);

	explicit_computer_name = realm_options_computer_name (options, disco->domain_name);
	/* Set netbios name to explicit or truncated name if available */
	if (explicit_computer_name != NULL)
		authid = explicit_computer_name;
	else if (disco->explicit_netbios)
		authid = disco->explicit_netbios;

	/* try to get the NetBIOS name from the keytab while leaving the domain */
	if (explicit_computer_name == NULL && !do_join) {
		name_from_keytab = realm_kerberos_get_netbios_name_from_keytab(disco->kerberos_realm);
		if (name_from_keytab != NULL) {
			authid = name_from_keytab;
		}
	}

	join->config = realm_ini_config_new (REALM_INI_NO_WATCH | REALM_INI_PRIVATE);
	realm_ini_config_set (join->config, REALM_SAMBA_CONFIG_GLOBAL,
	                      "security", "ads",
	                      "kerberos method", "system keytab",
	                      "realm", disco->kerberos_realm,
	                      "netbios name", authid,
	                      NULL);

    /*
	 * Samba complains if we don't set a 'workgroup' setting for the realm we're
	 * going to join. If we didn't yet manage to lookup the workgroup, then go ahead
	 * and assume that the first domain component is the workgroup name.
	 */

	if (disco && disco->workgroup) {
		realm_ini_config_set (join->config, REALM_SAMBA_CONFIG_GLOBAL,
		                      "workgroup", disco->workgroup, NULL);

	} else {
		workgroup = fallback_workgroup (disco->domain_name);
		realm_ini_config_set (join->config, REALM_SAMBA_CONFIG_GLOBAL,
		                      "workgroup", workgroup, NULL);
		if (disco)
			disco->workgroup = workgroup;
		else
			g_free (workgroup);
	}

	/* Write out the config file for use by various net commands */
	join->custom_smb_conf = g_build_filename (g_get_tmp_dir (), "realmd-smb-conf.XXXXXX", NULL);
	temp_fd = g_mkstemp_full (join->custom_smb_conf, O_WRONLY, S_IRUSR | S_IWUSR);
	if (temp_fd != -1) {
		if (realm_ini_config_write_fd (join->config, temp_fd, &error)) {
			realm_ini_config_set_filename (join->config, join->custom_smb_conf);

		} else {
			g_warning ("couldn't write to a temp file: %s: %s", join->custom_smb_conf, error->message);
			g_error_free (error);
		}

		close (temp_fd);
	} else {
		g_warning ("Couldn't create temp file in: %s", g_get_tmp_dir ());
	}

	g_free (name_from_keytab);
	return join;
}

static void
begin_net_process (JoinClosure *join,
                   GBytes *input,
                   GAsyncReadyCallback callback,
                   gpointer user_data,
                   ...) G_GNUC_NULL_TERMINATED;

static void
begin_net_process (JoinClosure *join,
                   GBytes *input,
                   GAsyncReadyCallback callback,
                   gpointer user_data,
                   ...)
{
	char *env[8];
	GPtrArray *args;
	gchar *logenv = NULL;
	gchar *arg;
	va_list va;
	int at = 0;

	env[at++] = "LANG=C";

	/*
	 * HACK: Samba's 'net ads -k join' requires that LOGNAME is set or
	 * otherwise it fails to authenticate.
	 */
	if (!g_getenv ("LOGNAME"))
		env[at++] = logenv = g_strdup_printf ("LOGNAME=%s", g_get_user_name ());
	if (join->envvar)
		env[at++] = join->envvar;

	env[at++] = NULL;
	g_assert (at < G_N_ELEMENTS (env));

	args = g_ptr_array_new ();

	/* Use our custom smb.conf */
	g_ptr_array_add (args, (gpointer)realm_settings_path ("net"));
	if (join->custom_smb_conf) {
		g_ptr_array_add (args, "-s");
		g_ptr_array_add (args, join->custom_smb_conf);
	}

	if (join->disco->explicit_server) {
		g_ptr_array_add (args, "-S");
		g_ptr_array_add (args, join->disco->explicit_server);
	}

	va_start (va, user_data);
	do {
		arg = va_arg (va, gchar *);
		g_ptr_array_add (args, arg);
	} while (arg != NULL);
	va_end (va);

	realm_command_runv_async ((gchar **)args->pdata, env, input,
	                          join->invocation, callback, user_data);

	g_free (logenv);
	g_ptr_array_free (args, TRUE);
}

static void
on_keytab_do_finish (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GError *error = NULL;
	gint status;

	status = realm_command_run_finish (result, NULL, &error);
	if (error == NULL && status != 0)
		g_set_error (&error, REALM_ERROR, REALM_ERROR_INTERNAL,
		             "Extracting host keytab failed");

	if (error != NULL)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
	g_object_unref (task);
}

static void
on_join_do_keytab (GObject *source,
                   GAsyncResult *result,
                   gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	JoinClosure *join = g_task_get_task_data (task);
	GError *error = NULL;
	GString *output = NULL;
	gint status;

	status = realm_command_run_finish (result, &output, &error);
	if (error == NULL && status != 0) {

		/*
		 * This is bad and ugly. We run the process with LC_ALL=C so
		 * at least we know these messages will be in english.
		 *
		 * At first I thought this was a deficiency in samba's 'net'
		 * command. It's true that 'net' could be better at returning
		 * different error codes for different types of failures.
		 *
		 * But in the end this is a deficiency in Windows. When you use
		 * LDAP to do enrollment, and the permissions aren't correct
		 * it often returns stupid errors such as 'Constraint violation'
		 * or 'Object class invalid' instead of 'Insufficient access'.
		 */
		if (g_pattern_match_simple ("*NT_STATUS_ACCESS_DENIED*", output->str) ||
		    g_pattern_match_simple ("*failed*: Constraint violation*", output->str) ||
		    g_pattern_match_simple ("*failed*: Object class violation*", output->str) ||
		    g_pattern_match_simple ("*failed*: Insufficient access*", output->str) ||
		    g_pattern_match_simple ("*: Access denied*", output->str) ||
		    g_pattern_match_simple ("*not have administrator privileges*", output->str) ||
		    g_pattern_match_simple ("*failure*: *not been granted the requested logon type*", output->str) ||
		    g_pattern_match_simple ("*failure*: User not allowed to log on to this computer*", output->str) ||
		    g_pattern_match_simple ("*failure*: *specified account is not allowed to authenticate to the machine*", output->str)) {
			g_set_error (&error, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
			             "Insufficient permissions to join the domain %s",
			             join->disco->domain_name);
		} else if (g_pattern_match_simple ("*: Logon failure*", output->str) ||
		           g_pattern_match_simple ("*: Password expired*", output->str)) {
			g_set_error (&error, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
			             "The %s account, password, or credentials are invalid",
			             join->user_name);
		} else {
			g_set_error (&error, REALM_ERROR, REALM_ERROR_INTERNAL,
			             "Joining the domain %s failed", join->disco->domain_name);
		}
	}

	if (output)
		g_string_free (output, TRUE);

	if (error != NULL) {
		g_task_return_error (task, error);

	/* Do keytab with a user name */
	} else if (join->user_name != NULL) {
		begin_net_process (join, join->password_input,
		                   on_keytab_do_finish, g_object_ref (task),
		                   "-U", join->user_name, "ads", "keytab", "create", NULL);

	/* Do keytab with a ccache file */
	} else {
		begin_net_process (join, NULL,
		                   on_keytab_do_finish, g_object_ref (task),
		                   "-k", "ads", "keytab", "create", NULL);
	}

	g_object_unref (task);
}

static void
begin_join (GTask *task,
            JoinClosure *join,
            GVariant *options)
{
	const gchar *computer_ou;
	gchar *strange_ou;
	GError *error = NULL;
	const gchar *upn;
	const gchar *os;
	int at = 0;

	computer_ou = realm_options_computer_ou (options, join->disco->domain_name);
	if (computer_ou != NULL) {
		strange_ou = realm_dn_util_build_samba_ou (computer_ou, join->disco->domain_name);
		if (strange_ou) {
			if (!g_str_equal (strange_ou, ""))
				join->join_args[at++] = g_strdup_printf ("createcomputer=%s", strange_ou);
			g_free (strange_ou);
		} else {
			g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			             "The computer-ou argument must be a valid LDAP DN and contain only OU=xxx RDN values.");
		}
	}

	os = realm_options_ad_specific(options, "os-name");
	if (os != NULL && !g_str_equal (os, ""))
		join->join_args[at++] = g_strdup_printf ("osName=%s", os);

	os = realm_options_ad_specific(options, "os-version");
	if (os != NULL && !g_str_equal (os, ""))
		join->join_args[at++] = g_strdup_printf ("osVer=%s", os);

	upn = realm_options_user_principal (options, join->disco->domain_name);
	if (upn) {
		if (g_str_equal (upn, ""))
			upn = NULL;
		join->join_args[at++] = g_strdup_printf ("createupn%s%s",
		                                         upn ? "=" : "",
		                                         upn ? upn : "");
	}

	g_assert (at < G_N_ELEMENTS (join->join_args));

	if (error != NULL) {
		g_task_return_error (task, error);

	/* Do join with a user name */
	} else if (join->user_name) {
		begin_net_process (join, join->password_input,
		                   on_join_do_keytab, g_object_ref (task),
		                   "-U", join->user_name,
		                   "-k", "ads", "join", join->disco->domain_name,
		                   join->join_args[0], join->join_args[1],
		                   join->join_args[2], join->join_args[3],
		                   join->join_args[4], NULL);

	/* Do join with a ccache */
	} else {
		begin_net_process (join, NULL,
		                   on_join_do_keytab, g_object_ref (task),
		                   "-k", "ads", "join", join->disco->domain_name,
		                   join->join_args[0], join->join_args[1],
		                   join->join_args[2], join->join_args[3],
		                   join->join_args[4], NULL);
	}
}

void
realm_samba_enroll_join_async (RealmDisco *disco,
                               RealmCredential *cred,
                               GVariant *options,
                               GDBusMethodInvocation *invocation,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	GTask *task;
	JoinClosure *join;
	const gchar *explicit_computer_name;

	g_return_if_fail (disco != NULL);
	g_return_if_fail (cred != NULL);

	task = g_task_new (NULL, NULL, callback, user_data);
	join = join_closure_init (task, disco, options, invocation, TRUE);
	explicit_computer_name = realm_options_computer_name (options, disco->domain_name);
	if (explicit_computer_name != NULL) {
		realm_diagnostics_info (invocation, "Joining using a manual netbios name: %s",
		                        explicit_computer_name);
	} else if (disco->explicit_netbios) {
		realm_diagnostics_info (invocation, "Joining using a truncated netbios name: %s",
		                        disco->explicit_netbios);
	}

	switch (cred->type) {
	case REALM_CREDENTIAL_PASSWORD:
		join->password_input = realm_command_build_password_line (cred->x.password.value);
		join->user_name = g_strdup (cred->x.password.name);
		break;
	case REALM_CREDENTIAL_CCACHE:
		join->envvar = g_strdup_printf ("KRB5CCNAME=%s", cred->x.ccache.file);
		break;
	default:
		g_return_if_reached ();
	}

	begin_join (task, join, options);

	g_object_unref (task);
}

gboolean
realm_samba_enroll_join_finish (GAsyncResult *result,
                                GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
on_leave_complete (GObject *source,
                   GAsyncResult *result,
                   gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	JoinClosure *join = g_task_get_task_data (task);
	GError *error = NULL;
	gint status;

	status = realm_command_run_finish (result, NULL, &error);
	if (error == NULL && status != 0)
		g_set_error (&error, REALM_ERROR, REALM_ERROR_INTERNAL,
		             "Leaving the domain %s failed", join->disco->domain_name);

	if (error != NULL)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
	g_object_unref (task);
}

void
realm_samba_enroll_leave_async (RealmDisco *disco,
                                RealmCredential *cred,
                                GVariant *options,
                                GDBusMethodInvocation *invocation,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	GTask *task;
	JoinClosure *join;

	task = g_task_new (NULL, NULL, callback, user_data);
	join = join_closure_init (task, disco, options, invocation, FALSE);

	switch (cred->type) {
	case REALM_CREDENTIAL_PASSWORD:
		join->password_input = realm_command_build_password_line (cred->x.password.value);
		join->user_name = g_strdup (cred->x.password.name);
		begin_net_process (join, join->password_input,
		                   on_leave_complete, g_object_ref (task),
		                   "-U", join->user_name, "ads", "leave", NULL);
		break;
	case REALM_CREDENTIAL_CCACHE:
		join->envvar = g_strdup_printf ("KRB5CCNAME=%s", cred->x.ccache.file);
		begin_net_process (join, NULL,
		                   on_leave_complete, g_object_ref (task),
		                   "-k", "ads", "leave", NULL);
		break;
	default:
		g_return_if_reached ();
	}


	g_object_unref (task);
}

gboolean
realm_samba_enroll_leave_finish (GAsyncResult *result,
                                 GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}
