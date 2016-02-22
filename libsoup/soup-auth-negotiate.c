/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-auth-negotiate.c: HTTP Negotiate Authentication helper
 *
 * Copyright (C) 2009,2013 Guido Guenther <agx@sigxcpu.org>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#ifdef LIBSOUP_HAVE_GSSAPI
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>
#endif /* LIBSOUP_HAVE_GSSAPI */

#include "soup-auth-negotiate.h"
#include "soup-headers.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-misc-private.h"
#include "soup-uri.h"

#ifdef LIBSOUP_HAVE_GSSAPI
const gboolean soup_auth_negotiate_supported = TRUE;
#else
const gboolean soup_auth_negotiate_supported = FALSE;
#endif /* LIBSOUP_HAVE_GSSAPI */

G_DEFINE_TYPE (SoupAuthNegotiate, soup_auth_negotiate, SOUP_TYPE_CONNECTION_AUTH)

#define AUTH_GSS_ERROR      -1
#define AUTH_GSS_COMPLETE    1
#define AUTH_GSS_CONTINUE    0

typedef enum {
	SOUP_NEGOTIATE_NEW,
	SOUP_NEGOTIATE_RECEIVED_CHALLENGE, /* received initial negotiate header */
	SOUP_NEGOTIATE_SENT_RESPONSE,      /* sent response to server */
	SOUP_NEGOTIATE_FAILED
} SoupNegotiateState;

typedef struct {
	gboolean initialized;
	gchar *response_header;

#ifdef LIBSOUP_HAVE_GSSAPI
	gss_ctx_id_t context;
	gss_name_t   server_name;
#endif /* LIBSOUP_HAVE_GSSAPI */

	SoupNegotiateState state;
} SoupNegotiateConnectionState;

typedef struct {
	gboolean is_authenticated;

	gulong message_finished_signal_id;
	gulong message_got_headers_signal_id;

	SoupNegotiateConnectionState *conn_state;
} SoupAuthNegotiatePrivate;

#define SOUP_AUTH_NEGOTIATE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_AUTH_NEGOTIATE, SoupAuthNegotiatePrivate))

#ifdef LIBSOUP_HAVE_GSSAPI
static gboolean check_auth_trusted_uri (SoupConnectionAuth *auth,
					SoupMessage *msg);
static gboolean soup_gss_build_response (SoupNegotiateConnectionState *conn,
					 SoupAuth *auth, GError **err);
static void soup_gss_client_cleanup (SoupNegotiateConnectionState *conn);
static gchar *soup_gss_client_get_realm (SoupAuth *auth, GError **err);
static gboolean soup_gss_client_init (SoupNegotiateConnectionState *conn,
				      const char *host, GError **err);
static int soup_gss_client_step (SoupNegotiateConnectionState *conn,
				 const char *host, GError **err);

static GSList *trusted_uris;
static GSList *blacklisted_uris;

static void parse_uris_from_env_variable (const gchar *env_variable, GSList **list);

static void check_server_response (SoupMessage *msg, gpointer auth);
static void remove_server_response_handler (SoupMessage *msg, gpointer auth);

static const char spnego_OID[] = "\x2b\x06\x01\x05\x05\x02";
static const gss_OID_desc gss_mech_spnego = { sizeof (spnego_OID) - 1, (void *) &spnego_OID };

static const char ntlmssp_v1_OID[] = "\x2b\x06\x01\x04\x01\x82\x37\x02\x02\x0a";
static const gss_OID_desc gss_mech_ntlmssp_v1 = { sizeof (ntlmssp_v1_OID) - 1, (void *) &ntlmssp_v1_OID };
#endif /* LIBSOUP_HAVE_GSSAPI */

static void
soup_auth_negotiate_init (SoupAuthNegotiate *negotiate)
{
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);

	priv->message_got_headers_signal_id = 0;
	priv->message_finished_signal_id = 0;
	priv->is_authenticated = FALSE;
}

static gpointer
soup_auth_negotiate_create_connection_state (SoupConnectionAuth *auth)
{
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (auth);
	SoupNegotiateConnectionState *conn;

	conn = g_slice_new0 (SoupNegotiateConnectionState);
	conn->state = SOUP_NEGOTIATE_NEW;
	priv->conn_state = conn;

	return conn;
}

static void
soup_auth_negotiate_free_connection_state (SoupConnectionAuth *auth,
					   gpointer state)
{
	SoupAuthNegotiate *negotiate = SOUP_AUTH_NEGOTIATE (auth);
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);
	SoupNegotiateConnectionState *conn = state;

