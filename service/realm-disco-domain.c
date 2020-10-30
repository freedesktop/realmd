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

#include "realm-command.h"
#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-disco.h"
#include "realm-disco-dns.h"
#include "realm-disco-domain.h"
#include "realm-disco-mscldap.h"
#include "realm-disco-rootdse.h"
#include "realm-errors.h"
#include "realm-invocation.h"
#include "realm-network.h"

#include <glib/gi18n.h>

typedef struct _Callback {
	GAsyncReadyCallback function;
	gpointer user_data;
	struct _Callback *next;
} Callback;

typedef struct {
	GObject parent;
	gchar *input;
	gboolean use_ldaps;
	GCancellable *cancellable;
	GDBusMethodInvocation *invocation;
	GSocketAddressEnumerator *enumerator;
	gint outstanding;
	gboolean completed;
	RealmDisco *disco;
	Callback *callback;
} RealmDiscoDomain;

typedef struct {
	GObjectClass parent;
} RealmDiscoDomainClass;

#define REALM_TYPE_DISCO_DOMAIN      (realm_disco_domain_get_type ())
#define REALM_DISCO_DOMAIN(inst)     (G_TYPE_CHECK_INSTANCE_CAST ((inst), REALM_TYPE_DISCO_DOMAIN, RealmDiscoDomain))
#define REALM_IS_DISCO_DOMAIN(inst)  (G_TYPE_CHECK_INSTANCE_TYPE ((inst), REALM_TYPE_DISCO_DOMAIN))

static GHashTable *discover_cache = NULL;

static void  step_discover  (RealmDiscoDomain *self,
                             RealmDisco *disco);

GType realm_disco_domain_get_type (void) G_GNUC_CONST;

void  realm_disco_domain_async_result_init (GAsyncResultIface *iface);

G_DEFINE_TYPE_WITH_CODE (RealmDiscoDomain, realm_disco_domain, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_RESULT, realm_disco_domain_async_result_init);
);

static void
realm_disco_domain_init (RealmDiscoDomain *self)
{
	self->cancellable = g_cancellable_new ();
}

static void
realm_disco_domain_finalize (GObject *obj)
{
	RealmDiscoDomain *self = REALM_DISCO_DOMAIN (obj);

	g_free (self->input);
	g_object_unref (self->cancellable);
	g_object_unref (self->invocation);
	g_clear_object (&self->enumerator);
	realm_disco_unref (self->disco);

	g_assert (self->callback == NULL);
	G_OBJECT_CLASS (realm_disco_domain_parent_class)->finalize (obj);
}

static void
realm_disco_domain_class_init (RealmDiscoDomainClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = realm_disco_domain_finalize;
}

static GObject *
realm_disco_domain_get_source_object (GAsyncResult *result)
{
	return g_object_ref (result);
}

static gpointer
realm_disco_domain_get_user_data (GAsyncResult *result)
{
	/* What is this useful for? */
	g_return_val_if_reached (NULL);
}

void
realm_disco_domain_async_result_init (GAsyncResultIface *iface)
{
	iface->get_source_object = realm_disco_domain_get_source_object;
	iface->get_user_data = realm_disco_domain_get_user_data;
}

static void
complete_discover (RealmDiscoDomain *self)
{
	Callback *call, *next;

	g_assert (!self->completed);
	self->completed = TRUE;

	/* No longer in the concurrency cache */
	g_hash_table_remove (discover_cache, self->input);
	if (g_hash_table_size (discover_cache) == 0)
		g_hash_table_destroy (discover_cache);
	discover_cache = NULL;

	/* Stop all other results */
	g_cancellable_cancel (self->cancellable);

	call = self->callback;
	self->callback = NULL;

	if (self->disco)
		realm_diagnostics_info (self->invocation, "Successfully discovered: %s", self->disco->domain_name);

	while (call != NULL) {
		next = call->next;
		if (call->function)
			(call->function) (NULL, G_ASYNC_RESULT (self), call->user_data);
		g_free (call);
		call = next;
	}
}

static void
on_discover_rootdse (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
	RealmDiscoDomain *self = REALM_DISCO_DOMAIN (user_data);
	GError *error = NULL;
	RealmDisco *disco;

	self->outstanding--;
	disco = realm_disco_rootdse_finish (result, &error);

	if (error && !self->completed)
		realm_diagnostics_error (self->invocation, error, NULL);
	g_clear_error (&error);
	step_discover (self, disco);

	g_object_unref (self);
}

