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

#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-disco-mscldap.h"
#include "realm-disco-rootdse.h"
#include "realm-ldap.h"
#include "realm-options.h"

#include <glib/gi18n.h>

#include <resolv.h>

typedef struct _Closure Closure;

struct _Closure {
	RealmDisco *disco;
	GSource *source;
	GDBusMethodInvocation *invocation;

	gchar *default_naming_context;
	gint msgid;

	gboolean (* request) (GTask *task,
	                      Closure *clo,
	                      LDAP *ldap);

	gboolean (* result) (GTask *task,
	                     Closure *clo,
	                     LDAP *ldap,
	                     LDAPMessage *msg);
};

static void
closure_free (gpointer data)
{
	Closure *clo = data;

	ldap_memfree (clo->default_naming_context);

	g_source_destroy (clo->source);
	g_source_unref (clo->source);
	g_clear_object (&clo->invocation);
	realm_disco_unref (clo->disco);
	g_free (clo);
}

static gboolean
entry_has_attribute (LDAP *ldap,
                     LDAPMessage *entry,
                     const gchar *field,
                     const gchar *value)
{
	struct berval **bvs = NULL;
	gboolean has = FALSE;
	gsize len;
	int i;

	len = strlen (value);
	if (entry != NULL)
		bvs = ldap_get_values_len (ldap, entry, field);

	for (i = 0; bvs && bvs[i]; i++) {
		if (bvs[i]->bv_len == len &&
		    memcmp (bvs[i]->bv_val, value, len) == 0) {
			has = TRUE;
			break;
		}
	}

	ldap_value_free_len (bvs);

	return has;
}

static gchar *
entry_get_attribute (LDAP *ldap,
                     LDAPMessage *entry,
                     const gchar *field,
                     gboolean domain_name)
{
	struct berval **bvs = NULL;
	gchar *value = NULL;

	if (entry != NULL)
		bvs = ldap_get_values_len (ldap, entry, field);

	if (bvs && bvs[0]) {
		value = g_strndup (bvs[0]->bv_val, bvs[0]->bv_len);
		if (domain_name) {
		       if (!realm_options_check_domain_name (value)) {
			       g_free (value);
			       g_message ("Invalid value in LDAP %s field", field);
			       value = NULL;
		       }
		}
	}

	ldap_value_free_len (bvs);

	return value;
}

