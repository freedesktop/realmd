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

#include "realm-diagnostics.h"
#include "realm-disco-dns.h"

#include <glib/gi18n.h>

typedef enum {
	PHASE_NONE,
	PHASE_SRV,
	PHASE_HOST,
	PHASE_DONE
} DiscoPhase;

typedef struct {
	GSocketAddressEnumerator parent;
	gchar *name;
	GQueue addresses;
	GQueue targets;
	gint current_port;
	gboolean use_ldaps;
	gint returned;
	DiscoPhase phase;
	GResolver *resolver;
	GDBusMethodInvocation *invocation;
} RealmDiscoDns;

typedef struct {
	GSocketAddressEnumeratorClass parent;
} RealmDiscoDnsClass;

#define REALM_TYPE_DISCO_DNS      (realm_disco_dns_get_type ())
#define REALM_DISCO_DNS(inst)     (G_TYPE_CHECK_INSTANCE_CAST ((inst), REALM_TYPE_DISCO_DNS, RealmDiscoDns))
#define REALM_IS_DISCO_DNS(inst)  (G_TYPE_CHECK_INSTANCE_TYPE ((inst), REALM_TYPE_DISCO_DNS))

static void return_or_resolve (RealmDiscoDns *self, GTask *task);

GType realm_disco_dns_get_type (void) G_GNUC_CONST;

G_DEFINE_TYPE (RealmDiscoDns, realm_disco_dns, G_TYPE_SOCKET_ADDRESS_ENUMERATOR);

static void
realm_disco_dns_init (RealmDiscoDns *self)
{
	g_queue_init (&self->addresses);
	g_queue_init (&self->targets);
}

static void
realm_disco_dns_finalize (GObject *obj)
{
	RealmDiscoDns *self = REALM_DISCO_DNS (obj);
	gpointer value;

	g_free (self->name);
	g_object_unref (self->invocation);
	g_clear_object (&self->resolver);

	for (;;) {
		value = g_queue_pop_head (&self->addresses);
		if (!value)
			break;
		g_object_unref (value);
	}

	for (;;) {
		value = g_queue_pop_head (&self->targets);
		if (!value)
			break;
		g_srv_target_free (value);
	}

	G_OBJECT_CLASS (realm_disco_dns_parent_class)->finalize (obj);
}

static GSocketAddress *
realm_disco_dns_next (GSocketAddressEnumerator *enumerator,
                      GCancellable *cancellable,
                      GError **error)
{
	/* We don't use this synchronously in realmd */
	g_return_val_if_reached (NULL);
}

static void
on_name_resolved (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	RealmDiscoDns *self = g_task_get_source_object (task);
	GError *error = NULL;
	GList *addrs;
	GList *l;

	addrs = g_resolver_lookup_by_name_finish (self->resolver, result, &error);

	if (error)
		g_debug ("%s", error->message);

	/* These are not real errors, just absence of addresses */
	if (g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND) ||
	    g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE))
		g_clear_error (&error);

	if (error) {
		g_task_return_error (task, error);

	} else {
		for (l = addrs; l != NULL; l = g_list_next (l))
			g_queue_push_head (&self->addresses, g_inet_socket_address_new (l->data, self->current_port));
		g_list_free_full (addrs, g_object_unref);
		return_or_resolve (self, task);
	}

	g_object_unref (task);
}


static void
on_service_resolved (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
	GTask *task = G_TASK (user_data);
	RealmDiscoDns *self = g_task_get_source_object (task);
	GError *error = NULL;
	GList *targets;
	GList *l;

	targets = g_resolver_lookup_service_finish (self->resolver, result, &error);

	if (error)
		g_debug ("%s", error->message);

	/* These are not real errors, just absence of addresses */
	if (g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND) ||
	    g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE))
		g_clear_error (&error);

	if (error) {
		g_task_return_error (task, error);

	} else {
		for (l = targets; l != NULL; l = g_list_next (l))
			g_queue_push_tail (&self->targets, l->data);
		g_list_free (targets);
		return_or_resolve (self, task);
	}

	g_object_unref (task);
}

