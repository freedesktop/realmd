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
#include "realm-disco.h"
#include "realm-errors.h"
#include "realm-kerberos.h"
#include "realm-kerberos-config.h"
#include "realm-kerberos-membership.h"
#include "realm-options.h"
#include "realm-packages.h"
#include "realm-provider.h"
#include "realm-samba.h"
#include "realm-samba-config.h"
#include "realm-samba-enroll.h"
#include "realm-samba-winbind.h"
#include "realm-settings.h"

#include <glib/gstdio.h>
#include <glib/gi18n.h>

#include <errno.h>
#include <string.h>

struct _RealmSamba {
	RealmKerberos parent;
	RealmIniConfig *config;
	gulong config_sig;
};

typedef struct {
	RealmKerberosClass parent_class;
} RealmSambaClass;

enum {
	PROP_0,
	PROP_PROVIDER,
};

static const gchar *SAMBA_PACKAGES[] = {
	REALM_DBUS_IDENTIFIER_WINBIND,
	REALM_DBUS_IDENTIFIER_SAMBA,
	NULL
};

static void realm_samba_kerberos_membership_iface (RealmKerberosMembershipIface *iface);

G_DEFINE_TYPE_WITH_CODE (RealmSamba, realm_samba, REALM_TYPE_KERBEROS,
                         G_IMPLEMENT_INTERFACE (REALM_TYPE_KERBEROS_MEMBERSHIP, realm_samba_kerberos_membership_iface);
);

static void
realm_samba_init (RealmSamba *self)
{

}

static void
realm_samba_constructed (GObject *obj)
{
	RealmKerberos *kerberos = REALM_KERBEROS (obj);

	G_OBJECT_CLASS (realm_samba_parent_class)->constructed (obj);

	realm_kerberos_set_details (kerberos,
	                            REALM_DBUS_OPTION_SERVER_SOFTWARE, REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY,
	                            REALM_DBUS_OPTION_CLIENT_SOFTWARE, REALM_DBUS_IDENTIFIER_WINBIND,
	                            NULL);

	realm_kerberos_set_suggested_admin (kerberos, "Administrator");
	realm_kerberos_set_login_policy (kerberos, REALM_KERBEROS_ALLOW_ANY_LOGIN);
	realm_kerberos_set_required_package_sets (kerberos, SAMBA_PACKAGES);
}

static gchar *
lookup_enrolled_realm (RealmSamba *self)
{
	gchar *enrolled = NULL;
	gchar *security;

	security = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "security");
	if (security != NULL && g_ascii_strcasecmp (security, "ADS") == 0)
		enrolled = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "realm");
	return enrolled;
}

static gboolean
lookup_is_enrolled (RealmSamba *self)
{
	const gchar *name;
	gchar *enrolled;
	gboolean ret = FALSE;

	enrolled = lookup_enrolled_realm (self);
	if (enrolled != NULL) {
		name = realm_kerberos_get_realm_name (REALM_KERBEROS (self));
		ret = g_strcmp0 (name, enrolled) == 0;
		g_free (enrolled);
	}

	return ret;
}

static gchar *
lookup_login_prefix (RealmSamba *self)
{
	gchar *workgroup;
	gchar *separator;

	/* When using default, just have a direct login format */
	if (realm_samba_config_get_boolean (self->config, REALM_SAMBA_CONFIG_GLOBAL,
	                                    "winbind use default domain", FALSE))
		return g_strdup ("");

	workgroup = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "workgroup");
	if (workgroup == NULL)
		return NULL;

	separator = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "winbind separator");
	if (separator == NULL)
		separator = g_strdup ("\\");

	return g_strdup_printf ("%s%s", workgroup, separator);
}

typedef struct {
	GDBusMethodInvocation *invocation;
	GVariant *options;
	RealmDisco *disco;
	RealmCredential *cred;
} EnrollClosure;

static void
enroll_closure_free (gpointer data)
{
	EnrollClosure *enroll = data;
	realm_disco_unref (enroll->disco);
	g_variant_unref (enroll->options);
	realm_credential_unref (enroll->cred);
	g_object_unref (enroll->invocation);
	g_free (enroll);
}