#ifdef LIBSOUP_HAVE_GSSAPI
	soup_gss_client_cleanup (conn);
#endif /* LIBSOUP_HAVE_GSSAPI */
	g_free (conn->response_header);

	g_slice_free (SoupNegotiateConnectionState, conn);
	priv->conn_state = NULL;
}

static gboolean
soup_auth_negotiate_update_connection (SoupConnectionAuth *auth, SoupMessage *msg,
				       const char *header, gpointer state)
{
	SoupNegotiateConnectionState *conn = state;
#ifdef LIBSOUP_HAVE_GSSAPI
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (auth);
	GError *err = NULL;

	if (!check_auth_trusted_uri (auth, msg)) {
		conn->state = SOUP_NEGOTIATE_FAILED;
		return TRUE;
	}

	/* Found negotiate header with no token, start negotiate */
	if (strcmp (header, "Negotiate") == 0) {
		if (conn->state > SOUP_NEGOTIATE_RECEIVED_CHALLENGE) {
			/* If we were already negotiating and we get a 401
			 * with no token, that means we failed. */
			conn->state = SOUP_NEGOTIATE_FAILED;
			return TRUE;
		}
		conn->state = SOUP_NEGOTIATE_RECEIVED_CHALLENGE;
		if (soup_gss_build_response (conn, SOUP_AUTH (auth), &err)) {
			/* Register the callbacks just once */
			if (priv->message_finished_signal_id == 0) {
				gulong id = 0;
				id = g_signal_connect (msg,
						       "finished",
						       G_CALLBACK (remove_server_response_handler),
						       auth);
				priv->message_finished_signal_id = id;
			}

			if (priv->message_got_headers_signal_id == 0) {
				gulong id = 0;
				/* Wait for the 2xx response to verify server response */
				id = g_signal_connect (msg,
						       "got_headers",
						       G_CALLBACK (check_server_response),
						       auth);
				priv->message_got_headers_signal_id = id;
			}
			return TRUE;
		} else {
			/* FIXME: report further upward via
			 * soup_message_get_error_message  */
			g_warning ("gssapi step failed: %s", err->message);
		}
	} else if (!strncmp (header, "Negotiate ", 10)) {
		if (soup_gss_client_step (conn, header + 10, &err) == AUTH_GSS_CONTINUE) {
			conn->state = SOUP_NEGOTIATE_RECEIVED_CHALLENGE;
			return TRUE;
		}
	}

	g_clear_error (&err);
#endif /* LIBSOUP_HAVE_GSSAPI */
	conn->state = SOUP_NEGOTIATE_FAILED;
	return TRUE;
}

static GSList *
soup_auth_negotiate_get_protection_space (SoupAuth *auth, SoupURI *source_uri)
{
	char *space, *p;

	space = g_strdup (source_uri->path);

	/* Strip filename component */
	p = strrchr (space, '/');
	if (p && p == space && p[1])
		p[1] = '\0';
	else if (p && p[1])
		*p = '\0';

	return g_slist_prepend (NULL, space);
}

static void
soup_auth_negotiate_authenticate (SoupAuth *auth, const char *username,
				  const char *password)
{
	SoupAuthNegotiate *negotiate = SOUP_AUTH_NEGOTIATE (auth);
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);

	/* It is not possible to authenticate with username and password. */
	priv->is_authenticated = FALSE;
}

static gboolean
soup_auth_negotiate_is_authenticated (SoupAuth *auth)
{
	SoupAuthNegotiate *negotiate = SOUP_AUTH_NEGOTIATE (auth);
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);

	/* We are authenticated just in case we received the GSS_S_COMPLETE. */
	return priv->is_authenticated;
}

static gboolean
soup_auth_negotiate_can_authenticate (SoupAuth *auth)
{
	return FALSE;
}

static char *
soup_auth_negotiate_get_connection_authorization (SoupConnectionAuth *auth,
						  SoupMessage *msg,
						  gpointer state)
{
	SoupNegotiateConnectionState *conn = state;
	char *header = NULL;

	if (conn->state == SOUP_NEGOTIATE_RECEIVED_CHALLENGE) {
		header = conn->response_header;
		conn->response_header = NULL;
		conn->state = SOUP_NEGOTIATE_SENT_RESPONSE;
	}

	return header;
}

