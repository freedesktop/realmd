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

#include "realm-ldap.h"

#include <glib/gi18n.h>
#include <glib-unix.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <errno.h>

#include <lber.h>

/*
 * So the reason that we don't use GSocket is because its fd's are always
 * non-blocking internally. We can't just go and hand these non-blocking
 * fds to openldap, which then fiddles with blocking state.
 */

GQuark
realm_ldap_error_get_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("realm-ldap-error");
	return quark;
}

typedef struct {
	GSource source;
	int sock;
	LDAP *ldap;
	GPollFD pollfd;
	GIOCondition condition;
	GCancellable *cancellable;
	GPollFD cancel_pollfd;

	gboolean connect_done;

	/* An LDAP failure we should always return if non-zero */
	int force_fail;
} LdapSource;

static gboolean
ldap_source_prepare (GSource *source,
                     gint *timeout)
{
	LdapSource *ls = (LdapSource *)source;

	if (ls->force_fail != 0)
		return TRUE;
	if (g_cancellable_is_cancelled (ls->cancellable))
		return TRUE;

	*timeout = -1;
	if ((ls->condition & ls->pollfd.revents) != 0)
		return TRUE;

	ls->pollfd.events = ls->condition;
	return FALSE;
}

static gboolean
ldap_source_check (GSource *source)
{
	int timeout;
	return ldap_source_prepare (source, &timeout);
}

static void
ldap_set_result_code (LDAP *ldap,
                      int rc)
{
	int check;

	if (ldap_get_option (ldap, LDAP_OPT_RESULT_CODE, &check) < 0 || check != rc) {
		ldap_set_option (ldap, LDAP_OPT_RESULT_CODE, &rc);
		ldap_set_option (ldap, LDAP_OPT_DIAGNOSTIC_MESSAGE, NULL);
	}
}

static gboolean
ldap_source_dispatch (GSource     *source,
                      GSourceFunc  callback,
                      gpointer     user_data)
{
	RealmLdapCallback func = (RealmLdapCallback)callback;
	LdapSource *ls = (LdapSource *)source;
	GIOCondition cond;
	socklen_t slen;
	int error;

	cond = ls->pollfd.revents & ls->condition;

	/*
	 * We report cancels as an error. The callback can check if it
	 * was cancelled by looking for LDAP_CANCELLED result code.
	 */
	if (g_cancellable_is_cancelled (ls->cancellable)) {
		ls->force_fail = LDAP_CANCELLED;

	} else if (cond & (G_IO_HUP | G_IO_ERR)) {
		g_debug ("socket closed or error");
		ls->force_fail = LDAP_SERVER_DOWN;

	} else if (cond & G_IO_OUT) {

		/*
		 * Read out the result of the asynchronous non-blocking connect()
		 * call. If it fails we propagate that error up.
		 */
		if (!ls->connect_done) {
			ls->connect_done = TRUE;
			slen = sizeof (int);
			if (getsockopt (ls->sock, SOL_SOCKET, SO_ERROR, &error, &slen) != 0) {
				g_warning ("getsockopt() for SO_ERROR failed");
				ls->force_fail = LDAP_SERVER_DOWN;
			} else if (error != 0) {
				g_debug ("Cannot connect: %s", g_strerror (error));
				ls->force_fail = LDAP_SERVER_DOWN;
			}
		}
	}

	if (ls->force_fail != 0) {
		ldap_set_result_code (ls->ldap, ls->force_fail);
		cond |= G_IO_ERR;
	}

	if (func != NULL && cond != 0) {
		cond = (* func) (ls->ldap, cond, user_data);
		if ((cond & G_IO_NVAL) == G_IO_NVAL)
			return FALSE;
		cond |= G_IO_HUP | G_IO_ERR;
		ls->condition = cond;
	}

	return TRUE;
}

static void
ldap_source_finalize (GSource *source)
{
	LdapSource *ls = (LdapSource *)source;

	ldap_destroy (ls->ldap);

	ls->sock = -1;
	ls->ldap = NULL;

	if (ls->cancellable) {
		g_cancellable_release_fd (ls->cancellable);
		g_object_unref (ls->cancellable);
	}
}