static void
on_winbind_done (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GError *error = NULL;

	realm_samba_winbind_configure_finish (result, &error);
	if (error != NULL)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
	g_object_unref (task);
}

static void
on_join_do_winbind (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	EnrollClosure *enroll = g_task_get_task_data (task);
	RealmSamba *self = g_task_get_source_object (task);
	GError *error = NULL;
	const gchar *name;
	const gchar *computer_name;

	computer_name = realm_options_computer_name (enroll->options, enroll->disco->domain_name);
	/* Use truncated name if set and explicit name is not available */
	if (enroll->disco->explicit_netbios && computer_name == NULL)
		computer_name = enroll->disco->explicit_netbios;


	realm_samba_enroll_join_finish (result, &error);
	if (error == NULL) {
		realm_ini_config_change (self->config, REALM_SAMBA_CONFIG_GLOBAL, &error,
		                         "security", "ads",
		                         "realm", enroll->disco->kerberos_realm,
		                         "workgroup", enroll->disco->workgroup,
		                         "template homedir", realm_settings_string ("users", "default-home"),
		                         "template shell", realm_settings_string ("users", "default-shell"),
		                         "netbios name", computer_name,
		                         "password server", enroll->disco->explicit_server,
		                         "kerberos method", "secrets and keytab",
		                         NULL);
	}

	if (error == NULL && enroll->disco->dns_fqdn != NULL) {
		realm_ini_config_change (self->config, REALM_SAMBA_CONFIG_GLOBAL, &error,
		                         "additional dns hostnames", enroll->disco->dns_fqdn,
		                         NULL);
	}

	if (error == NULL) {
		configure_krb5_conf_for_domain (enroll->disco->kerberos_realm, &error);
		if (error != NULL) {
			realm_diagnostics_error (enroll->invocation, error,
			                         "Failed to update Kerberos "
			                         "configuration, not fatal, "
			                         "please check manually");
			g_clear_error (&error);
		}
	}

	if (error == NULL) {
		name = realm_kerberos_get_name (REALM_KERBEROS (self));
		realm_samba_winbind_configure_async (self->config, name, enroll->options,
		                                     enroll->invocation,
		                                     on_winbind_done, g_object_ref (task));
	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static void
on_install_do_join (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	EnrollClosure *enroll = g_task_get_task_data (task);
	GError *error = NULL;

	realm_packages_install_finish (result, &error);
	if (error == NULL) {
		realm_samba_enroll_join_async (enroll->disco, enroll->cred, enroll->options,
		                               enroll->invocation, on_join_do_winbind,
		                               g_object_ref (task));

	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static gboolean
validate_membership_options (GVariant *options,
                             GError **error)
{
	const gchar *software;

	/* Figure out the method that we're going to use to enroll */
	if (g_variant_lookup (options, REALM_DBUS_OPTION_MEMBERSHIP_SOFTWARE, "&s", &software)) {
		if (!g_str_equal (software, REALM_DBUS_IDENTIFIER_SAMBA)) {
			g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			             _("Unsupported or unknown membership software '%s'"), software);
			return FALSE;
		}
	}

	return TRUE;
}

static void
realm_samba_join_async (RealmKerberosMembership *membership,
                        RealmCredential *cred,
                        GVariant *options,
                        GDBusMethodInvocation *invocation,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
	RealmKerberos *realm = REALM_KERBEROS (membership);
	RealmSamba *self = REALM_SAMBA (realm);
	GTask *task;
	EnrollClosure *enroll;
	GError *error = NULL;
	gchar *enrolled;

	task = g_task_new (realm, NULL, callback, user_data);
	enroll = g_new0 (EnrollClosure, 1);
	enroll->disco = realm_disco_ref (realm_kerberos_get_disco (realm));
	enroll->invocation = g_object_ref (invocation);
	enroll->options = g_variant_ref (options);
	enroll->cred = realm_credential_ref (cred);
	g_task_set_task_data (task, enroll, enroll_closure_free);

	/* Make sure not already enrolled in a realm */
	enrolled = lookup_enrolled_realm (self);
	if (enrolled != NULL) {
		g_task_return_new_error (task, REALM_ERROR, REALM_ERROR_ALREADY_CONFIGURED,
		                         _("Already joined to a domain"));

	} else if (!validate_membership_options (options, &error)) {
		g_task_return_error (task, error);

	} else {
		realm_packages_install_async (SAMBA_PACKAGES, enroll->invocation,
		                              g_dbus_method_invocation_get_connection (enroll->invocation),
		                              on_install_do_join, g_object_ref (task));
	}

	g_free (enrolled);
	g_object_unref (task);
}

static const RealmCredential *
realm_samba_join_creds (RealmKerberosMembership *self)
{
	static const RealmCredential creds[] = {
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_ADMIN },
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_USER },
		{ REALM_CREDENTIAL_CCACHE, REALM_CREDENTIAL_OWNER_ADMIN },
		{ 0, },
	};

	return creds;
}

typedef struct {
	GDBusMethodInvocation *invocation;
	RealmDisco *disco;
} LeaveClosure;

static void
leave_closure_free (gpointer data)
{
	LeaveClosure *leave = data;
	realm_disco_unref (leave->disco);
	g_object_unref (leave->invocation);
	g_free (leave);
}

static void
on_deconfigure_done (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GError *error = NULL;

	realm_samba_winbind_deconfigure_finish (result, &error);
	if (error != NULL)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
	g_object_unref (task);
}

static void
leave_deconfigure_begin (RealmSamba *self,
                         GTask *task)
{
	LeaveClosure *leave;
	GError *error = NULL;

	leave = g_task_get_task_data (task);

	/* Flush the keytab of all the entries for this realm */
	realm_diagnostics_info (leave->invocation, "Removing entries from keytab for realm");

	if (!realm_kerberos_flush_keytab (leave->disco->kerberos_realm, &error)) {
		g_task_return_error (task, error);
		return;
	}

	/* Deconfigure smb.conf */
	realm_diagnostics_info (leave->invocation, "Updating smb.conf file");
	if (!realm_ini_config_change (self->config, REALM_SAMBA_CONFIG_GLOBAL, &error,
	                              "workgroup", NULL,
	                              "realm", NULL,
	                              "additional dns hostnames", NULL,
	                              "security", "user",
	                              NULL)) {
		g_task_return_error (task, error);
		return;
	}

	/* And then deconfigure winbind */
	realm_samba_winbind_deconfigure_async (self->config, leave->invocation,
	                                       on_deconfigure_done, g_object_ref (task));
}

static void
on_leave_do_deconfigure (GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	LeaveClosure *leave = g_task_get_task_data (task);
	RealmSamba *self = g_task_get_source_object (task);
	GError *error = NULL;

	/* We don't care if we can leave or not, just continue with other steps */
	realm_samba_enroll_leave_finish (result, &error);
	if (error != NULL) {
		realm_diagnostics_error (leave->invocation, error, NULL);
		g_error_free (error);
	}

	leave_deconfigure_begin (self, task);

	g_object_unref (task);
}

static void
realm_samba_leave_async (RealmKerberosMembership *membership,
                         RealmCredential *cred,
                         GVariant *options,
                         GDBusMethodInvocation *invocation,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
	RealmSamba *self = REALM_SAMBA (membership);
	RealmKerberos *kerberos = REALM_KERBEROS (self);
	GTask *task;
	LeaveClosure *leave;
	const gchar *realm_name;
	gchar *enrolled;

	realm_name = realm_kerberos_get_realm_name (kerberos);

	task = g_task_new (self, NULL, callback, user_data);
	leave = g_new0 (LeaveClosure, 1);
	leave->disco = realm_disco_ref (realm_kerberos_get_disco (kerberos));
	leave->invocation = g_object_ref (invocation);
	g_task_set_task_data (task, leave, leave_closure_free);

	/* Check that enrolled in this realm */
	enrolled = lookup_enrolled_realm (self);
	if (g_strcmp0 (enrolled, realm_name) != 0) {
		g_task_return_new_error (task, REALM_ERROR, REALM_ERROR_NOT_CONFIGURED,
		                         _("Not currently joined to this domain"));
		g_object_unref (task);
		return;
	}

	switch (cred->type) {
	case REALM_CREDENTIAL_PASSWORD:
		realm_samba_enroll_leave_async (leave->disco, cred, options, leave->invocation,
		                                on_leave_do_deconfigure, g_object_ref (task));
		break;
	case REALM_CREDENTIAL_AUTOMATIC:
		leave_deconfigure_begin (self, task);
		break;
	default:
		g_return_if_reached ();
	}

	g_object_unref (task);
}

static const RealmCredential *
realm_samba_leave_creds (RealmKerberosMembership *self)
{
	static const RealmCredential creds[] = {
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_ADMIN },
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_USER },
		{ REALM_CREDENTIAL_AUTOMATIC, REALM_CREDENTIAL_OWNER_NONE },
		{ 0, },
	};

	return creds;
}