static gboolean
soup_auth_negotiate_is_connection_ready (SoupConnectionAuth *auth,
					 SoupMessage        *msg,
					 gpointer            state)
{
	SoupNegotiateConnectionState *conn = state;

	return conn->state != SOUP_NEGOTIATE_FAILED;
}

static void
soup_auth_negotiate_class_init (SoupAuthNegotiateClass *auth_negotiate_class)
{
	SoupAuthClass *auth_class = SOUP_AUTH_CLASS (auth_negotiate_class);
	SoupConnectionAuthClass *conn_auth_class =
			SOUP_CONNECTION_AUTH_CLASS (auth_negotiate_class);

	g_type_class_add_private (auth_negotiate_class, sizeof (SoupAuthNegotiatePrivate));

	auth_class->scheme_name = "Negotiate";
	auth_class->strength = 7;

	auth_class->get_protection_space = soup_auth_negotiate_get_protection_space;
	auth_class->authenticate = soup_auth_negotiate_authenticate;
	auth_class->is_authenticated = soup_auth_negotiate_is_authenticated;
	auth_class->can_authenticate = soup_auth_negotiate_can_authenticate;

	conn_auth_class->create_connection_state = soup_auth_negotiate_create_connection_state;
	conn_auth_class->free_connection_state = soup_auth_negotiate_free_connection_state;
	conn_auth_class->update_connection = soup_auth_negotiate_update_connection;
	conn_auth_class->get_connection_authorization = soup_auth_negotiate_get_connection_authorization;
	conn_auth_class->is_connection_ready = soup_auth_negotiate_is_connection_ready;

#ifdef LIBSOUP_HAVE_GSSAPI
	parse_uris_from_env_variable ("SOUP_GSSAPI_TRUSTED_URIS", &trusted_uris);
	parse_uris_from_env_variable ("SOUP_GSSAPI_BLACKLISTED_URIS", &blacklisted_uris);
#endif /* LIBSOUP_HAVE_GSSAPI */
}

#ifdef LIBSOUP_HAVE_GSSAPI
static void
check_server_response (SoupMessage *msg, gpointer auth)
{
	gint ret;
	const char *auth_headers;
	GError *err = NULL;
	SoupAuthNegotiate *negotiate = auth;
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);
	SoupNegotiateConnectionState *conn = priv->conn_state;

	if (auth != soup_message_get_auth (msg))
		return;

	if (msg->status_code == SOUP_STATUS_UNAUTHORIZED)
		return;

	/* FIXME: need to check for proxy-auth too */
	auth_headers = soup_message_headers_get_one (msg->response_headers,
						     "WWW-Authenticate");
	if (!auth_headers || g_ascii_strncasecmp (auth_headers, "Negotiate ", 10) != 0) {
		g_warning ("Failed to parse auth header %s", auth_headers);
		conn->state = SOUP_NEGOTIATE_FAILED;
		goto out;
	}

	ret = soup_gss_client_step (conn, auth_headers + 10, &err);

	priv->is_authenticated = ret == AUTH_GSS_COMPLETE;

	if (ret == AUTH_GSS_CONTINUE) {
		conn->state = SOUP_NEGOTIATE_RECEIVED_CHALLENGE;
	} else if (ret == AUTH_GSS_ERROR) {
		if (err)
			g_warning ("%s", err->message);
		conn->state = SOUP_NEGOTIATE_FAILED;
	}
 out:
	g_clear_error (&err);
}

static void
remove_server_response_handler (SoupMessage *msg, gpointer auth)
{
	SoupAuthNegotiate *negotiate = auth;
	SoupAuthNegotiatePrivate *priv = SOUP_AUTH_NEGOTIATE_GET_PRIVATE (negotiate);

	g_signal_handler_disconnect (msg, priv->message_got_headers_signal_id);
	priv->message_got_headers_signal_id = 0;

	g_signal_handler_disconnect (msg, priv->message_finished_signal_id);
	priv->message_finished_signal_id = 0;
}

/* Check if scheme://host:port from message matches the given URI. */
static gint
match_base_uri (SoupURI *list_uri, SoupURI *msg_uri)
{
	if (msg_uri->scheme != list_uri->scheme)
		return 1;

	if (list_uri->port && (msg_uri->port != list_uri->port))
		return 1;

	if (list_uri->host)
		return !soup_host_matches_host (msg_uri->host, list_uri->host);

	return 0;
}

