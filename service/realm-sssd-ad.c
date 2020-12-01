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
#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-errors.h"
#include "realm-kerberos-config.h"
#include "realm-kerberos-membership.h"
#include "realm-options.h"
#include "realm-packages.h"
#include "realm-samba-enroll.h"
#include "realm-service.h"
#include "realm-settings.h"
#include "realm-sssd.h"
#include "realm-sssd-ad.h"
#include "realm-sssd-config.h"

#include <glib/gstdio.h>
#include <glib/gi18n.h>

#include <errno.h>
#include <string.h>

struct _RealmSssdAd {
	RealmSssd parent;
};

typedef struct {
	RealmSssdClass parent_class;
} RealmSssdAdClass;

static const gchar *ADCLI_PACKAGES[] = {
	REALM_DBUS_IDENTIFIER_SSSD,
	REALM_DBUS_IDENTIFIER_ADCLI,
	NULL
};

static const gchar *SAMBA_PACKAGES[] = {
	REALM_DBUS_IDENTIFIER_SSSD,
	REALM_DBUS_IDENTIFIER_SAMBA,
	NULL
};

static const gchar *ALL_PACKAGES[] = {
	REALM_DBUS_IDENTIFIER_SSSD,
	REALM_DBUS_IDENTIFIER_ADCLI,
	REALM_DBUS_IDENTIFIER_SAMBA,
	NULL
};

static void realm_sssd_ad_kerberos_membership_iface (RealmKerberosMembershipIface *iface);

G_DEFINE_TYPE_WITH_CODE (RealmSssdAd, realm_sssd_ad, REALM_TYPE_SSSD,
                         G_IMPLEMENT_INTERFACE (REALM_TYPE_KERBEROS_MEMBERSHIP, realm_sssd_ad_kerberos_membership_iface);
);

static void
realm_sssd_ad_init (RealmSssdAd *self)
{

}

static void
realm_sssd_ad_constructed (GObject *obj)
{
	RealmKerberos *kerberos = REALM_KERBEROS (obj);

	G_OBJECT_CLASS (realm_sssd_ad_parent_class)->constructed (obj);

	realm_kerberos_set_details (kerberos,
	                            REALM_DBUS_OPTION_SERVER_SOFTWARE, REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY,
	                            REALM_DBUS_OPTION_CLIENT_SOFTWARE, REALM_DBUS_IDENTIFIER_SSSD,
	                            NULL);

	realm_kerberos_set_suggested_admin (kerberos, "Administrator");
	realm_kerberos_set_login_policy (kerberos, REALM_KERBEROS_ALLOW_REALM_LOGINS);
	realm_kerberos_set_required_package_sets (kerberos, ALL_PACKAGES);
}

typedef struct {
	GDBusMethodInvocation *invocation;
	RealmCredential *cred;
	GVariant *options;
	RealmDisco *disco;
	gboolean use_adcli;
	gboolean use_ldaps;
	const gchar **packages;
} JoinClosure;

static void
join_closure_free (gpointer data)
{
	JoinClosure *join = data;
	realm_disco_unref (join->disco);
	g_object_unref (join->invocation);
	realm_credential_unref (join->cred);
	g_variant_ref (join->options);
	g_free (join);
}

static void
on_enable_nss_done (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	GError *error = NULL;
	gint status;

	status = realm_command_run_finish (result, NULL, &error);
	if (error == NULL && status != 0)
		g_set_error (&error, REALM_ERROR, REALM_ERROR_INTERNAL,
		             _("Enabling SSSD in nsswitch.conf and PAM failed."));
	if (error != NULL)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
	g_object_unref (task);
}

