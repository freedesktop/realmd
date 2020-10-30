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

#ifndef __REALM_DISCO_DOMAIN_H__
#define __REALM_DICSO_DOMAIN_H__

#include "realm-disco.h"

#include <gio/gio.h>

G_BEGIN_DECLS

void          realm_disco_domain_async    (const gchar *string,
                                           gboolean use_ldaps,
                                           GDBusMethodInvocation *invocation,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

RealmDisco *  realm_disco_domain_finish   (GAsyncResult *result,
                                           GError **error);

G_END_DECLS

#endif /* __REALM_DISCO_DOMAIN_H__ */
