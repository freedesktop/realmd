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

#ifndef __REALM_DISCO_H__
#define __REALM_DISCO_H__

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
	gint refs;
	const gchar *server_software;
	gchar *domain_name;
	gchar *kerberos_realm;
	gchar *workgroup;
	gchar *explicit_server;
	gchar *explicit_netbios;
	GSocketAddress *server_address;
	gchar *dns_fqdn;
} RealmDisco;

#define        REALM_TYPE_DISCO             (realm_disco_get_type ())

GType          realm_disco_get_type         (void) G_GNUC_CONST;

RealmDisco *   realm_disco_new              (const gchar *domain);

RealmDisco *   realm_disco_ref              (RealmDisco *disco);

void           realm_disco_unref            (gpointer disco);

G_END_DECLS

#endif /* __REALM_DISCO_H__ */
