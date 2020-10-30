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

#ifndef __REALM_DBUS_CONSTANTS_H__
#define __REALM_DBUS_CONSTANTS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define   REALM_DBUS_BUS_NAME                      "org.freedesktop.realmd"
#define   REALM_DBUS_SERVICE_PATH                  "/org/freedesktop/realmd"

#define   DBUS_PEER_INTERFACE                      "org.freedesktop.DBus.Peer"
#define   DBUS_PROPERTIES_INTERFACE                "org.freedesktop.DBus.Properties"
#define   DBUS_INTROSPECTABLE_INTERFACE            "org.freedesktop.DBus.Introspectable"

#define   REALM_DBUS_PROVIDER_INTERFACE            "org.freedesktop.realmd.Provider"
#define   REALM_DBUS_REALM_INTERFACE               "org.freedesktop.realmd.Realm"
#define   REALM_DBUS_KERBEROS_INTERFACE            "org.freedesktop.realmd.Kerberos"
#define   REALM_DBUS_KERBEROS_MEMBERSHIP_INTERFACE "org.freedesktop.realmd.KerberosMembership"
#define   REALM_DBUS_SERVICE_INTERFACE             "org.freedesktop.realmd.Service"

#define   REALM_DBUS_DIAGNOSTICS_SIGNAL            "Diagnostics"

#define   REALM_DBUS_ERROR_INTERNAL                "org.freedesktop.realmd.Error.Internal"
#define   REALM_DBUS_ERROR_FAILED                  "org.freedesktop.realmd.Error.Failed"
#define   REALM_DBUS_ERROR_BUSY                    "org.freedesktop.realmd.Error.Busy"
#define   REALM_DBUS_ERROR_NOT_AUTHORIZED          "org.freedesktop.realmd.Error.NotAuthorized"
#define   REALM_DBUS_ERROR_CANCELLED               "org.freedesktop.realmd.Error.Cancelled"
#define   REALM_DBUS_ERROR_ALREADY_CONFIGURED      "org.freedesktop.realmd.Error.AlreadyConfigured"
#define   REALM_DBUS_ERROR_NOT_CONFIGURED          "org.freedesktop.realmd.Error.NotConfigured"
#define   REALM_DBUS_ERROR_AUTH_FAILED             "org.freedesktop.realmd.Error.AuthenticationFailed"
#define   REALM_DBUS_ERROR_BAD_HOSTNAME            "org.freedesktop.realmd.Error.BadHostname"
#define   REALM_DBUS_ERROR_CANCELLED               "org.freedesktop.realmd.Error.Cancelled"

#define   REALM_DBUS_DISCOVERY_DOMAIN              "domain"
#define   REALM_DBUS_DISCOVERY_KDCS                "kerberos-kdcs"
#define   REALM_DBUS_DISCOVERY_REALM               "kerberos-realm"

#define   REALM_DBUS_NAME_CHARS                    "abcdefghijklnmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

#define   REALM_DBUS_LOGIN_POLICY_ANY              "allow-any-login"
#define   REALM_DBUS_LOGIN_POLICY_REALM            "allow-realm-logins"
#define   REALM_DBUS_LOGIN_POLICY_PERMITTED        "allow-permitted-logins"
#define   REALM_DBUS_LOGIN_POLICY_DENY             "deny-any-login"

#define   REALM_DBUS_OPTION_OPERATION              "operation"
#define   REALM_DBUS_OPTION_COMPUTER_OU            "computer-ou"
#define   REALM_DBUS_OPTION_AUTOMATIC_ID_MAPPING   "automatic-id-mapping"
#define   REALM_DBUS_OPTION_SERVER_SOFTWARE        "server-software"
#define   REALM_DBUS_OPTION_CLIENT_SOFTWARE        "client-software"
#define   REALM_DBUS_OPTION_MEMBERSHIP_SOFTWARE    "membership-software"
#define   REALM_DBUS_OPTION_USER_PRINCIPAL         "user-principal"
#define   REALM_DBUS_OPTION_MANAGE_SYSTEM          "manage-system"
#define   REALM_DBUS_OPTION_COMPUTER_NAME          "computer-name"
#define   REALM_DBUS_OPTION_OS_NAME                "os-name"
#define   REALM_DBUS_OPTION_OS_VERSION             "os-version"
#define   REALM_DBUS_OPTION_LEGACY_SMB_CONF        "legacy-samba-config"
#define   REALM_DBUS_OPTION_USE_LDAPS              "use-ldaps"

#define   REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY   "active-directory"
#define   REALM_DBUS_IDENTIFIER_WINBIND            "winbind"
#define   REALM_DBUS_IDENTIFIER_IPA                "ipa"
#define   REALM_DBUS_IDENTIFIER_FREEIPA            "freeipa"
#define   REALM_DBUS_IDENTIFIER_SSSD               "sssd"
#define   REALM_DBUS_IDENTIFIER_SAMBA              "samba"
#define   REALM_DBUS_IDENTIFIER_ADCLI              "adcli"
#define   REALM_DBUS_IDENTIFIER_EXAMPLE            "example"

G_END_DECLS

#endif /* __REALM_DBUS_CONSTANTS_H__ */
