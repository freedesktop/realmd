/* realmd -- Realm configuration service
 *
 * Copyright 2020 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Sumit Bose <sbose@redhat.com>
 */

#include "config.h"

#include "realm-ini-config.h"
#include "realm-kerberos-config.h"
#include "realm-settings.h"

#include <string.h>

RealmIniConfig *
realm_kerberos_config_new_with_flags (RealmIniFlags flags,
                                      GError **error)
{
	RealmIniConfig *config;
	const gchar *filename;
	GError *err = NULL;

	config = realm_ini_config_new (REALM_INI_LINE_CONTINUATIONS | flags);

	filename = realm_settings_path ("krb5.conf");

	realm_ini_config_read_file (config, filename, &err);

	if (err != NULL) {
		/* If the caller wants errors, then don't return an invalid samba config */
		if (error) {
			g_propagate_error (error, err);
			g_object_unref (config);
			config = NULL;

		/* If the caller doesn't care, then warn but continue */
		} else {
			g_warning ("Couldn't load config file: %s: %s", filename,
			           err->message);
			g_error_free (err);
		}
	}

	return config;
}

RealmIniConfig *
realm_kerberos_config_new (GError **error)
{
	return realm_kerberos_config_new_with_flags (REALM_INI_NONE, error);
}

gboolean
configure_krb5_conf_for_domain (const gchar *realm, GError **error )
{
	RealmIniConfig *config;
	gboolean res;
	GFile *gfile;
	GFileInfo *file_info = NULL;

	config = realm_kerberos_config_new (error);
	if (config == NULL) {
		return FALSE;
	}

	/* When writing to a file glib will replace the original file with a
	 * new one. To make sure permissions and other attributes like e.g.
	 * SELinux labels stay the same this information is saved before the
	 * change and applied to the new file afterwards. */
	gfile = g_file_new_for_path (realm_ini_config_get_filename (config));
	file_info = g_file_query_info (gfile, "*", 0, NULL, error);
	g_object_unref (gfile);
	if (*error != NULL) {
		g_warning ("Couldn't load file attributes, "
		           "will continue without: %s: %s",
		           realm_ini_config_get_filename (config),
		           (*error)->message);
		g_clear_error (error);
	}

	if (!realm_ini_config_begin_change (config, error)) {
		g_object_unref (config);
		return FALSE;
	}

	realm_ini_config_set (config, "libdefaults",
	                              "default_realm", realm,
	                              "udp_preference_limit", "0",
	                               NULL);

	res = realm_ini_config_finish_change (config, error);

	if (file_info != NULL) {
		gfile = g_file_new_for_path (realm_ini_config_get_filename (config));
		if (!g_file_set_attributes_from_info (gfile, file_info,
		                                      0, NULL, error)) {
			g_warning ("Couldn't set file attributes: %s: %s",
			           realm_ini_config_get_filename (config),
			           (*error)->message);
		}
		g_object_unref (file_info);
		g_object_unref (gfile);
	}

	g_object_unref (config);

	return res;
}