static GSourceFuncs socket_source_funcs = {
	ldap_source_prepare,
	ldap_source_check,
	ldap_source_dispatch,
	ldap_source_finalize,
};

/* Not included in ldap.h but documented */
int ldap_init_fd (ber_socket_t fd, int proto, LDAP_CONST char *url, struct ldap **ldp);

GSource *
realm_ldap_connect_anonymous (GSocketAddress *address,
                              GSocketProtocol protocol,
                              gboolean use_ldaps,
                              GCancellable *cancellable)
{
	GSource *source;
	LdapSource *ls;
	gchar *addrname;
	GInetSocketAddress *inet;
	GSocketFamily family;
	struct berval cred;
	Sockbuf *sb = NULL;
	gsize native_len;
	gpointer native;
	int version;
	gint port;
	gchar *url;
	int rc;
	int opt_rc;
	int ldap_opt_val;
	const char *errmsg = NULL;

	g_return_val_if_fail (G_IS_INET_SOCKET_ADDRESS (address), NULL);

	inet = G_INET_SOCKET_ADDRESS (address);
	addrname = g_inet_address_to_string (g_inet_socket_address_get_address (inet));
	port = g_inet_socket_address_get_port (inet);
	family = g_inet_address_get_family (g_inet_socket_address_get_address (inet));
	if (port == 0)
		port = 389;

	source = g_source_new (&socket_source_funcs, sizeof (LdapSource));
	g_source_set_name (source, "LdapSource");
	ls = (LdapSource *)source;

	switch (protocol) {
	case G_SOCKET_PROTOCOL_TCP:
		ls->sock = socket (g_socket_address_get_family (address),
		                   SOCK_STREAM, IPPROTO_TCP);

		/* Not an expected failure */
		if (ls->sock < 0) {
			g_critical ("couldn't open socket to: %s: %s", addrname, strerror (errno));
			return NULL;
		}

		if (!g_unix_set_fd_nonblocking (ls->sock, TRUE, NULL))
			g_warning ("couldn't set to non-blocking");

		native_len = g_socket_address_get_native_size (address);
		native = g_malloc (native_len);
		if (!g_socket_address_to_native (address, native, native_len, NULL))
			g_return_val_if_reached (NULL);

		if (connect (ls->sock, native, native_len) < 0 &&
		    errno != EINPROGRESS) {
			g_debug ("Cannot connect: %s", g_strerror (errno));
			ls->force_fail = LDAP_SERVER_DOWN;
		}

		if (!g_unix_set_fd_nonblocking (ls->sock, FALSE, NULL))
			g_warning ("couldn't set to blocking");

		if (family == G_SOCKET_FAMILY_IPV4) {
			url = g_strdup_printf ("%s://%s:%d",
			                       use_ldaps ? "ldaps" : "ldap",
			                       addrname, port);
		} else if (family == G_SOCKET_FAMILY_IPV6) {
			url = g_strdup_printf ("%s://[%s]:%d",
			                       use_ldaps ? "ldaps" : "ldap",
			                       addrname, port);
		} else {
			url = NULL;
		}
		rc = ldap_init_fd (ls->sock, 1, url, &ls->ldap);
		g_free (url);

		g_free (native);

		/* Not an expected failure */
		if (rc != LDAP_SUCCESS) {
			g_warning ("ldap_init_fd() failed: %s", ldap_err2string (rc));
			return NULL;
		}

		if (use_ldaps) {
			/* Since we currently use the IP address in the URI
			 * the certificate check might fail because in most
			 * cases the IP address won't be listed in the SANs of
			 * the LDAP server certificate. We will try to
			 * continue in this case and not fail. */
			ldap_opt_val = LDAP_OPT_X_TLS_ALLOW;
			rc = ldap_set_option (ls->ldap,
			                       LDAP_OPT_X_TLS_REQUIRE_CERT,
			                       &ldap_opt_val);
			if (rc != LDAP_OPT_SUCCESS) {
				g_debug ("Failed to disable certificate checking, trying without");
			}

			ldap_opt_val = 0;
			rc = ldap_set_option (ls->ldap, LDAP_OPT_X_TLS_NEWCTX,
			                       &ldap_opt_val);
			if (rc != LDAP_OPT_SUCCESS) {
				g_debug ("Failed to refresh LDAP context for TLS, trying without");
			}

			rc = ldap_install_tls (ls->ldap);
			if (rc != LDAP_SUCCESS) {
				opt_rc = ldap_get_option (ls->ldap,
				                          LDAP_OPT_DIAGNOSTIC_MESSAGE,
				                          (void *) &errmsg);
				if (opt_rc != LDAP_SUCCESS) {
					errmsg = "- no details -";
				}
				g_warning ("ldap_start_tls_s() failed [%s]: %s",
				           ldap_err2string (rc), errmsg);
				return NULL;
			}
		}

		break;

	case G_SOCKET_PROTOCOL_UDP:
		url = g_strdup_printf ("cldap://%s:%d", addrname, port);

		/*
		 * HACK: ldap_init_fd() does not work for UDP, otherwise we
		 * could use the same code path as above, but it doesn't
		 * block while connecting anyway, so just use ldap_initialize()
		 */
		rc = ldap_initialize (&ls->ldap, url);
		g_free (url);

		/* Not an expected failure */
		if (rc != LDAP_SUCCESS) {
			g_warning ("ldap_initialize() failed: %s", ldap_err2string (rc));
			return NULL;
		}

		/*
		 * An anonymous bind is used to actually connect the connection
		 * so we can get at the socket. For UDP with openldap an anonymous
		 * bind is treated as a no-op.
		 */

		cred.bv_val = "";
		cred.bv_len = 0;
		rc = ldap_sasl_bind_s (ls->ldap, NULL, LDAP_SASL_SIMPLE, &cred, NULL, NULL, NULL);

		/* Not an expected failure */
		if (rc != LDAP_SUCCESS) {
			g_warning ("ldap_sasl_bind_s() failed: %s", ldap_err2string (rc));
			return NULL;
		}

		if (ldap_get_option (ls->ldap, LDAP_OPT_SOCKBUF, &sb) < 0)
			g_return_val_if_reached (NULL);
		g_return_val_if_fail (sb != NULL, NULL);
		if (ber_sockbuf_ctrl (sb, LBER_SB_OPT_GET_FD, &ls->sock) != 1)
			g_return_val_if_reached (NULL);

		ls->connect_done = TRUE;
		break;

	default:
		g_return_val_if_reached (NULL);
		break;
	}

	g_free (addrname);

	version = LDAP_VERSION3;
	if (ldap_set_option (ls->ldap, LDAP_OPT_PROTOCOL_VERSION, &version) != 0)
		g_return_val_if_reached (NULL);
	ldap_set_option (ls->ldap, LDAP_OPT_REFERRALS , LDAP_OPT_OFF);

	ls->condition = G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR;
	ls->pollfd.fd = ls->sock;
	ls->pollfd.events = ls->condition;
	ls->pollfd.revents = 0;
	g_source_add_poll (source, &ls->pollfd);

	if (g_cancellable_make_pollfd (cancellable,
	                               &ls->cancel_pollfd)) {
		ls->cancellable = g_object_ref (cancellable);
		g_source_add_poll (source, &ls->cancel_pollfd);
	}

	return source;
}

void
realm_ldap_set_condition (GSource *source,
                          GIOCondition cond)
{
	LdapSource *ls = (LdapSource *)source;
	GMainContext *context;

	ls->condition = cond | G_IO_HUP | G_IO_ERR;

	context = g_source_get_context (source);
	if (context != NULL)
		g_main_context_wakeup (context);
}

void
realm_ldap_set_error (GError **error,
                      LDAP *ldap,
                      int code)
{
	char *info = NULL;

	if (code <= 0) {
		if (ldap_get_option (ldap, LDAP_OPT_RESULT_CODE, &code) != 0)
			g_return_if_reached ();
	}

	if (code == LDAP_CANCELLED) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
		             _("The operation was cancelled"));
		return;
	}

	if (ldap != NULL) {
		if (ldap_get_option (ldap, LDAP_OPT_DIAGNOSTIC_MESSAGE, (void*)&info) != 0)
			info = NULL;
	}

	g_set_error_literal (error, REALM_LDAP_ERROR, code,
	                     info ? info : ldap_err2string (code));
}