static gboolean
realm_samba_change_logins (RealmKerberos *realm,
                           GDBusMethodInvocation *invocation,
                           const gchar **add,
                           const gchar **remove,
                           GError **error)
{
	RealmSamba *self = REALM_SAMBA (realm);
	gchar **names;

	if (!lookup_is_enrolled (self)) {
		g_set_error (error, REALM_ERROR, REALM_ERROR_NOT_CONFIGURED,
		             _("Not joined to this domain"));
		return FALSE;
	}

	/* We cannot handle removing logins */
	names = realm_kerberos_parse_logins (realm, TRUE, remove, error);
	if (names == NULL)
		return FALSE;
	if (names[0] != NULL) {
		g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
		             _("The Samba provider cannot restrict permitted logins."));
		g_strfreev (names);
		return FALSE;
	}

	g_strfreev (names);
	names = realm_kerberos_parse_logins (realm, TRUE, add, error);
	if (names == NULL)
		return FALSE;

	/*
	 * Samba cannot restrict the set of logins. We allow specific logins to be
	 * added, but not changing the mode to only allow the permitted logins.
	 * In addition we don't keep track of the list of permitted logins.
	 */

	g_strfreev (names);
	return TRUE;
}

static void
realm_samba_logins_async (RealmKerberos *realm,
                          GDBusMethodInvocation *invocation,
                          RealmKerberosLoginPolicy login_policy,
                          const gchar **add,
                          const gchar **remove,
                          GVariant *options,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	GTask *task;
	GError *error = NULL;

	task = g_task_new (realm, NULL, callback, user_data);

	if (login_policy == REALM_KERBEROS_ALLOW_REALM_LOGINS)
		login_policy = REALM_KERBEROS_ALLOW_ANY_LOGIN;

	/* Sadly we don't support this option */
	if (login_policy != REALM_KERBEROS_ALLOW_ANY_LOGIN &&
	    login_policy != REALM_KERBEROS_POLICY_NOT_SET) {
		g_task_return_new_error (task, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
		                         _("The Samba provider cannot restrict permitted logins."));

	/* Make note of the permitted logins, so we can return them in the property */
	} else if (!realm_samba_change_logins (realm, invocation, add, remove, &error)) {
		g_task_return_error (task, error);

	} else {
		g_task_return_boolean (task, TRUE);
	}

	g_object_unref (task);
}

