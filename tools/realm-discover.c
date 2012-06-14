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

#include "realm-dbus-constants.h"
#include "realm-dbus-generated.h"

#include <krb5/krb5.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

static void
handle_error (GError *error,
              const gchar *format,
              ...)
{
	GString *message;
	va_list va;

	message = g_string_new ("");
	g_string_append_printf (message, "%s: ", g_get_prgname ());

	va_start (va, format);
	g_string_append_vprintf (message, format, va);
	va_end (va);

	if (error) {
		g_string_append (message, ": ");
		g_string_append (message, error->message);
		g_error_free (error);
	}

	g_printerr ("%s\n", message->str);
	g_string_free (message, TRUE);
}

static void
print_details_for_realm_info (GVariant *realm_info)
{
	RealmDbusKerberos *realm = NULL;
	const gchar *bus_name;
	const gchar *object_path;
	const gchar *interface_name;
	GError *error = NULL;
	GVariantIter iter;
	GVariant *details;
	const gchar *name;
	const gchar *value;

	g_variant_get (realm_info, "(&s&o&s)", &bus_name, &object_path, &interface_name);

	if (!g_str_equal (interface_name, REALM_DBUS_KERBEROS_REALM_INTERFACE))
		return;

	realm = realm_dbus_kerberos_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
	                                                    G_DBUS_PROXY_FLAGS_NONE,
	                                                    bus_name, object_path,
	                                                    NULL, &error);

	if (error != NULL) {
		g_warning ("couldn't use realm service: %s", error->message);
		g_error_free (error);
		return;
	}

	g_print ("%s\n", realm_dbus_kerberos_get_name (realm));
	g_print ("domain: %s\n", realm_dbus_kerberos_get_domain (realm));

	details = realm_dbus_kerberos_get_details (realm);
	if (details) {
		g_variant_iter_init (&iter, details);
		while (g_variant_iter_loop (&iter, "{&s&s}", &name, &value))
			g_print ("%s: %s\n", name, value);
	}

	g_object_unref (realm);
}

static void
on_diagnostics_signal (GDBusConnection *connection,
                       const gchar *sender_name,
                       const gchar *object_path,
                       const gchar *interface_name,
                       const gchar *signal_name,
                       GVariant *parameters,
                       gpointer user_data)
{
	const gchar *data;
	const gchar *unused;
	g_variant_get (parameters, "(&s&s)", &data, &unused);
	g_printerr ("%s", data);
}

static void
connect_to_diagnostics (GDBusProxy *proxy)
{
	GDBusConnection *connection;
	const gchar *bus_name;
	const gchar *object_path;

	connection = g_dbus_proxy_get_connection (proxy);
	bus_name = g_dbus_proxy_get_name (proxy);
	object_path = g_dbus_proxy_get_object_path (proxy);

	g_dbus_connection_signal_subscribe (connection, bus_name,
	                                    REALM_DBUS_DIAGNOSTICS_INTERFACE,
	                                    REALM_DBUS_DIAGNOSTICS_SIGNAL,
	                                    object_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                    on_diagnostics_signal, NULL, NULL);
}

typedef struct {
	GAsyncResult *result;
	GMainLoop *loop;
} SyncClosure;

static void
on_complete_get_result (GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
	SyncClosure *sync = user_data;
	sync->result = g_object_ref (result);
	g_main_loop_quit (sync->loop);
}

static int
realm_discover (const gchar *string,
                gboolean verbose)
{
	RealmDbusProvider *provider;
	GVariant *realm_info;
	gboolean found = FALSE;
	GError *error = NULL;
	GVariantIter iter;
	SyncClosure sync;
	GVariant *realms;
	gint relevance;

	provider = realm_dbus_provider_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
	                                                       G_DBUS_PROXY_FLAGS_NONE,
	                                                       REALM_DBUS_ALL_PROVIDER_NAME,
	                                                       REALM_DBUS_ALL_PROVIDER_PATH,
	                                                       NULL, &error);
	if (error != NULL) {
		handle_error (error, "couldn't connect to realm service");
		return 2;
	}

	/* Setup diagnostics */
	if (verbose)
		connect_to_diagnostics (G_DBUS_PROXY (provider));

	sync.result = NULL;
	sync.loop = g_main_loop_new (NULL, FALSE);

	g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (provider), G_MAXINT);
	realm_dbus_provider_call_discover (provider, string, "unused-operation-id",
	                                   NULL, on_complete_get_result, &sync);

	/* This mainloop is quit by on_complete_get_result */
	g_main_loop_run (sync.loop);

	realm_dbus_provider_call_discover_finish (provider, &relevance, &realms,
	                                          sync.result, &error);

	g_object_unref (sync.result);
	g_main_loop_unref (sync.loop);

	if (error != NULL) {
		handle_error (error, "couldn't discover realm");
		return 2;
	}

	g_variant_iter_init (&iter, realms);
	while ((realm_info = g_variant_iter_next_value (&iter)) != NULL) {
		print_details_for_realm_info (realm_info);
		g_variant_unref (realm_info);
		found = TRUE;
	}

	g_variant_unref (realms);

	if (!found) {
		handle_error (NULL, "no such realm found: %s", string);
		return 1;
	}

	return 0;
}

int
main (int argc,
      char *argv[])
{
	GOptionContext *context;
	gboolean arg_verbose = FALSE;
	GError *error = NULL;
	gint result = 0;
	gint ret;
	gint i;

	GOptionEntry option_entries[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &arg_verbose, "Verbose output", NULL },
		{ NULL, }
	};

	g_type_init ();

	context = g_option_context_new ("realm-or-domain");
	g_option_context_add_main_entries (context, option_entries, NULL);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_printerr ("%s: %s\n", g_get_prgname (), error->message);
		g_error_free (error);
		ret = 2;
	}

	/* The default realm? */
	if (argc == 1) {
		ret = realm_discover ("", arg_verbose);

	/* Specific realms */
	} else {
		for (i = 1; i < argc; i++) {
			ret = realm_discover (argv[i], arg_verbose);
			if (ret != 0)
				result = ret;
		}
	}

	g_option_context_free (context);
	return result;
}