static void
on_sssd_enable_nss (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	JoinClosure *join = g_task_get_task_data (task);
	GError *error = NULL;

	realm_service_enable_and_restart_finish (result, &error);

	if (error == NULL) {
		realm_command_run_known_async ("sssd-enable-logins", NULL, join->invocation,
		                               on_enable_nss_done, g_object_ref (task));

	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static gboolean
configure_sssd_for_domain (RealmIniConfig *config,
                           RealmDisco *disco,
                           GVariant *options,
                           gboolean use_adcli,
                           GError **error)
{
	const gchar *services[] = { "nss", "pam", NULL };
	GString *realmd_tags;
	const gchar *access_provider;
	const gchar *shell;
    const gchar *explicit_computer_name;
	gchar *authid = NULL;
	gboolean qualify;
	gboolean ret;
	gchar *section;
	gchar *home;

	home = realm_sssd_build_default_home (realm_settings_string ("users", "default-home"));
	qualify = realm_options_qualify_names (disco->domain_name, TRUE);
	shell = realm_settings_string ("users", "default-shell");
	explicit_computer_name = realm_options_computer_name (options, disco->domain_name);
	realmd_tags = g_string_new ("");
	if (realm_options_manage_system (options, disco->domain_name))
		g_string_append (realmd_tags, "manages-system ");
	g_string_append (realmd_tags, use_adcli ? "joined-with-adcli " : "joined-with-samba ");

	/*
	 * Explicitly set the netbios authid for sssd to use in these cases, since
	 * otherwise sssd won't know which kerberos principal to use
	 */
	if (explicit_computer_name != NULL)
		authid = g_strdup_printf ("%s$", explicit_computer_name);
	else if (disco->explicit_netbios)
		authid = g_strdup_printf ("%s$", disco->explicit_netbios);

	ret = realm_sssd_config_add_domain (config, disco->domain_name, error,
	                                    "cache_credentials", "True",
		                            "use_fully_qualified_names", qualify ? "True" : "False",

	                                    "id_provider", "ad",

	                                    "ad_domain", disco->domain_name,
	                                    "krb5_realm", disco->kerberos_realm,
	                                    "krb5_store_password_if_offline", "True",
	                                    "ldap_id_mapping", realm_options_automatic_mapping (options, disco->domain_name) ? "True" : "False",
	                                    "realmd_tags", realmd_tags->str,

	                                    "fallback_homedir", home,
	                                    "default_shell", shell,
	                                    "ad_server", disco->explicit_server,
	                                    "ldap_sasl_authid", authid,
	                                    NULL);

	if (ret)
		ret = realm_ini_config_change_list (config, "sssd", "services", ", ", services, NULL, error);

	g_free (authid);
	g_string_free (realmd_tags, TRUE);

	if (ret) {
		if (realm_options_manage_system (options, disco->domain_name))
			access_provider = "ad";
		else
			access_provider = "simple";
		section = realm_sssd_config_domain_to_section (disco->domain_name);
		ret = realm_sssd_set_login_policy (config, section, access_provider, NULL, NULL, FALSE, error);
		free (section);
	}

	g_free (home);

	return ret;
}

static void
on_join_do_sssd (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	JoinClosure *join = g_task_get_task_data (task);
	RealmSssd *sssd = g_task_get_source_object (task);
	GError *error = NULL;

	if (join->use_adcli) {
		if (!realm_adcli_enroll_join_finish (result, &error)) {
			if (join->cred->type == REALM_CREDENTIAL_AUTOMATIC &&
			    g_error_matches (error, REALM_ERROR, REALM_ERROR_AUTH_FAILED)) {
				g_clear_error (&error);
				g_set_error (&error, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
				             _("Unable to automatically join the domain"));
			}
		}
	} else {
		realm_samba_enroll_join_finish (result, &error);
	}

	if (error == NULL) {
		configure_sssd_for_domain (realm_sssd_get_config (sssd), join->disco,
		                           join->options, join->use_adcli, &error);
	}

	if (error == NULL) {
		configure_krb5_conf_for_domain (join->disco->kerberos_realm, &error);
		if (error != NULL) {
			realm_diagnostics_error (join->invocation, error,
			                         "Failed to update Kerberos "
			                         "configuration, not fatal, "
			                         "please check manually");
			g_clear_error (&error);
		}
	}

	if (error == NULL) {
		realm_service_enable_and_restart ("sssd", join->invocation,
		                                  on_sssd_enable_nss, g_object_ref (task));

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
	JoinClosure *join = g_task_get_task_data (task);
	GError *error = NULL;

	realm_packages_install_finish (result, &error);
	if (error == NULL) {
		if (join->use_adcli) {
			realm_adcli_enroll_join_async (join->disco,
			                               join->cred,
			                               join->options,
			                               join->use_ldaps,
			                               join->invocation,
			                               on_join_do_sssd,
			                               g_object_ref (task));
		} else {
			realm_samba_enroll_join_async (join->disco,
			                               join->cred,
			                               join->options,
			                               join->invocation, on_join_do_sssd,
			                               g_object_ref (task));
		}

	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static gboolean
parse_join_options (JoinClosure *join,
                    RealmCredential *cred,
                    GVariant *options,
                    GError **error)
{
	const gchar *software;

	/* Figure out the method that we're going to use to enroll */
	if (g_variant_lookup (options, REALM_DBUS_OPTION_MEMBERSHIP_SOFTWARE, "&s", &software)) {
		if (!g_str_equal (software, REALM_DBUS_IDENTIFIER_ADCLI) &&
		    !g_str_equal (software, REALM_DBUS_IDENTIFIER_SAMBA)) {
			g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			             _("Unsupported or unknown membership software '%s'"), software);
			return FALSE;
		}
	} else {
		software = NULL;
	}

	/*
	 * If we are enrolling with a one time password, or automatically, use
	 * adcli. Samba doesn't support computer passwords or using reset accounts.
	 */
	if ((cred->type == REALM_CREDENTIAL_SECRET && cred->owner == REALM_CREDENTIAL_OWNER_NONE) ||
	    (cred->type == REALM_CREDENTIAL_AUTOMATIC && cred->owner == REALM_CREDENTIAL_OWNER_NONE)) {
		if (!software)
			software = REALM_DBUS_IDENTIFIER_ADCLI;
		if (!g_str_equal (software, REALM_DBUS_IDENTIFIER_ADCLI)) {
			g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			             _("Joining a domain with a one time password is only supported with the '%s' membership software"),
			             REALM_DBUS_IDENTIFIER_ADCLI);
			return FALSE;
		}

	/*
	 * If we are enrolling with a user password, then we have to use samba,
	 * adcli only supports admin passwords.
	 */
	} else if (cred->type == REALM_CREDENTIAL_PASSWORD && cred->owner == REALM_CREDENTIAL_OWNER_USER) {
		if (!software)
			software = REALM_DBUS_IDENTIFIER_SAMBA;
		if (!g_str_equal (software, REALM_DBUS_IDENTIFIER_SAMBA)) {
			g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			             _("Joining a domain with a user password is only supported with the '%s' membership software"),
			             REALM_DBUS_IDENTIFIER_SAMBA);
			return FALSE;
		}

	/*
	 * For other valid types of credentials we prefer adcli.
	 */
	} else if (cred->type == REALM_CREDENTIAL_CCACHE ||
	           (cred->type == REALM_CREDENTIAL_PASSWORD && cred->owner == REALM_CREDENTIAL_OWNER_ADMIN)) {
		if (!software)
			software = REALM_DBUS_IDENTIFIER_ADCLI;

	/* It would be odd to get here */
	} else {
		g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		             _("Unsupported credentials for joining a domain"));
		return FALSE;
	}

	g_assert (software != NULL);

	/*
	 * Check if ldaps should be used and if membership software supports
	 * it.
	 */
	join->use_ldaps = realm_option_use_ldaps (options);
	if (join->use_ldaps &&
	           g_str_equal (software, REALM_DBUS_IDENTIFIER_SAMBA)) {
		realm_diagnostics_info (join->invocation,
		                        "Membership software %s does "
		                        "not support ldaps, trying "
		                        "without.", software);
	}

	if (g_str_equal (software, REALM_DBUS_IDENTIFIER_ADCLI)) {
		join->use_adcli = TRUE;
		join->packages = ADCLI_PACKAGES;
	} else {
		join->use_adcli = FALSE;
		join->packages = SAMBA_PACKAGES;
	}

	return TRUE;
}

static void
realm_sssd_ad_join_async (RealmKerberosMembership *membership,
                          RealmCredential *cred,
                          GVariant *options,
                          GDBusMethodInvocation *invocation,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	RealmKerberos *realm = REALM_KERBEROS (membership);
	RealmSssd *sssd = REALM_SSSD (realm);
	GTask *task;
	JoinClosure *join;
	GError *error = NULL;

	task = g_task_new (realm, NULL, callback, user_data);
	join = g_new0 (JoinClosure, 1);
	join->disco = realm_disco_ref (realm_kerberos_get_disco (realm));
	join->invocation = g_object_ref (invocation);
	join->options = g_variant_ref (options);
	join->cred = realm_credential_ref (cred);
	g_task_set_task_data (task, join, join_closure_free);

	/* Make sure not already enrolled in a realm */
	if (realm_sssd_get_config_section (sssd) != NULL) {
		g_task_return_new_error (task, REALM_ERROR, REALM_ERROR_ALREADY_CONFIGURED,
		                         _("Already joined to this domain"));

	} else if (realm_sssd_config_have_domain (realm_sssd_get_config (sssd), realm_kerberos_get_realm_name (realm))) {
		g_task_return_new_error (task, REALM_ERROR, REALM_ERROR_ALREADY_CONFIGURED,
		                         _("A domain with this name is already configured"));

	} else if (!parse_join_options (join, cred, options, &error)) {
		g_task_return_error (task, error);

	/* Prepared successfully without an error */
	} else {
		realm_packages_install_async (join->packages, join->invocation,
		                              g_dbus_method_invocation_get_connection (join->invocation),
		                              on_install_do_join, g_object_ref (task));
	}

	g_object_unref (task);
}

static const RealmCredential *
realm_sssd_ad_join_creds (RealmKerberosMembership *membership)
{
	/*
	 * Each line is a combination of owner and what kind of credentials are supported,
	 * same for enroll/leave. We can't accept a ccache with samba because of certain
	 * corner cases. However we do accept ccache for an admin user, and then we use
	 * adcli with that ccache.
	 */

	static const RealmCredential creds[] = {
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_USER, },
		{ REALM_CREDENTIAL_CCACHE, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_AUTOMATIC, REALM_CREDENTIAL_OWNER_NONE, },
		{ REALM_CREDENTIAL_SECRET, REALM_CREDENTIAL_OWNER_NONE, },
		{ 0, },
	};

	static const RealmCredential creds_no_auto[] = {
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_USER, },
		{ REALM_CREDENTIAL_CCACHE, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_SECRET, REALM_CREDENTIAL_OWNER_NONE, },
		{ 0, }
	};

	const gchar *name;

	name = realm_kerberos_get_name (REALM_KERBEROS (membership));
	return realm_options_automatic_join (name) ? creds : creds_no_auto;
}