static gboolean
search_ldap (GTask *task,
             Closure *clo,
             LDAP *ldap,
             const gchar *base,
             int scope,
             const char *filter,
             const char **attrs)
{
	GError *error = NULL;
	int rc;

	if (!filter)
		filter = "(objectClass=*)";

	g_debug ("Searching %s for %s", base, filter);
	rc = ldap_search_ext (ldap, base, scope, filter,
	                      (char **)attrs, 0, NULL, NULL, NULL, -1, &clo->msgid);

	if (rc != 0) {
		realm_ldap_set_error (&error, ldap, rc);
		g_task_return_error (task, error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
result_krb_realm (GTask *task,
                  Closure *clo,
                  LDAP *ldap,
                  LDAPMessage *message)
{
	LDAPMessage *entry;

	entry = ldap_first_entry (ldap, message);

	g_free (clo->disco->kerberos_realm);
	clo->disco->kerberos_realm = entry_get_attribute (ldap, entry, "cn", TRUE);

	g_debug ("Found realm: %s", clo->disco->kerberos_realm);

	/* All done */
	g_task_return_boolean (task, TRUE);
	return FALSE;
}

static gboolean
request_krb_realm (GTask *task,
                   Closure *clo,
                   LDAP *ldap)
{
	const char *attrs[] = { "cn", NULL };

	clo->request = NULL;
	clo->result = result_krb_realm;

	return search_ldap (task, clo, ldap, clo->default_naming_context,
	                    LDAP_SCOPE_SUB, "(objectClass=krbRealmContainer)", attrs);
}

static gboolean
result_domain_info (GTask *task,
                    Closure *clo,
                    LDAP *ldap,
                    LDAPMessage *message)
{
	LDAPMessage *entry;
	struct berval **bvs;

	entry = ldap_first_entry (ldap, message);

	/* If we can't retrieve this, then nothing more to do */
	if (entry == NULL) {
		g_debug ("Couldn't read default naming context");
		g_task_return_new_error (task, REALM_LDAP_ERROR, LDAP_NO_SUCH_OBJECT,
		                         "Couldn't lookup domain name on LDAP server");
		return FALSE;
	}

	/* What kind of server is it? */
	clo->disco->server_software = NULL;
	bvs = ldap_get_values_len (ldap, entry, "info");
	if (bvs && bvs[0] && bvs[0]->bv_len >= 3) {
		if (g_ascii_strncasecmp (bvs[0]->bv_val, "IPA", 3) == 0)
			clo->disco->server_software = REALM_DBUS_IDENTIFIER_IPA;
	}
	ldap_value_free_len (bvs);

	if (clo->disco->server_software)
		g_debug ("Got server software: %s", clo->disco->server_software);

	/* What is the domain name? */
	g_free (clo->disco->domain_name);
	clo->disco->domain_name = entry_get_attribute (ldap, entry, "associatedDomain", TRUE);

	g_debug ("Got associatedDomain: %s", clo->disco->domain_name);

	/* Next search for Kerberos container */
	clo->request = request_krb_realm;
	clo->result = NULL;
	return TRUE;
}

static gboolean
request_domain_info (GTask *task,
                     Closure *clo,
                     LDAP *ldap)
{
	const char *attrs[] = { "info", "associatedDomain", NULL };
	int ret;
	int ldap_opt_val;

	clo->request = NULL;
	clo->result = result_domain_info;

	/* Trying to setup a TLS tunnel in the case the IPA server requires an
	 * encrypted connected. Trying without in case of an error. Since we
	 * most probably do not have the IPA CA certificate we will not check
	 * the server certificate. */
	ldap_opt_val = LDAP_OPT_X_TLS_NEVER;
	ret = ldap_set_option (ldap, LDAP_OPT_X_TLS_REQUIRE_CERT, &ldap_opt_val);
	if (ret != LDAP_OPT_SUCCESS) {
		g_debug ("Failed to disable certificate checking, trying without");
	}

	ldap_opt_val = 0;
	ret = ldap_set_option (ldap, LDAP_OPT_X_TLS_NEWCTX, &ldap_opt_val);
	if (ret != LDAP_OPT_SUCCESS) {
		g_debug ("Failed to refresh LDAP context for TLS, trying without");
	}

	ret = ldap_start_tls_s (ldap, NULL, NULL);
	if (ret != LDAP_SUCCESS) {
		g_debug ("Failed to setup TLS tunnel, trying without");
	}

	return search_ldap (task, clo, ldap, clo->default_naming_context,
	                    LDAP_SCOPE_BASE, NULL, attrs);
}

static void
on_udp_mscldap_complete (GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	Closure *clo = g_task_get_task_data (task);
	GError *error = NULL;

	realm_disco_unref (clo->disco);
	clo->disco = realm_disco_mscldap_finish (result, &error);

	if (error != NULL) {
		g_debug ("Failed UDP Netlogon response: %s", error->message);
		g_task_return_error (task, error);
	} else {
		g_debug ("Received UDP Netlogon response");
		g_task_return_boolean (task, TRUE);
	}

	g_object_unref (task);
}

static gboolean
result_netlogon (GTask *task,
                 Closure *clo,
                 LDAP *ldap,
                 LDAPMessage *message)
{
	GError *error = NULL;

	if (realm_disco_mscldap_result (ldap, message, clo->disco, &error)) {
		g_debug ("Received TCP Netlogon response");
		g_task_return_boolean (task, TRUE);
	} else {
		g_debug ("Failed TCP Netlogon response: %s", error->message);
		g_task_return_error (task, error);
	}

	/* All done */
	return FALSE;
}

static gboolean
request_netlogon (GTask *task,
                  Closure *clo,
                  LDAP *ldap)
{
	GError *error = NULL;

	g_debug ("Sending TCP Netlogon request");

	if (!realm_disco_mscldap_request (ldap, &clo->msgid, &error)) {
		g_task_return_error (task, error);
		return FALSE;
	}

	clo->request = NULL;
	clo->result = result_netlogon;
	return TRUE;
}

static gboolean
result_root_dse (GTask *task,
                 Closure *clo,
                 LDAP *ldap,
                 LDAPMessage *message)
{
	GInetSocketAddress *inet;
	LDAPMessage *entry;
	gchar *string;

	entry = ldap_first_entry (ldap, message);

	/* Parse out the default naming context */
	clo->default_naming_context = entry_get_attribute (ldap, entry, "defaultNamingContext", FALSE);

	g_debug ("Got defaultNamingContext: %s", clo->default_naming_context);

	/* This means that this is an Active Directory server */
	if (entry_has_attribute (ldap, entry, "supportedCapabilities",
	                         "1.2.840.113556.1.4.800")) {

		/* This means that this is Windows 2003+ */
		if (entry_has_attribute (ldap, entry, "supportedCapabilities",
		                         "1.2.840.113556.1.4.1670")) {

			/*
			 * Do a TCP NetLogon request since doing this over
			 * TCP is supported, and we already have a connection
			 */
			clo->request = request_netlogon;
			clo->result = NULL;
			return TRUE;

		/* Prior to Windows 2003 we have to use UDP for netlogon lookup */
		} else {
			inet = G_INET_SOCKET_ADDRESS (clo->disco->server_address);
			string = g_inet_address_to_string (g_inet_socket_address_get_address (inet));
			realm_diagnostics_info (clo->invocation, "Sending MS-CLDAP ping to: %s", string);
			g_free (string);

			realm_disco_mscldap_async (clo->disco->server_address, G_SOCKET_PROTOCOL_UDP,
			                           clo->disco->explicit_server, g_task_get_cancellable (task),
			                           on_udp_mscldap_complete, g_object_ref (task));

			/* Disconnect from TCP at this point */
			return FALSE;
		}

	/* Not an Active Directory server, check for IPA */
	} else {

		if (clo->default_naming_context == NULL) {
			g_task_return_new_error (task, REALM_LDAP_ERROR, LDAP_NO_SUCH_OBJECT,
			                         "Couldn't find default naming context on LDAP server");
			return FALSE;
		}

		/* Next search for IPA field */
		clo->request = request_domain_info;
		clo->result = NULL;
		return TRUE;
	}
}

static gboolean
request_root_dse (GTask *task,
                  Closure *clo,
                  LDAP *ldap)
{
	const char *attrs[] = { "defaultNamingContext", "supportedCapabilities", NULL };

	clo->request = NULL;
	clo->result = result_root_dse;

	return search_ldap (task, clo, ldap, "", LDAP_SCOPE_BASE, NULL, attrs);
}

static GIOCondition
on_ldap_io (LDAP *ldap,
            GIOCondition cond,
            gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	Closure *clo = g_task_get_task_data (task);
	struct timeval tvpoll = { 0, 0 };
	LDAPMessage *message;
	GError *error = NULL;
	gboolean ret;

	/* Some failure */
	if (cond & G_IO_ERR) {
		realm_ldap_set_error (&error, ldap, 0);
		g_task_return_error (task, error);
		return G_IO_NVAL;
	}

	/* Ready to get a result */
	if (cond & G_IO_IN && clo->result != NULL) {
		switch (ldap_result (ldap, clo->msgid, 0, &tvpoll, &message)) {
		case LDAP_RES_INTERMEDIATE:
		case LDAP_RES_SEARCH_REFERENCE:
			ret = TRUE;
			break;
		case -1:
			realm_ldap_set_error (&error, ldap, -1);
			g_task_return_error (task, error);
			ret = FALSE;
			break;
		case 0:
			ret = TRUE;
			break;
		default:
			ret = clo->result (task, clo, ldap, message);
			ldap_msgfree (message);
			break;
		}

		if (!ret)
			return G_IO_NVAL;
	}

	if (cond & G_IO_OUT && clo->request != NULL) {
		if (!(clo->request) (task, clo, ldap))
			return G_IO_NVAL;
	}

	return (clo->request ? G_IO_OUT : 0) |
	       (clo->result ? G_IO_IN : 0);
}

void
realm_disco_rootdse_async (GSocketAddress *address,
                           const gchar *explicit_server,
                           gboolean use_ldaps,
                           GDBusMethodInvocation *invocation,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	GTask *task;
	Closure *clo;

	g_return_if_fail (address != NULL);

	task = g_task_new (NULL, cancellable, callback, user_data);
	clo = g_new0 (Closure, 1);
	clo->disco = realm_disco_new (NULL);
	clo->disco->explicit_server = g_strdup (explicit_server);
	clo->disco->server_address = g_object_ref (address);

	clo->invocation = invocation ? g_object_ref (invocation) : NULL;
	clo->request = request_root_dse;
	g_task_set_task_data (task, clo, closure_free);

	clo->source = realm_ldap_connect_anonymous (address, G_SOCKET_PROTOCOL_TCP,
	                                            use_ldaps, cancellable);
	if (clo->source == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		                         _("Failed to setup LDAP connection"));
		g_object_unref (task);
		return;
	}
	g_source_set_callback (clo->source, (GSourceFunc)on_ldap_io,
	                       g_object_ref (task), g_object_unref);
	g_source_attach (clo->source, g_task_get_context (task));

	g_object_unref (task);
}

RealmDisco *
realm_disco_rootdse_finish (GAsyncResult *result,
                            GError **error)
{
	Closure *clo;
	RealmDisco *disco;

	g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (!g_task_propagate_boolean (G_TASK (result), error))
		return FALSE;

	clo = g_task_get_task_data (G_TASK (result));
	disco = clo->disco;
	clo->disco = NULL;

	/* Should have been set above */
	g_return_val_if_fail (disco->domain_name, NULL);

	if (!disco->kerberos_realm)
		disco->kerberos_realm = g_ascii_strup (disco->domain_name, -1);

	return disco;
}