static void
return_or_resolve (RealmDiscoDns *self,
                   GTask *task)
{
	GSocketAddress *address;
	GSrvTarget *target;

	address = g_queue_pop_head (&self->addresses);
	if (address) {
		self->returned++;
		g_task_return_pointer (task, address, g_object_unref);
		return;
	}

	target = g_queue_pop_head (&self->targets);
	if (target) {
		self->current_port = self->use_ldaps ? 636 : g_srv_target_get_port (target);
		g_resolver_lookup_by_name_async (self->resolver, g_srv_target_get_hostname (target),
		                                 g_task_get_cancellable (task), on_name_resolved,
		                                 g_object_ref (task));
		g_srv_target_free (target);
		return;
	}

	switch (self->returned > 0 ? PHASE_DONE : self->phase) {
	case PHASE_NONE:
		realm_diagnostics_info (self->invocation, "Resolving: _ldap._tcp.%s", self->name);
		g_resolver_lookup_service_async (self->resolver, "ldap", "tcp", self->name,
		                                 g_task_get_cancellable (task),
		                                 on_service_resolved, g_object_ref (task));
		self->phase = PHASE_SRV;
		break;
	case PHASE_SRV:
		realm_diagnostics_info (self->invocation, "Resolving: %s", self->name);
		g_resolver_lookup_by_name_async (self->resolver, self->name,
		                                 g_task_get_cancellable (task), on_name_resolved,
		                                 g_object_ref (task));
		self->current_port = self->use_ldaps ? 636 : 389;
		self->phase = PHASE_HOST;
		break;
	case PHASE_HOST:
		realm_diagnostics_info (self->invocation, "No results: %s", self->name);
		self->phase = PHASE_DONE;
		/* fall through */
	case PHASE_DONE:
		g_task_return_pointer (task, NULL, NULL);
		break;
	}
}

static void
realm_disco_dns_next_async (GSocketAddressEnumerator *enumerator,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	RealmDiscoDns *self = REALM_DISCO_DNS (enumerator);
	GTask *task;

	task = g_task_new (enumerator, cancellable, callback, user_data);
	return_or_resolve (self, task);
	g_object_unref (task);
}

static GSocketAddress *
realm_disco_dns_next_finish (GSocketAddressEnumerator *enumerator,
                             GAsyncResult *result,
                             GError **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

static void
realm_disco_dns_class_init (RealmDiscoDnsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GSocketAddressEnumeratorClass *enum_class = G_SOCKET_ADDRESS_ENUMERATOR_CLASS (klass);

	object_class->finalize = realm_disco_dns_finalize;

	enum_class->next = realm_disco_dns_next;
	enum_class->next_async = realm_disco_dns_next_async;
	enum_class->next_finish = realm_disco_dns_next_finish;
}

GSocketAddressEnumerator *
realm_disco_dns_enumerate_servers (const gchar *domain_or_server,
                                   gboolean use_ldaps,
                                   GDBusMethodInvocation *invocation)
{
	RealmDiscoDns *self;
	GInetAddress *inet;
	gchar *input;

	input = g_strdup (domain_or_server);
	g_strstrip (input);

	self = g_object_new (REALM_TYPE_DISCO_DNS, NULL);
	self->name = g_hostname_to_ascii (input);
	self->use_ldaps = use_ldaps;
	self->invocation = g_object_ref (invocation);

	/* If is an IP, skip resolution */
	if (g_hostname_is_ip_address (input)) {
		inet = g_inet_address_new_from_string (input);
		g_queue_push_head (&self->addresses,
		                   g_inet_socket_address_new (inet, use_ldaps ? 636 : 389));
		g_object_unref (inet);
		self->phase = PHASE_HOST;
	} else {
		self->resolver = g_resolver_get_default ();
	}

	g_free (input);
	return G_SOCKET_ADDRESS_ENUMERATOR (self);
}

RealmDiscoDnsHint
realm_disco_dns_get_hint (GSocketAddressEnumerator *enumerator)
{
	g_return_val_if_fail (REALM_IS_DISCO_DNS (enumerator), FALSE);
	switch (REALM_DISCO_DNS (enumerator)->phase) {
	case PHASE_HOST:
		return REALM_DISCO_IS_SERVER;
	default:
		return 0;
	}
}

const gchar *
realm_disco_dns_get_name (GSocketAddressEnumerator *enumerator)
{
	g_return_val_if_fail (REALM_IS_DISCO_DNS (enumerator), NULL);
	return REALM_DISCO_DNS (enumerator)->name;
}