/* Parses a comma separated list of URIS from the environment. */
static void
parse_uris_from_env_variable (const gchar *env_variable, GSList **list)
{
	gchar **uris = NULL;
	const gchar *env;
	gint i;
	guint length;

	/* Initialize the list */
	*list = NULL;

	if (!(env = g_getenv (env_variable)))
		return;

	if (!(uris = g_strsplit (env, ",", -1)))
		return;

	length = g_strv_length (uris);
	for (i = 0; i < length; i++) {
		SoupURI *uri;

		/* If the supplied URI is valid, append it to the list */
		if ((uri = soup_uri_new (uris[i])))
			*list = g_slist_prepend (*list, uri);
	}

	g_strfreev (uris);
}

static gboolean
check_auth_trusted_uri (SoupConnectionAuth *auth, SoupMessage *msg)
{
	SoupURI *msg_uri;
	GSList *matched = NULL;

	g_return_val_if_fail (auth != NULL, FALSE);
	g_return_val_if_fail (msg != NULL, FALSE);

	msg_uri = soup_message_get_uri (msg);

	/* First check if the URI is not on blacklist */
	if (blacklisted_uris &&
	    g_slist_find_custom (blacklisted_uris, msg_uri, (GCompareFunc) match_base_uri))
		return FALSE;

	/* If no trusted URIs are set, we allow all HTTPS URIs */
	if (!trusted_uris)
		return msg_uri->scheme == SOUP_URI_SCHEME_HTTPS;

	matched = g_slist_find_custom (trusted_uris,
				       msg_uri,
				       (GCompareFunc) match_base_uri);

	return matched ? TRUE : FALSE;
}

static gboolean
soup_gss_build_response (SoupNegotiateConnectionState *conn, SoupAuth *auth, GError **err)
{
	if (!conn->initialized) {
		gchar *realm = NULL;
		if ((realm = soup_gss_client_get_realm (auth, NULL))) {
			g_object_set (G_OBJECT (auth), SOUP_AUTH_REALM, realm, NULL);
			g_free (realm);
		}

		if (!soup_gss_client_init (conn, soup_auth_get_host (auth), err))
			return FALSE;
	}

	if (soup_gss_client_step (conn, "", err) != AUTH_GSS_CONTINUE)
		return FALSE;

	return TRUE;
}

static void
soup_gss_error (OM_uint32 err_maj, OM_uint32 err_min, GError **err)
{
	OM_uint32 maj_stat, min_stat, msg_ctx = 0;
	gss_buffer_desc status;
	gchar *buf_maj = NULL, *buf_min = NULL;

	do {
		maj_stat = gss_display_status (&min_stat,
					       err_maj,
					       GSS_C_GSS_CODE,
					       (gss_OID) &gss_mech_spnego,
					       &msg_ctx,
					       &status);
		if (GSS_ERROR (maj_stat))
			break;

		buf_maj = g_strdup ((gchar *) status.value);
		gss_release_buffer (&min_stat, &status);

		maj_stat = gss_display_status (&min_stat,
					       err_min,
					       GSS_C_MECH_CODE,
					       GSS_C_NULL_OID,
					       &msg_ctx,
					       &status);
		if (!GSS_ERROR (maj_stat)) {
			buf_min = g_strdup ((gchar *) status.value);
			gss_release_buffer (&min_stat, &status);
		}

		if (err && *err == NULL) {
			g_set_error (err,
				     SOUP_HTTP_ERROR,
				     SOUP_STATUS_UNAUTHORIZED,
				     "%s: %s",
				     buf_maj,
				     buf_min ? buf_min : "");
		}
		g_free (buf_maj);
		g_free (buf_min);
		buf_min = buf_maj = NULL;
	} while (!GSS_ERROR (maj_stat) && msg_ctx != 0);
}