typedef struct {
	GDBusMethodInvocation *invocation;
	gchar *realm_name;
	gboolean use_adcli;
} LeaveClosure;

static void
leave_closure_free (gpointer data)
{
	LeaveClosure *leave = data;
	g_free (leave->realm_name);
	g_object_unref (leave->invocation);
	g_free (leave);
}

static void
on_leave_do_deconfigure (GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	LeaveClosure *leave = g_task_get_task_data (task);
	RealmSssd *sssd = g_task_get_source_object (task);
	GError *error = NULL;

	/* We don't care if we can leave or not, just continue with other steps */
	if (leave->use_adcli)
		realm_adcli_enroll_delete_finish (result, &error);
	else
		realm_samba_enroll_leave_finish (result, &error);

	if (error != NULL) {
		realm_diagnostics_error (leave->invocation, error, NULL);
		g_error_free (error);
	}

	realm_sssd_deconfigure_domain_tail (sssd, task, leave->invocation);

	g_object_unref (task);
}

static void
realm_sssd_ad_leave_async (RealmKerberosMembership *membership,
                           RealmCredential *cred,
                           GVariant *options,
                           GDBusMethodInvocation *invocation,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	RealmSssdAd *self = REALM_SSSD_AD (membership);
	RealmKerberos *realm = REALM_KERBEROS (self);
	RealmSssd *sssd = REALM_SSSD (self);
	RealmDisco *disco;
	const gchar *section;
	GTask *task;
	LeaveClosure *leave;
	gchar *tags;
	gboolean use_ldaps = FALSE;

	task = g_task_new (self, NULL, callback, user_data);

	/* Check that enrolled in this realm */
	section = realm_sssd_get_config_section (sssd);
	if (!section) {
		g_task_return_new_error (task, REALM_ERROR, REALM_ERROR_NOT_CONFIGURED,
		                         _("Not currently joined to this domain"));
		g_object_unref (task);
		return;
	}

	tags = realm_ini_config_get (realm_sssd_get_config (sssd), section, "realmd_tags");

	/* This also has the side-effect of populating the disco info if necessary */
	disco = realm_kerberos_get_disco (realm);

	switch (cred->type) {
	case REALM_CREDENTIAL_AUTOMATIC:
		realm_sssd_deconfigure_domain_tail (REALM_SSSD (self), task, invocation);
		break;
	case REALM_CREDENTIAL_CCACHE:
	case REALM_CREDENTIAL_PASSWORD:
		leave = g_new0 (LeaveClosure, 1);
		leave->realm_name = g_strdup (realm_kerberos_get_realm_name (realm));
		leave->invocation = g_object_ref (invocation);
		leave->use_adcli = strstr (tags ? tags : "", "joined-with-adcli") ? TRUE : FALSE;
		g_task_set_task_data (task, leave, leave_closure_free);

		use_ldaps = realm_option_use_ldaps (options);
		if (leave->use_adcli) {
			realm_adcli_enroll_delete_async (disco, cred, options,
			                                 use_ldaps,  invocation,
			                                 on_leave_do_deconfigure, g_object_ref (task));
		} else {
			if (use_ldaps) {
				realm_diagnostics_info (leave->invocation,
				                        "Membership software does "
				                        "not support ldaps, trying "
				                        "without.");
			}
			realm_samba_enroll_leave_async (disco, cred, options, invocation,
			                                on_leave_do_deconfigure, g_object_ref (task));
		}
		break;
	default:
		g_return_if_reached ();
	}

	g_free (tags);
	g_object_unref (task);
}