static void
on_discover_next_address (GObject *source,
                          GAsyncResult *result,
                          gpointer user_data)
{
	RealmDiscoDomain *self = REALM_DISCO_DOMAIN (user_data);
	GSocketAddressEnumerator *enumerator = G_SOCKET_ADDRESS_ENUMERATOR (source);
	GError *error = NULL;
	GSocketAddress *address;
	GInetSocketAddress *inet;
	const gchar *explicit_host;
	RealmDiscoDnsHint hint;
	gchar *string;

	if (self->completed) {
		g_object_unref (self);
		return;
	}

	address = g_socket_address_enumerator_next_finish (enumerator, result, &error);
	if (error != NULL || address == NULL) {
		if (error && !self->completed && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			realm_diagnostics_error (self->invocation, error, "Couldn't lookup realm DNS records");
		g_clear_error (&error);
		g_clear_object (&self->enumerator);

	} else {
		inet = G_INET_SOCKET_ADDRESS (address);
		string = g_inet_address_to_string (g_inet_socket_address_get_address (inet));

		hint = realm_disco_dns_get_hint (enumerator);
		if (hint & REALM_DISCO_IS_SERVER)
			explicit_host = realm_disco_dns_get_name (enumerator);
		else
			explicit_host = NULL;

		realm_diagnostics_info (self->invocation, "Performing LDAP DSE lookup on: %s", string);
		realm_disco_rootdse_async (address, explicit_host,
		                           self->use_ldaps,
		                           self->invocation, self->cancellable,
		                           on_discover_rootdse, g_object_ref (self));
		self->outstanding++;
		g_free (string);
	}

	step_discover (self, NULL);
	g_clear_object (&address);
	g_object_unref (self);
}

static void
step_discover (RealmDiscoDomain *self,
               RealmDisco *disco)
{
	/* Already done, just skip these results */
	if (self->completed) {
		realm_disco_unref (disco);

	/* Either have a result, or finished searching: done */
	} else if (disco || (self->enumerator == NULL && self->outstanding == 0)) {
		self->disco = disco;
		complete_discover (self);

	/* Otherwise try up to three servers at once */
	} else if (self->enumerator && self->outstanding < 3) {
		g_socket_address_enumerator_next_async (self->enumerator,
		                                        self->cancellable,
		                                        on_discover_next_address,
		                                        g_object_ref (self));
	}
}

static void
on_cancel_propagate (GCancellable *source,
                     gpointer dest)
{
	g_cancellable_cancel (dest);
}

void
realm_disco_domain_async (const gchar *string,
                          gboolean use_ldaps,
                          GDBusMethodInvocation *invocation,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	RealmDiscoDomain *self;
	GCancellable *cancellable;
	Callback *call;

	g_return_if_fail (string != NULL);
	g_return_if_fail (invocation == NULL || G_IS_DBUS_METHOD_INVOCATION (invocation));

	if (!discover_cache)
		discover_cache = g_hash_table_new (g_str_hash, g_str_equal);

	self = g_hash_table_lookup (discover_cache, string);

	if (self == NULL) {
		self = g_object_new (REALM_TYPE_DISCO_DOMAIN, NULL);
		self->input = g_strdup (string);
		self->use_ldaps = use_ldaps;
		self->invocation = g_object_ref (invocation);
		self->enumerator = realm_disco_dns_enumerate_servers (string,
		                                                      use_ldaps,
		                                                      invocation);

		g_hash_table_insert (discover_cache, self->input, self);
		g_assert (!self->completed);

		cancellable = realm_invocation_get_cancellable (invocation);
		if (cancellable) {
			g_cancellable_connect (cancellable, (GCallback)on_cancel_propagate,
			                       g_object_ref (self->cancellable), g_object_unref);
		}

		step_discover (self, NULL);

	} else {
		g_assert (!self->completed);
		g_object_ref (self);
	}

	call = g_new0 (Callback, 1);
	call->function = callback;
	call->user_data = user_data;
	call->next = self->callback;
	self->callback = call;

	g_object_unref (self);
}

RealmDisco *
realm_disco_domain_finish (GAsyncResult *result,
                           GError **error)
{
	RealmDiscoDomain *self;

	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	self = REALM_DISCO_DOMAIN (result);

	/* Didn't find a valid domain */
	if (!self->disco)
		return NULL;

	return realm_disco_ref (self->disco);
}