static gchar *
soup_gss_client_get_realm (SoupAuth *auth, GError **err)
{
	gchar *name = NULL, *realm = NULL;
	OM_uint32 maj_stat, min_stat;
	gss_name_t gss_name;
	gss_buffer_desc out = GSS_C_EMPTY_BUFFER;
	gss_OID_set mechanisms;

	maj_stat = gss_inquire_cred (&min_stat,
				     GSS_C_NO_CREDENTIAL,
				     &gss_name,
				     NULL,
				     NULL,
				     &mechanisms);

	if (GSS_ERROR (maj_stat)) {
		soup_gss_error (maj_stat, min_stat, err);
		goto out;
	}

	if (maj_stat != GSS_S_COMPLETE)
		goto out;

	maj_stat = gss_display_name (&min_stat,
				     gss_name,
				     &out,
				     NULL);

	if (GSS_ERROR (maj_stat)) {
		soup_gss_error (maj_stat, min_stat, err);
		goto out;
	}

	if (maj_stat == GSS_S_COMPLETE)
		name = g_strndup (out.value, out.length);

	if (name && *name && mechanisms->count > 0) {
		gss_OID oid = &mechanisms->elements[0];

		if (gss_oid_equal (oid, (gss_OID) gss_mech_krb5) ||
		    gss_oid_equal (oid, (gss_OID) gss_mech_krb5_old)) {
			/* user@REALM */
			const gchar *realm_start = strstr (name, "@");
			if (realm_start && *realm_start)
				realm = g_strdup (realm_start + 1);
		} else if (gss_oid_equal (oid, (gss_OID) &gss_mech_ntlmssp_v1)) {
			/* REALM/user */
			const gchar *realm_end = strstr (name, "/");
			if (realm_end)
				realm = g_strndup (name, realm_end - name);
		}
	}
	maj_stat = gss_release_oid_set(&min_stat, &mechanisms);
	maj_stat = gss_release_buffer (&min_stat, &out);
 out:
	maj_stat = gss_release_name (&min_stat, &gss_name);

	g_free (name);

	return realm;
}

static gboolean
soup_gss_client_init (SoupNegotiateConnectionState *conn, const gchar *host, GError **err)
{
	OM_uint32 maj_stat, min_stat;
	gchar *service = NULL;
	gss_buffer_desc token = GSS_C_EMPTY_BUFFER;
	gboolean ret = FALSE;
	gchar *h;

	conn->server_name = GSS_C_NO_NAME;
	conn->context = GSS_C_NO_CONTEXT;

	h = g_ascii_strdown (host, -1);
	service = g_strconcat ("HTTP@", h, NULL);
	token.length = strlen (service);
	token.value = (gchar *) service;

	maj_stat = gss_import_name (&min_stat,
				    &token,
				    (gss_OID) GSS_C_NT_HOSTBASED_SERVICE,
				    &conn->server_name);

	if (GSS_ERROR (maj_stat)) {
		soup_gss_error (maj_stat, min_stat, err);
		ret = FALSE;
		goto out;
	}

	conn->initialized = TRUE;
	ret = TRUE;
out:
	g_free (h);
	g_free (service);
	return ret;
}

static gint
soup_gss_client_step (SoupNegotiateConnectionState *conn, const gchar *challenge, GError **err)
{
	OM_uint32 maj_stat, min_stat;
	gss_buffer_desc in = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc out = GSS_C_EMPTY_BUFFER;
	gint ret = AUTH_GSS_CONTINUE;

	g_clear_pointer (&conn->response_header, g_free);

	if (challenge && *challenge) {
		size_t len;
		in.value = g_base64_decode (challenge, &len);
		in.length = len;
	}

	maj_stat = gss_init_sec_context (&min_stat,
					 GSS_C_NO_CREDENTIAL,
					 &conn->context,
					 conn->server_name,
					 (gss_OID) &gss_mech_spnego,
					 GSS_C_MUTUAL_FLAG,
					 GSS_C_INDEFINITE,
					 GSS_C_NO_CHANNEL_BINDINGS,
					 &in,
					 NULL,
					 &out,
					 NULL,
					 NULL);

	if ((maj_stat != GSS_S_COMPLETE) && (maj_stat != GSS_S_CONTINUE_NEEDED)) {
		soup_gss_error (maj_stat, min_stat, err);
		ret = AUTH_GSS_ERROR;
		goto out;
	}

	ret = (maj_stat == GSS_S_COMPLETE) ? AUTH_GSS_COMPLETE : AUTH_GSS_CONTINUE;
	if (out.length) {
		gchar *response = g_base64_encode ((const guchar *) out.value, out.length);
		conn->response_header = g_strconcat ("Negotiate ", response, NULL);
		g_free (response);
		maj_stat = gss_release_buffer (&min_stat, &out);
	}

out:
	if (out.value)
		gss_release_buffer (&min_stat, &out);
	if (in.value)
		g_free (in.value);
	return ret;
}

static void
soup_gss_client_cleanup (SoupNegotiateConnectionState *conn)
{
	OM_uint32 min_stat;

	gss_release_name (&min_stat, &conn->server_name);
}
#endif /* LIBSOUP_HAVE_GSSAPI */
