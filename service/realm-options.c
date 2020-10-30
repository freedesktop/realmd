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

#include "realm-dbus-constants.h"
#include "realm-options.h"
#include "realm-settings.h"

#include <string.h>

gboolean
realm_options_automatic_install (void)
{
	return realm_settings_boolean ("service", "automatic-install", TRUE);
}

gboolean
realm_options_manage_system (GVariant *options,
                             const gchar *realm_name)
{
	gboolean manage;
	gchar *section;

	section = g_utf8_casefold (realm_name, -1);
	if (realm_settings_value (section, REALM_DBUS_OPTION_MANAGE_SYSTEM))
		manage = realm_settings_boolean (section, REALM_DBUS_OPTION_MANAGE_SYSTEM, TRUE);
	else if (!g_variant_lookup (options, REALM_DBUS_OPTION_MANAGE_SYSTEM, "b", &manage))
		manage = TRUE;
	g_free (section);

	return manage;
}

const gchar *
realm_options_user_principal (GVariant *options,
                              const gchar *realm_name)
{
	const gchar *principal;
	gchar *section;

	if (!g_variant_lookup (options, REALM_DBUS_OPTION_USER_PRINCIPAL, "&s", &principal))
		principal = NULL;

	if (!principal) {
		section = g_utf8_casefold (realm_name, -1);
		if (realm_settings_boolean (section, REALM_DBUS_OPTION_USER_PRINCIPAL, FALSE))
			principal = ""; /* auto-generate */
		g_free (section);
	}

	return principal;
}

const gchar *
realm_options_computer_ou (GVariant *options,
                           const gchar *realm_name)
{
	const gchar *computer_ou = NULL;
	gchar *section;

	if (options) {
		if (!g_variant_lookup (options, REALM_DBUS_OPTION_COMPUTER_OU, "&s", &computer_ou))
			computer_ou = NULL;
	}

	if (realm_name && !computer_ou) {
		section = g_utf8_casefold (realm_name, -1);
		computer_ou = realm_settings_value (section, REALM_DBUS_OPTION_COMPUTER_OU);
		g_free (section);
	}

	return g_strdup (computer_ou);
}

gboolean
realm_options_automatic_mapping (GVariant *options,
                                 const gchar *realm_name)
{
	gboolean mapping = FALSE;
	gboolean option = FALSE;
	gchar *section;

	if (options) {
		option = g_variant_lookup (options, REALM_DBUS_OPTION_AUTOMATIC_ID_MAPPING, "b", &mapping);
	}

	if (realm_name && !option) {
		section = g_utf8_casefold (realm_name, -1);
		mapping = realm_settings_boolean (section, REALM_DBUS_OPTION_AUTOMATIC_ID_MAPPING, TRUE);
		g_free (section);
	}

	return mapping;
}

gboolean
realm_options_automatic_join (const gchar *realm_name)
{
	gchar *section;
	gboolean mapping;

	section = g_utf8_casefold (realm_name, -1);
	mapping = realm_settings_boolean (section, "automatic-join", FALSE);
	g_free (section);

	return mapping;
}

gboolean
realm_options_qualify_names (const gchar *realm_name,
                             gboolean def)
{
	gchar *section;
	gboolean qualify;

	section = g_utf8_casefold (realm_name, -1);
	qualify = realm_settings_boolean (section, "fully-qualified-names", def);
	g_free (section);

	return qualify;
}

gboolean
realm_options_check_domain_name (const gchar *name)
{
	/*
	 * DNS Domain names are pretty liberal (internet domain names
	 * are more restrictive) See RFC 2181 section 11
	 *
	 * http://www.ietf.org/rfc/rfc2181.txt
	 *
	 * However we cannot consume names with whitespace and problematic
	 * punctuation, due to the various programs that parse the
	 * configuration files we set up.
	 */

	gsize i, len;
	static const gchar *invalid = "=[]:";

	g_return_val_if_fail (name != NULL, FALSE);

	for (i = 0, len = strlen (name); i < len; i++) {
		if (name[i] <= ' ')
			return FALSE;
		if (strchr (invalid, name[i]))
			return FALSE;
	}

	return TRUE;
}

const gchar *
realm_options_computer_name (GVariant *options,
                           const gchar *realm_name)
{
	const gchar *computer_name = NULL;
	gchar *section;

	if (options) {
		if (!g_variant_lookup (options, REALM_DBUS_OPTION_COMPUTER_NAME, "&s", &computer_name))
			computer_name = NULL;
	}

	if (realm_name && !computer_name) {
		section = g_utf8_casefold (realm_name, -1);
		computer_name = realm_settings_value (section, REALM_DBUS_OPTION_COMPUTER_NAME);
		g_free (section);
	}

	return g_strdup (computer_name);
}

const gchar *
realm_options_ad_specific (GVariant *options,
                           const gchar *option_name)
{
	const gchar *value = NULL;

	if (options) {
		if (!g_variant_lookup (options, option_name, "&s", &value))
			value = NULL;
	}

	if (!value) {
		value = realm_settings_value ("active-directory", option_name);
	}

	return g_strdup (value);
}

gboolean realm_option_use_ldaps (GVariant *options)
{
	gchar *use_ldaps_str;

	use_ldaps_str = realm_options_ad_specific (options,
	                                          REALM_DBUS_OPTION_USE_LDAPS);
	if (use_ldaps_str != NULL
	            && ( g_ascii_strcasecmp (use_ldaps_str, "True") == 0
	                || g_ascii_strcasecmp (use_ldaps_str, "Yes") == 0)) {
		g_free (use_ldaps_str);
		return TRUE;
	}
	g_free (use_ldaps_str);

	return FALSE;
}
