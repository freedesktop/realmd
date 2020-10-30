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
#include "realm-disco-domain.h"
#include "realm-errors.h"
#include "realm-kerberos.h"
#include "realm-packages.h"
#include "realm-sssd-ad.h"
#include "realm-sssd-ipa.h"
#include "realm-sssd-provider.h"
#include "realm-sssd-config.h"
#include "realm-options.h"

#include <glib/gstdio.h>

#include <errno.h>

struct _RealmSssdProvider {
	RealmProvider parent;
	RealmIniConfig *config;
};

typedef struct {
	RealmProviderClass parent_class;
} RealmSssdProviderClass;

enum {
	PROP_0,
	PROP_SSSD_CONFIG,
};

#define   REALM_DBUS_SSSD_PATH                    "/org/freedesktop/realmd/Sssd"

G_DEFINE_TYPE (RealmSssdProvider, realm_sssd_provider, REALM_TYPE_PROVIDER);

static void
realm_sssd_provider_init (RealmSssdProvider *self)
{
	self->config = realm_sssd_config_new (NULL);
}

static void
realm_sssd_provider_constructed (GObject *obj)
{
	RealmSssdProvider *self;
	GType realm_type;
	gchar **domains;
	gchar *type;
	gchar *name;
	gint i;

	G_OBJECT_CLASS (realm_sssd_provider_parent_class)->constructed (obj);

	self = REALM_SSSD_PROVIDER (obj);

	realm_provider_set_name (REALM_PROVIDER (self), "Sssd");

	domains = realm_sssd_config_get_domains (self->config);
	for (i = 0; domains && domains[i] != 0; i++) {
		if (realm_sssd_config_load_domain (self->config, domains[i], NULL, &type, &name)) {
			if (g_strcmp0 (type, "ad") == 0)
				realm_type = REALM_TYPE_SSSD_AD;
			else if (g_strcmp0 (type, "ipa") == 0)
				realm_type = REALM_TYPE_SSSD_IPA;
			else
				realm_type = 0;

			if (realm_type)
				realm_provider_lookup_or_register_realm (REALM_PROVIDER (self), realm_type, name, NULL);

			g_free (name);
			g_free (type);
		}
	}
	g_strfreev (domains);
}

static void
on_kerberos_discover (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	RealmDisco *disco;
	GError *error = NULL;

	disco = realm_disco_domain_finish (result, &error);
	if (error)
		g_task_return_error (task, error);
	else
		g_task_return_pointer (task, disco, realm_disco_unref);
	g_object_unref (task);
}

static void
realm_sssd_provider_discover_async (RealmProvider *provider,
                                    const gchar *string,
                                    GVariant *options,
                                    GDBusMethodInvocation *invocation,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	GTask *task;

	task = g_task_new (provider, NULL, callback, user_data);
	g_task_set_task_data (task, g_variant_ref (options), (GDestroyNotify)g_variant_unref);

	if (!realm_provider_match_software (options,
	                                    REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY,
	                                    REALM_DBUS_IDENTIFIER_SSSD,
	                                    REALM_DBUS_IDENTIFIER_SAMBA) &&
	    !realm_provider_match_software (options,
	                                    REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY,
	                                    REALM_DBUS_IDENTIFIER_SSSD,
	                                    REALM_DBUS_IDENTIFIER_ADCLI) &&
	    !realm_provider_match_software (options,
	                                    REALM_DBUS_IDENTIFIER_FREEIPA,
	                                    REALM_DBUS_IDENTIFIER_SSSD,
	                                    REALM_DBUS_IDENTIFIER_FREEIPA) &&
	    !realm_provider_match_software (options,
	                                    REALM_DBUS_IDENTIFIER_IPA,
	                                    REALM_DBUS_IDENTIFIER_SSSD,
	                                    REALM_DBUS_IDENTIFIER_IPA)) {
		g_task_return_pointer (task, NULL, NULL);

	} else {
		realm_disco_domain_async (string,
		                          realm_option_use_ldaps (options),
		                          invocation, on_kerberos_discover,
		                          g_object_ref (task));
	}

	g_object_unref (task);
}

static GList *
realm_sssd_provider_discover_finish (RealmProvider *provider,
                                     GAsyncResult *result,
                                     gint *relevance,
                                     GError **error)
{
	RealmKerberos *realm = NULL;
	RealmDisco *disco;
	GVariant *options;
	gint priority;

	disco = g_task_propagate_pointer (G_TASK (result), error);
	if (disco == NULL)
		return NULL;

	options = g_task_get_task_data (G_TASK (result));

	if (disco->server_software == NULL ||
	    !realm_provider_match_software (options, disco->server_software,
	                                    REALM_DBUS_IDENTIFIER_SSSD, NULL)) {
		realm = NULL;

	} else if (g_str_equal (disco->server_software, REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY)) {
		realm = realm_provider_lookup_or_register_realm (provider,
		                                                 REALM_TYPE_SSSD_AD,
		                                                 disco->domain_name, disco);
		priority = realm_provider_is_default (REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY, REALM_DBUS_IDENTIFIER_SSSD) ? 100 : 50;

	} else if (g_str_equal (disco->server_software, REALM_DBUS_IDENTIFIER_IPA)) {
		realm = realm_provider_lookup_or_register_realm (provider,
		                                                 REALM_TYPE_SSSD_IPA,
		                                                 disco->domain_name, disco);
		priority = 100;
	}

	realm_disco_unref (disco);

	if (realm == NULL)
		return NULL;

	/* Return a higher priority if we're the default */
	*relevance = priority;
	return g_list_append (NULL, g_object_ref (realm));
}

static void
realm_sssd_provider_get_property (GObject *obj,
                                  guint prop_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
	RealmSssdProvider *self = REALM_SSSD_PROVIDER (obj);

	switch (prop_id) {
	case PROP_SSSD_CONFIG:
		g_value_set_object (value, self->config);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_sssd_provider_finalize (GObject *obj)
{
	RealmSssdProvider *self = REALM_SSSD_PROVIDER (obj);

	g_object_unref (self->config);

	G_OBJECT_CLASS (realm_sssd_provider_parent_class)->finalize (obj);
}

void
realm_sssd_provider_class_init (RealmSssdProviderClass *klass)
{
	RealmProviderClass *provider_class = REALM_PROVIDER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	provider_class->discover_async = realm_sssd_provider_discover_async;
	provider_class->discover_finish = realm_sssd_provider_discover_finish;

	object_class->constructed = realm_sssd_provider_constructed;
	object_class->get_property = realm_sssd_provider_get_property;
	object_class->finalize = realm_sssd_provider_finalize;

	g_object_class_install_property (object_class, PROP_SSSD_CONFIG,
	            g_param_spec_object ("sssd-config", "Sssd Config", "Sssd Config",
	                                 REALM_TYPE_INI_CONFIG, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

}

RealmProvider *
realm_sssd_provider_new (void)
{
	return g_object_new (REALM_TYPE_SSSD_PROVIDER,
	                     "g-object-path", REALM_DBUS_SSSD_PATH,
	                     NULL);
}