static const RealmCredential *
realm_sssd_ad_leave_creds (RealmKerberosMembership *membership)
{
	/* For leave, we don't support one-time-password (ie: secret/none) */
	static const RealmCredential creds[] = {
		{ REALM_CREDENTIAL_PASSWORD, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_CCACHE, REALM_CREDENTIAL_OWNER_ADMIN, },
		{ REALM_CREDENTIAL_AUTOMATIC, REALM_CREDENTIAL_OWNER_NONE, },
		{ 0, },
	};

	return creds;
}

static gboolean
realm_sssd_ad_generic_finish (RealmKerberosMembership *realm,
                              GAsyncResult *result,
                              GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
realm_sssd_ad_discover_myself (RealmKerberos *realm,
                               RealmDisco *disco)
{
	RealmSssd *sssd = REALM_SSSD (realm);
	RealmIniConfig *config;
	const gchar *section;
	gchar *dollar;
	gchar *value;

	config = realm_sssd_get_config (sssd);
	section = realm_sssd_get_config_section (sssd);

	if (section == NULL)
		return;

	value = realm_ini_config_get (config, section, "ad_server");
	g_free (disco->explicit_server);
	disco->explicit_server = value;

	/*
	 * If this field has an authid that looks like a samAccountName
	 * (ie: Netbios name with a $ suffix) then it looks like an explicit
	 * netbios server name has been set.
	 */
	value = realm_ini_config_get (config, section, "ldap_sasl_authid");
	if (value) {
		dollar = strrchr (value, '$');
		if (dollar && dollar[1] == '\0') {
			dollar[0] = '\0';

		} else {
			g_free (value);
			value = NULL;
		}
	}

	g_free (disco->explicit_netbios);
	disco->explicit_netbios = value;
}

void
realm_sssd_ad_class_init (RealmSssdAdClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RealmKerberosClass *kerberos_class = REALM_KERBEROS_CLASS (klass);
	RealmSssdClass *sssd_class = REALM_SSSD_CLASS (klass);

	object_class->constructed = realm_sssd_ad_constructed;

	/* The provider in sssd.conf relevant to this realm type */
	sssd_class->sssd_conf_provider_name = "ad";

	kerberos_class->discover_myself = realm_sssd_ad_discover_myself;
}

static void
realm_sssd_ad_kerberos_membership_iface (RealmKerberosMembershipIface *iface)
{
	iface->join_async = realm_sssd_ad_join_async;
	iface->join_finish = realm_sssd_ad_generic_finish;
	iface->join_creds = realm_sssd_ad_join_creds;

	iface->leave_async = realm_sssd_ad_leave_async;
	iface->leave_finish = realm_sssd_ad_generic_finish;
	iface->leave_creds = realm_sssd_ad_leave_creds;
}
