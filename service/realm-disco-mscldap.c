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
#include "realm-disco-mscldap.h"
#include "realm-ldap.h"
#include "realm-options.h"

#include <glib/gi18n.h>

#include <errno.h>
#include <resolv.h>
#include <unistd.h>

typedef struct {
	gchar *explicit_server;
	GSocketAddress *address;
	GSource *source;
	gint count;
	gint fever_id;
	gint normal_id;
} Closure;

/* Number of rapid requets to do */
#define DISCO_FEVER  4

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

static void
closure_free (gpointer data)
{
	Closure *clo = data;

	g_free (clo->explicit_server);
	g_object_unref (clo->address);
	if (clo->fever_id)
		g_source_remove (clo->fever_id);
	if (clo->normal_id)
		g_source_remove (clo->normal_id);
	g_source_destroy (clo->source);
	g_source_unref (clo->source);
	g_free (clo);
}

static gchar *
explicit_netbios_name (void)
{
	gchar hostname[HOST_NAME_MAX + 1];
	gchar *dot;

	/*
	 * Only return a explicit truncated host name if the
	 * computer host name cannot be made seamlessly translated
	 * to a netbios name due to it's length.
	 *
	 * We would love to leave this responsibility to our lower level
	 * tools, but unfortunately samba doesn't know how to do this
	 * properly, and expects us to set it properly in smb.conf
	 *
	 * In addition sssd falls over if truncation is done. So we have
	 * to tell sssd about it.
	 */

	if (gethostname (hostname, sizeof (hostname)) < 0) {
		g_warning ("Couldn't get the computer host name: %s", g_strerror (errno));
		return NULL;
	}

	dot = strchr (hostname, '.');
	if (dot != NULL)
		dot[0] = '\0';

	if (strlen (hostname) > 15) {
		hostname[15] = '\0';
		return g_ascii_strup (hostname, -1);
	}

	return NULL;
}

static gchar *
get_string (guchar *beg,
            guchar *end,
            guchar **at)
{
	gchar buffer[HOST_NAME_MAX];
	int n;

	n = dn_expand (beg, end, *at, buffer, sizeof (buffer));
	if (n < 0)
		return NULL;

	if (!realm_options_check_domain_name (buffer)) {
		g_message ("received invalid NetLogon string characters");
		return NULL;
	}

	(*at) += n;
	return g_strdup (buffer);
}

static gboolean
parse_string (guchar *beg,
              guchar *end,
              guchar **at,
              gchar **result)
{
	gchar *string;

	g_assert (result);

	string = get_string (beg, end, at);
	if (string == NULL)
		return FALSE;

	g_free (*result);
	*result = string;

	return TRUE;
}

static gboolean
get_32_le (unsigned char **at,
           unsigned char *end,
           unsigned int *val)
{
	unsigned char *p = *at;
	if (p + 4 > end)
		return FALSE;
	*val = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
	(*at) += 4;
	return TRUE;
}

static gboolean
skip_n (unsigned char **at,
        unsigned char *end,
        int n)
{
	if ((*at) + n > end)
		return FALSE;
	(*at) += n;
	return TRUE;
}

static gboolean
parse_netlogon (struct berval **bvs,
                RealmDisco *disco,
                GError **error)
{
	guchar *at, *end, *beg;
	gchar *unused = NULL;
	guint type, flags;
	gboolean success = FALSE;

	if (bvs != NULL && bvs[0] != NULL) {
		beg = (guchar *)bvs[0]->bv_val;
		end = beg + bvs[0]->bv_len;
		at = beg;
		success = TRUE;
	}

	/* domain forest */
	if (!success ||
	    !get_32_le (&at, end, &type) || type != 23 ||
	    !get_32_le (&at, end, &flags) ||
	    !skip_n (&at, end, 16) || /* guid */
	    !parse_string (beg, end, &at, &unused) || /* forest */
	    !parse_string (beg, end, &at, &disco->domain_name) ||
	    !parse_string (beg, end, &at, &unused) || /* hostname */
	    !parse_string (beg, end, &at, &disco->workgroup) ||
	    !parse_string (beg, end, &at, &unused) || /* shorthost */
	    !parse_string (beg, end, &at, &unused) || /* user */
	    !parse_string (beg, end, &at, &unused) || /* server site */
	    !parse_string (beg, end, &at, &unused)) { /* client site */
		success = FALSE;
	}

	g_free (unused);

	if (!success) {
		g_set_error (error, REALM_LDAP_ERROR, LDAP_PROTOCOL_ERROR,
		             _("Received invalid or unsupported Netlogon data from server"));
		return FALSE;
	}

	disco->server_software = REALM_DBUS_IDENTIFIER_ACTIVE_DIRECTORY;
	disco->explicit_netbios = explicit_netbios_name ();
	disco->kerberos_realm = g_ascii_strup (disco->domain_name, -1);
	return TRUE;
}

