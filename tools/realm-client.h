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

#ifndef __REALM_CLIENT_H__
#define __REALM_CLIENT_H__

#include <glib-object.h>

#include "realm-dbus-generated.h"

G_BEGIN_DECLS

#define REALM_TYPE_CLIENT            (realm_client_get_type ())
#define REALM_CLIENT(inst)           (G_TYPE_CHECK_INSTANCE_CAST ((inst), REALM_TYPE_CLIENT, RealmClient))
#define REALM_IS_CLIENT(inst)        (G_TYPE_CHECK_INSTANCE_TYPE ((inst), REALM_TYPE_CLIENT))
#define REALM_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), REALM_TYPE_CLIENT, RealmClientClass))
#define REALM_IS_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), REALM_TYPE_CLIENT))
#define REALM_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), REALM_TYPE_CLIENT, RealmClientClass))

typedef struct _RealmClient RealmClient;
typedef struct _RealmClientClass RealmClientClass;

GType                          realm_client_get_type                 (void) G_GNUC_CONST;

RealmClient *                  realm_client_new                      (gboolean verbose,
                                                                      const gchar *prefix);

RealmDbusProvider *            realm_client_get_provider             (RealmClient *self);

GList *                        realm_client_discover                 (RealmClient *self,
                                                                      const gchar *string,
                                                                      gboolean use_ldaps,
                                                                      const gchar *client_software,
                                                                      const gchar *server_software,
                                                                      const gchar *membership_software,
                                                                      const gchar *dbus_interface,
                                                                      gboolean *had_mismatched,
                                                                      GError **error);

RealmDbusRealm *               realm_client_get_realm                (RealmClient *self,
                                                                      const gchar *object_path);

RealmDbusRealm *               realm_client_to_realm                 (RealmClient *self,
                                                                      gpointer proxy);

RealmDbusKerberosMembership *  realm_client_get_kerberos_membership  (RealmClient *self,
                                                                      const gchar *object_path);

RealmDbusKerberosMembership *  realm_client_to_kerberos_membership   (RealmClient *self,
                                                                      gpointer proxy);

RealmDbusKerberos *            realm_client_get_kerberos             (RealmClient *self,
                                                                      const gchar *object_path);

RealmDbusKerberos *            realm_client_to_kerberos              (RealmClient *self,
                                                                      gpointer proxy);

GVariant *                     realm_client_build_principal_creds    (RealmClient *self,
                                                                      RealmDbusKerberosMembership *membership,
                                                                      GVariant *supported,
                                                                      const gchar *user_name,
                                                                      GError **error);

GVariant *                     realm_client_build_otp_creds          (RealmClient *self,
                                                                      GVariant *supported,
                                                                      const gchar *one_time_password,
                                                                      GError **error);

GVariant *                     realm_client_build_automatic_creds    (RealmClient *self,
                                                                      RealmDbusKerberos *realm,
                                                                      GVariant *supported,
                                                                      GError **error);

G_END_DECLS

#endif /* __REALM_H__ */
