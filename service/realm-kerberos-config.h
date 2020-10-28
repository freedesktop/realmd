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

#ifndef __REALM_KERBEROS_CONFIG_H__
#define __REALM_KERBEROS_CONFIG_H__

#include <gio/gio.h>

#include "realm-ini-config.h"


RealmIniConfig *    realm_kerberos_config_new                      (GError **error);

RealmIniConfig *    realm_kerberos_config_new_with_flags           (RealmIniFlags flags,
                                                                    GError **error);

gboolean            configure_krb5_conf_for_domain                 (const gchar *realm,
                                                                    GError **error );

G_END_DECLS

#endif /* __REALM_KERBEROS_CONFIG_H__ */
