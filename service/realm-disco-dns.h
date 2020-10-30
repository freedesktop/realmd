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

#ifndef __REALM_DISCO_DNS_H__
#define __REALM_DICSO_DNS_H__

#include <gio/gio.h>

typedef enum {
	REALM_DISCO_IS_SERVER = 1 << 3,
} RealmDiscoDnsHint;

G_BEGIN_DECLS

GSocketAddressEnumerator *  realm_disco_dns_enumerate_servers    (const gchar *domain_or_server,
                                                                  gboolean use_ldaps,
                                                                  GDBusMethodInvocation *invocation);

RealmDiscoDnsHint           realm_disco_dns_get_hint             (GSocketAddressEnumerator *enumerator);

const gchar *               realm_disco_dns_get_name             (GSocketAddressEnumerator *enumerator);

G_END_DECLS

#endif /* __REALM_DISCO_DNS_H__ */
