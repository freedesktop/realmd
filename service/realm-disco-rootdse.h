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

#ifndef __REALM_DISCO_ROOTDSE_H__
#define __REALM_DISCO_ROOTDSE_H__

#include <gio/gio.h>

#include "realm-disco.h"

void           realm_disco_rootdse_async    (GSocketAddress *address,
                                             const gchar *explicit_server,
                                             gboolean use_ldaps,
                                             GDBusMethodInvocation *invocation,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

RealmDisco *   realm_disco_rootdse_finish   (GAsyncResult *result,
                                             GError **error);

#endif /* __REALM_DISCO_ROOTDSE_H__ */
