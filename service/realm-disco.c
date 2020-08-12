/* realmd -- Realm configuration service
 *
 * Copyright 2013 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "realm-disco.h"

GType
realm_disco_get_type (void)
{
	static GType type = 0;

	if (type == 0)
		type = g_boxed_type_register_static ("RealmDisco",
		                                     (GBoxedCopyFunc)realm_disco_ref,
		                                     realm_disco_unref);

	return type;
}

RealmDisco *
realm_disco_new (const gchar *domain)
{
	RealmDisco *disco;

	disco = g_new0 (RealmDisco, 1);
	disco->refs = 1;
	disco->domain_name = g_strdup (domain);
	return disco;
}

RealmDisco *
realm_disco_ref (RealmDisco *disco)
{
	g_return_val_if_fail (disco != NULL, NULL);
	disco->refs++;
	return disco;
}

void
realm_disco_unref (gpointer data)
{
	RealmDisco *disco = data;

	if (!data)
		return;

	if (disco->refs-- == 1) {
		g_free (disco->domain_name);
		g_free (disco->explicit_server);
		g_free (disco->explicit_netbios);
		g_free (disco->kerberos_realm);
		g_free (disco->workgroup);
		g_free (disco->dns_fqdn);
		if (disco->server_address)
			g_object_unref (disco->server_address);
		g_free (disco);
	}
}
