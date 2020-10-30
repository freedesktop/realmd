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

#ifndef __REALM_LDAP_H__
#define __REALM_LDAP_H__

#include <gio/gio.h>

#include <ldap.h>

#define       REALM_IO_CANCELLED               1 << 10

#define       REALM_LDAP_ERROR                 (realm_ldap_error_get_quark ())

GQuark        realm_ldap_error_get_quark       (void) G_GNUC_CONST;

void          realm_ldap_set_error             (GError **error,
                                                LDAP *ldap,
                                                int code);

typedef GIOCondition (* RealmLdapCallback)     (LDAP *ldap,
                                                GIOCondition cond,
                                                gpointer data);

GSource *     realm_ldap_connect_anonymous     (GSocketAddress *address,
                                                GSocketProtocol protocol,
                                                gboolean use_ldaps,
                                                GCancellable *cancellable);

void          realm_ldap_set_condition         (GSource *source,
                                                GIOCondition cond);

#endif /* __REALM_LDAP_H__ */
