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

#ifndef __REALM_ADCLI_ENROLL_H__
#define __REALM_ADCLI_ENROLL_H__

#include "realm-credential.h"
#include "realm-disco.h"

#include <gio/gio.h>

#include <krb5.h>

G_BEGIN_DECLS

void         realm_adcli_enroll_join_async    (RealmDisco *disco,
                                               RealmCredential *cred,
                                               GVariant *options,
                                               gboolean use_ldaps,
                                               GDBusMethodInvocation *invocation,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

gboolean     realm_adcli_enroll_join_finish   (GAsyncResult *result,
                                               GError **error);

void         realm_adcli_enroll_delete_async  (RealmDisco *disco,
                                               RealmCredential *cred,
                                               GVariant *options,
                                               gboolean use_ldaps,
                                               GDBusMethodInvocation *invocation,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

gboolean     realm_adcli_enroll_delete_finish (GAsyncResult *result,
                                               GError **error);

G_END_DECLS

#endif /* __REALM_ADCLI_ENROLL_H__ */