gboolean
realm_disco_mscldap_result (LDAP *ldap,
                            LDAPMessage *message,
                            RealmDisco *disco,
                            GError **error)
{
	struct berval **bvs = NULL;
	LDAPMessage *entry;
	gboolean ret;

	entry = ldap_first_entry (ldap, message);
	if (entry != NULL)
		bvs = ldap_get_values_len (ldap, entry, "NetLogon");
	ret = parse_netlogon (bvs, disco, error);
	ldap_value_free_len (bvs);

	return ret;
}

gboolean
realm_disco_mscldap_request (LDAP *ldap,
                             int *msgidp,
                             GError **error)
{
	char *attrs[] = { "NetLogon", NULL };
	int rc;

	rc = ldap_search_ext (ldap, "", LDAP_SCOPE_BASE,
	                      "(&(NtVer=\\06\\00\\00\\00)(AAC=\\00\\00\\00\\00))",
	                      attrs, 0, NULL, NULL, NULL,
	                      -1, msgidp);

	if (rc != LDAP_SUCCESS) {
		realm_ldap_set_error (error, ldap, rc);
		return FALSE;
	}

	return TRUE;
}

static gboolean
on_resend (gpointer user_data)
{
	realm_ldap_set_condition (user_data, G_IO_OUT | G_IO_IN);
	return TRUE;
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
	RealmDisco *disco;
	int msgid;

	/* Cancelled */
	if (cond & G_IO_ERR) {
		realm_ldap_set_error (&error, ldap, 0);
		g_task_return_error (task, error);
		return G_IO_NVAL;
	}

	/* Ready for input */
	if (cond & G_IO_OUT) {
		g_debug ("Sending NetLogon ping");
		if (!realm_disco_mscldap_request (ldap, &msgid, &error)) {
			g_task_return_error (task, error);
			return G_IO_NVAL;
		}

		/* Remove rapid fire after sending a feverish batch */
		if (clo->count++ > DISCO_FEVER && clo->fever_id != 0) {
			g_source_remove (clo->fever_id);
			clo->fever_id = 0;
		}
	}

	/* Ready to get a result */
	if (cond & G_IO_IN) {
		switch (ldap_result (ldap, LDAP_RES_ANY, 0, &tvpoll, &message)) {
		case LDAP_RES_SEARCH_ENTRY:
		case LDAP_RES_SEARCH_RESULT:
			g_debug ("Received response");
			disco = realm_disco_new (NULL);
			disco->server_address = g_object_ref (clo->address);
			if (realm_disco_mscldap_result (ldap, message, disco, &error)) {
				disco->explicit_server = g_strdup (clo->explicit_server);
				g_task_return_pointer (task, disco, realm_disco_unref);
			} else {
				realm_disco_unref (disco);
				g_task_return_error (task, error);
			}
			ldap_msgfree (message);
			return G_IO_NVAL;
		case -1:
			realm_ldap_set_error (&error, ldap, -1);
			g_task_return_error (task, error);
			return G_IO_NVAL;
		case 0:
			break;
		default:
			/* Ignore and keep waiting */
			ldap_msgfree (message);
			break;
		}
	}

	/* Now wait for a response */
	return G_IO_IN;
}

void
realm_disco_mscldap_async (GSocketAddress *address,
                           GSocketProtocol protocol,
                           const gchar *explicit_server,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	GTask *task;
	Closure *clo;

	g_return_if_fail (address != NULL);

	task = g_task_new (NULL, cancellable, callback, user_data);
	clo = g_new0 (Closure, 1);
	clo->explicit_server = g_strdup (explicit_server);
	clo->address = g_object_ref (address);
	g_task_set_task_data (task, clo, closure_free);

	if (protocol == G_SOCKET_PROTOCOL_UDP &&
	    !ldap_is_ldap_url ("cldap://hostname")) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                         _("LDAP on this system does not support UDP connections"));
		g_object_unref (task);
		return;
	}

	clo->source = realm_ldap_connect_anonymous (address, protocol, FALSE, cancellable);
	if (clo->source == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		                         _("Failed to setup LDAP connection"));
		g_object_unref (task);
		return;
	}

	g_source_set_callback (clo->source, (GSourceFunc)on_ldap_io,
	                       g_object_ref (task), g_object_unref);
	g_source_attach (clo->source, g_task_get_context (task));

	if (protocol == G_SOCKET_PROTOCOL_UDP) {
		clo->fever_id = g_timeout_add (100, on_resend, clo->source);
		clo->normal_id = g_timeout_add (1000, on_resend, clo->source);
	}

	g_object_unref (task);
}

RealmDisco *
realm_disco_mscldap_finish (GAsyncResult *result,
                            GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	return g_task_propagate_pointer (G_TASK (result), error);
}