static void
update_properties (RealmSamba *self)
{
	RealmKerberos *kerberos = REALM_KERBEROS (self);
	GPtrArray *permitted;
	gchar *login_formats[2] = { NULL, NULL };
	gboolean configured;
	const gchar *name;
	gchar *domain;
	gchar *realm;
	gchar *prefix;

	g_object_freeze_notify (G_OBJECT (self));

	name = realm_kerberos_get_name (kerberos);

	domain = name ? g_ascii_strdown (name, -1) : NULL;
	realm_kerberos_set_domain_name (kerberos, domain);
	g_free (domain);

	realm = name ? g_ascii_strup (name, -1) : NULL;
	realm_kerberos_set_realm_name (kerberos, realm);
	g_free (realm);

	/*
	 * Although samba domains do not do much management of the system or
	 * pull that much policy, we cannot limit who can log in from the domain
	 * and also cannot join more than one domain, so this we mark a
	 * configured domain as one that manages the system.
	 */
	configured = lookup_is_enrolled (self);
	realm_kerberos_set_configured (kerberos, configured);
	realm_kerberos_set_manages_system (kerberos, configured);

	/* Setup the workgroup property */
	prefix = lookup_login_prefix (self);
	if (prefix != NULL) {
		login_formats[0] = g_strdup_printf ("%s%%U", prefix);
		realm_kerberos_set_login_formats (kerberos, (const gchar **)login_formats);
		g_free (login_formats[0]);
		g_free (prefix);
	} else {
		login_formats[0] = "%U";
		realm_kerberos_set_login_formats (kerberos, (const gchar **)login_formats);
	}

	permitted = g_ptr_array_new_full (0, g_free);
	g_ptr_array_add (permitted, NULL);

	realm_kerberos_set_permitted_logins (kerberos, (const gchar **)permitted->pdata);
	g_ptr_array_free (permitted, TRUE);

	g_object_thaw_notify (G_OBJECT (self));
}

static void
on_config_changed (RealmIniConfig *config,
                   gpointer user_data)
{
	update_properties (REALM_SAMBA (user_data));
}

static gboolean
realm_samba_membership_generic_finish (RealmKerberosMembership *realm,
                                       GAsyncResult *result,
                                       GError **error)
{
	if (!g_task_propagate_boolean (G_TASK (result), error))
		return FALSE;

	update_properties (REALM_SAMBA (realm));
	return TRUE;
}

static gboolean
realm_samba_generic_finish (RealmKerberos *realm,
                            GAsyncResult *result,
                            GError **error)
{
	if (!g_task_propagate_boolean (G_TASK (result), error))
		return FALSE;

	update_properties (REALM_SAMBA (realm));
	return TRUE;
}

static void
realm_samba_set_property (GObject *obj,
                          guint prop_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
	RealmSamba *self = REALM_SAMBA (obj);
	RealmProvider *provider;

	switch (prop_id) {
	case PROP_PROVIDER:
		provider = g_value_get_object (value);
		g_object_get (provider, "samba-config", &self->config, NULL);
		self->config_sig = g_signal_connect (self->config, "changed",
		                                     G_CALLBACK (on_config_changed),
		                                     self);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_samba_notify (GObject *obj,
                    GParamSpec *spec)
{
	if (g_str_equal (spec->name, "name"))
		update_properties (REALM_SAMBA (obj));

	if (G_OBJECT_CLASS (realm_samba_parent_class)->notify)
		G_OBJECT_CLASS (realm_samba_parent_class)->notify (obj, spec);
}

static void
realm_samba_finalize (GObject *obj)
{
	RealmSamba  *self = REALM_SAMBA (obj);

	if (self->config)
		g_object_unref (self->config);

	G_OBJECT_CLASS (realm_samba_parent_class)->finalize (obj);
}

static void
realm_samba_discover_myself (RealmKerberos *realm,
                             RealmDisco *disco)
{
	RealmSamba *self = REALM_SAMBA (realm);
	gchar *value;

	value = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "workgroup");
	g_free (disco->workgroup);
	disco->workgroup = value;

	value = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "netbios name");
	g_free (disco->explicit_netbios);
	disco->explicit_netbios = value;

	value = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "password server");
	g_free (disco->explicit_server);
	disco->explicit_server = value;
}

void
realm_samba_class_init (RealmSambaClass *klass)
{
	RealmKerberosClass *kerberos_class = REALM_KERBEROS_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	kerberos_class->logins_async = realm_samba_logins_async;
	kerberos_class->logins_finish = realm_samba_generic_finish;
	kerberos_class->discover_myself = realm_samba_discover_myself;

	object_class->constructed = realm_samba_constructed;
	object_class->set_property = realm_samba_set_property;
	object_class->notify = realm_samba_notify;
	object_class->finalize = realm_samba_finalize;

	g_object_class_override_property (object_class, PROP_PROVIDER, "provider");
}

static void
realm_samba_kerberos_membership_iface (RealmKerberosMembershipIface *iface)
{
	iface->join_async = realm_samba_join_async;
	iface->join_finish = realm_samba_membership_generic_finish;
	iface->join_creds = realm_samba_join_creds;

	iface->leave_async = realm_samba_leave_async;
	iface->leave_finish = realm_samba_membership_generic_finish;
	iface->leave_creds = realm_samba_leave_creds;
}

RealmKerberos *
realm_samba_new (const gchar *name,
                 RealmProvider *provider)
{
	return g_object_new (REALM_TYPE_SAMBA,
	                     "name", name,
	                     "provider", provider,
	                     NULL);
}
