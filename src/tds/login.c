/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005  Brian Bruns
 * Copyright (C) 2005-2015  Ziglio Frediano
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <stdarg.h>
#include <stdio.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#include <assert.h>

#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif /* HAVE_SYS_SOCKET_H */

#ifdef _WIN32
#include <process.h>
#endif

#include <freetds/tds.h>
#include <freetds/iconv.h>
#include <freetds/utils/string.h>
#include <freetds/bytes.h>
#include <freetds/tls.h>
#include <freetds/stream.h>
#include <freetds/checks.h>
#include <freetds/replacements.h>

static TDSRET tds_send_login(TDSSOCKET * tds, const TDSLOGIN * login);
static TDSRET tds71_do_login(TDSSOCKET * tds, TDSLOGIN * login);
static TDSRET tds7_send_login(TDSSOCKET * tds, const TDSLOGIN * login);
static void tds7_crypt_pass(const unsigned char *clear_pass,
			    size_t len, unsigned char *crypt_pass);

void
tds_set_version(TDSLOGIN * tds_login, TDS_TINYINT major_ver, TDS_TINYINT minor_ver)
{
	tds_login->tds_version = ((TDS_USMALLINT) major_ver << 8) + minor_ver;
}

void
tds_set_packet(TDSLOGIN * tds_login, int packet_size)
{
	tds_login->block_size = packet_size;
}

void
tds_set_port(TDSLOGIN * tds_login, int port)
{
	tds_login->port = port;
}

bool
tds_set_passwd(TDSLOGIN * tds_login, const char *password)
{
	if (password) {
		tds_dstr_zero(&tds_login->password);
		return !!tds_dstr_copy(&tds_login->password, password);
	}
	return true;
}
void
tds_set_bulk(TDSLOGIN * tds_login, bool enabled)
{
	tds_login->bulk_copy = enabled ? 1 : 0;
}

bool
tds_set_user(TDSLOGIN * tds_login, const char *username)
{
	return !!tds_dstr_copy(&tds_login->user_name, username);
}

bool
tds_set_host(TDSLOGIN * tds_login, const char *hostname)
{
	return !!tds_dstr_copy(&tds_login->client_host_name, hostname);
}

bool
tds_set_app(TDSLOGIN * tds_login, const char *application)
{
	return !!tds_dstr_copy(&tds_login->app_name, application);
}

/**
 * \brief Set the servername in a TDSLOGIN structure
 *
 * Normally copies \a server into \a tds_login.  If \a server does not point to a plausible name, the environment 
 * variables TDSQUERY and DSQUERY are used, in that order.  If they don't exist, the "default default" servername
 * is "SYBASE" (although the utility of that choice is a bit murky).  
 *
 * \param tds_login	points to a TDSLOGIN structure
 * \param server	the servername, or NULL, or a zero-length string
 * \todo open the log file earlier, so these messages can be seen.  
 */
bool
tds_set_server(TDSLOGIN * tds_login, const char *server)
{
#if 0
	/* Doing this in tds_alloc_login instead */
	static const char *names[] = { "TDSQUERY", "DSQUERY", "SYBASE" };
	int i;
	
	for (i=0; i < TDS_VECTOR_SIZE(names) && (!server || strlen(server) == 0); i++) {
		const char *source;
		if (i + 1 == TDS_VECTOR_SIZE(names)) {
			server = names[i];
			source = "compiled-in default";
		} else {
			server = getenv(names[i]);
			source = names[i];
		}
		if (server) {
			tdsdump_log(TDS_DBG_INFO1, "Setting TDSLOGIN::server_name to '%s' from %s.\n", server, source);
		}
	}
#endif
	if (server)
		return !!tds_dstr_copy(&tds_login->server_name, server);
	return true;
}

bool
tds_set_library(TDSLOGIN * tds_login, const char *library)
{
	return !!tds_dstr_copy(&tds_login->library, library);
}

bool
tds_set_client_charset(TDSLOGIN * tds_login, const char *charset)
{
	return !!tds_dstr_copy(&tds_login->client_charset, charset);
}

bool
tds_set_language(TDSLOGIN * tds_login, const char *language)
{
	return !!tds_dstr_copy(&tds_login->language, language);
}

struct tds_save_msg
{
	TDSMESSAGE msg;
	char type;
};

struct tds_save_env
{
	char *oldval;
	char *newval;
	int type;
};

typedef struct tds_save_context
{
	/* must be first !!! */
	TDSCONTEXT ctx;

	unsigned num_msg;
	struct tds_save_msg msgs[10];

	unsigned num_env;
	struct tds_save_env envs[10];
} TDSSAVECONTEXT;

static void
tds_save(TDSSAVECONTEXT *ctx, char type, TDSMESSAGE *msg)
{
	struct tds_save_msg *dest_msg;

	if (ctx->num_msg >= TDS_VECTOR_SIZE(ctx->msgs))
		return;

	dest_msg = &ctx->msgs[ctx->num_msg];
	dest_msg->type = type;
	dest_msg->msg = *msg;
#define COPY(name) if (msg->name) dest_msg->msg.name = strdup(msg->name);
	COPY(server);
	COPY(message);
	COPY(proc_name);
	COPY(sql_state);
#undef COPY
	++ctx->num_msg;
}

static int
tds_save_msg(const TDSCONTEXT *ctx, TDSSOCKET *tds TDS_UNUSED, TDSMESSAGE *msg)
{
	tds_save((TDSSAVECONTEXT *) ctx, 0, msg);
	return 0;
}

static int
tds_save_err(const TDSCONTEXT *ctx, TDSSOCKET *tds TDS_UNUSED, TDSMESSAGE *msg)
{
	tds_save((TDSSAVECONTEXT *) ctx, 1, msg);
	return TDS_INT_CANCEL;
}

static void
tds_save_env(TDSSOCKET * tds, int type, char *oldval, char *newval)
{
	TDSSAVECONTEXT *ctx;
	struct tds_save_env *env;

	if (tds_get_ctx(tds)->msg_handler != tds_save_msg)
		return;

	ctx = (TDSSAVECONTEXT *) tds_get_ctx(tds);
	if (ctx->num_env >= TDS_VECTOR_SIZE(ctx->envs))
		return;

	env = &ctx->envs[ctx->num_env];
	env->type = type;
	env->oldval = oldval ? strdup(oldval) : NULL;
	env->newval = newval ? strdup(newval) : NULL;
	++ctx->num_env;
}

static void
init_save_context(TDSSAVECONTEXT *ctx, const TDSCONTEXT *old_ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->ctx.locale = old_ctx->locale;
	ctx->ctx.msg_handler = tds_save_msg;
	ctx->ctx.err_handler = tds_save_err;
}

static void
replay_save_context(TDSSOCKET *tds, TDSSAVECONTEXT *ctx)
{
	unsigned n;

	/* replay all recorded messages */
	for (n = 0; n < ctx->num_msg; ++n)
		if (ctx->msgs[n].type == 0) {
			if (tds_get_ctx(tds)->msg_handler)
				tds_get_ctx(tds)->msg_handler(tds_get_ctx(tds), tds, &ctx->msgs[n].msg);
		} else {
			if (tds_get_ctx(tds)->err_handler)
				tds_get_ctx(tds)->err_handler(tds_get_ctx(tds), tds, &ctx->msgs[n].msg);
		}

	/* replay all recorded envs */
	for (n = 0; n < ctx->num_env; ++n)
		if (tds->env_chg_func)
			tds->env_chg_func(tds, ctx->envs[n].type, ctx->envs[n].oldval, ctx->envs[n].newval);
}

static void
reset_save_context(TDSSAVECONTEXT *ctx)
{
	unsigned n;

	/* free all messages */
	for (n = 0; n < ctx->num_msg; ++n)
		tds_free_msg(&ctx->msgs[n].msg);
	ctx->num_msg = 0;

	/* free all envs */
	for (n = 0; n < ctx->num_env; ++n) {
		free(ctx->envs[n].oldval);
		free(ctx->envs[n].newval);
	}
	ctx->num_env = 0;
}

static void
free_save_context(TDSSAVECONTEXT *ctx)
{
	reset_save_context(ctx);
}

/**
 * Set @@spid based on column data
 * \tds
 * @param curcol  column with spid data.
 */
static TDSRET
tds_set_spid(TDSSOCKET * tds, TDSCOLUMN *curcol)
{
	switch (tds_get_conversion_type(curcol->column_type, curcol->column_size)) {
	case SYBINT2:
		tds->conn->spid = *((TDS_USMALLINT *) curcol->column_data);
		break;
	case SYBINT4:
		tds->conn->spid = *((TDS_UINT *) curcol->column_data);
		break;
	default:
		return TDS_FAIL;
	}
	return TDS_SUCCESS;
}

/**
 * Set ncharsize based on column data.
 * \tds
 * @param res_info  resultset to get data from.
 */
static TDSRET
tds_set_nvc(TDSSOCKET * tds, TDSRESULTINFO *res_info)
{
	int charsize;

	/* Compute the ratios, put some acceptance in order to avoid issues. */
	/* The "3" constant came from the query issued (NVARCHAR(3)) */
	charsize = res_info->columns[0]->on_server.column_size / 3;
	if (charsize >= 1 && charsize <= 4)
		tds->conn->ncharsize = (uint8_t) charsize;
	return TDS_SUCCESS;
}

/**
 * Set unicharsize based on column data.
 * \tds
 * @param res_info  resultset to get data from.
 */
static TDSRET
tds_set_uvc(TDSSOCKET * tds, TDSRESULTINFO *res_info)
{
	int charsize;

	/* Compute the ratios, put some acceptance in order to avoid issues. */
	/* The "3" constant came from the query issued (UNIVARCHAR(3)) */
	charsize = res_info->columns[0]->on_server.column_size / 3;
	if (charsize >= 1 && charsize <= 4)
		tds->conn->unicharsize = (uint8_t) charsize;
	return TDS_SUCCESS;
}

/**
 * Parse the results from login queries
 * \tds
 */
static TDSRET
tds_parse_login_results(TDSSOCKET * tds, bool ignore_errors)
{
	TDS_INT result_type;
	TDS_INT done_flags;
	TDSRET rc;
	TDSCOLUMN *curcol;
	bool last_required = false;

	CHECK_TDS_EXTRA(tds);

	while ((rc = tds_process_tokens(tds, &result_type, &done_flags, TDS_RETURN_ROW|TDS_RETURN_DONE)) == TDS_SUCCESS) {

		switch (result_type) {
		case TDS_ROW_RESULT:
			if (!tds->res_info && tds->res_info->num_cols < 1)
				return TDS_FAIL;
			curcol = tds->res_info->columns[0];
			if (tds->res_info->num_cols == 1 && strcmp(tds_dstr_cstr(&curcol->column_name), "spid") == 0)
				rc = tds_set_spid(tds, curcol);
			if (tds->res_info->num_cols == 1 && strcmp(tds_dstr_cstr(&curcol->column_name), "nvc") == 0) {
				rc = tds_set_nvc(tds, tds->res_info);
				last_required = true;
			}
			if (tds->res_info->num_cols == 1 && strcmp(tds_dstr_cstr(&curcol->column_name), "uvc") == 0)
				rc = tds_set_uvc(tds, tds->res_info);
			TDS_PROPAGATE(rc);
			break;

		case TDS_DONE_RESULT:
		case TDS_DONEPROC_RESULT:
		case TDS_DONEINPROC_RESULT:
			if ((done_flags & TDS_DONE_ERROR) != 0 && !ignore_errors)
				return TDS_FAIL;
			if (last_required)
				ignore_errors = true;
			break;
		}
	}
	if (rc == TDS_NO_MORE_RESULTS)
		rc = TDS_SUCCESS;

	return rc;
}

static TDSRET
tds_process_single(TDSSOCKET *tds, char *query, bool ignore_errors)
{
	TDSRET erc;

	if (!query[0])
		return TDS_SUCCESS;

	/* submit and parse results */
	erc = tds_submit_query(tds, query);
	if (TDS_SUCCEED(erc))
		erc = tds_parse_login_results(tds, ignore_errors);

	/* prepare next query */
	query[0] = 0;

	if (TDS_FAILED(erc))
		free(query);

	return erc;
}

#define process_single(ignore_errors) do { \
	if (!single_query && TDS_FAILED(erc = tds_process_single(tds, str, ignore_errors))) \
		return erc; \
} while(0)

static TDSRET
tds_setup_connection(TDSSOCKET *tds, TDSLOGIN *login, bool set_db, bool single_query)
{
	TDSRET erc;
	char *str;
	size_t len;
	const char *const product_name = (tds->conn->product_name != NULL ? tds->conn->product_name : "");
	const bool is_sql_anywhere = (strcasecmp(product_name, "SQL Anywhere") == 0);
	const bool is_openserver = (strcasecmp(product_name, "OpenServer") == 0);

	len = 192 + tds_quote_id(tds, NULL, tds_dstr_cstr(&login->database),-1);
	if ((str = tds_new(char, len)) == NULL)
		return TDS_FAIL;

	str[0] = 0;
	if (login->text_size)
		sprintf(str, "SET TEXTSIZE %d\n", login->text_size);
	process_single(false);

	if (tds->conn->spid == -1 && !is_openserver)
		strcat(str, "SELECT @@spid spid\n");
	process_single(true);

	/* Select proper database if specified.
	 * SQL Anywhere does not support multiple databases and USE statement
	 * so don't send the request to avoid connection failures */
	if (set_db && !tds_dstr_isempty(&login->database) && !is_sql_anywhere) {
		strcat(str, "USE ");
		tds_quote_id(tds, strchr(str, 0), tds_dstr_cstr(&login->database), -1);
		strcat(str, "\n");
	}
	process_single(false);

	if (IS_TDS50(tds->conn) && !is_sql_anywhere && !is_openserver) {
		strcat(str, "SELECT CONVERT(NVARCHAR(3), 'abc') nvc\n");
		if (tds->conn->product_version >= TDS_SYB_VER(12, 0, 0))
			strcat(str, "EXECUTE ('SELECT CONVERT(UNIVARCHAR(3), ''xyz'') uvc')\n");
	}
	process_single(true);

	/* nothing to set, just return */
	if (str[0] == 0) {
		free(str);
		return TDS_SUCCESS;
	}

	erc = tds_submit_query(tds, str);
	free(str);
	TDS_PROPAGATE(erc);

	return tds_parse_login_results(tds, false);
}

/**
 * Do a connection to socket
 * @param tds connection structure. This should be a non-connected connection.
 * @return TDS_FAIL or TDS_SUCCESS if a connection was made to the server's port.
 * @return TDSERROR enumerated type if no TCP/IP connection could be formed. 
 * @param login info for login
 * @remark Possible error conditions:
 *		- TDSESOCK: socket(2) failed: insufficient local resources
 * 		- TDSECONN: connect(2) failed: invalid hostname or port (ETIMEDOUT, ECONNREFUSED, ENETUNREACH)
 * 		- TDSEFCON: connect(2) succeeded, login packet not acknowledged.  
 *		- TDS_FAIL: connect(2) succeeded, login failed.  
 */
static TDSRET
tds_connect(TDSSOCKET * tds, TDSLOGIN * login, int *p_oserr)
{
	int erc = -TDSEFCON;
	int connect_timeout = 0;
	bool db_selected = false;
	struct addrinfo *addrs;
	int orig_port;
	bool rerouted = false;
	/* save to restore during redirected connection */
	unsigned int orig_mars = login->mars;

	/*
	 * A major version of 0 means try to guess the TDS version. 
	 * We try them in an order that should work. 
	 */
	static const TDS_USMALLINT versions[] =
		{ 0x704
		, 0x500
		};

	if (!login->valid_configuration) {
		tdserror(tds_get_ctx(tds), tds, TDSECONF, 0);
		return TDS_FAIL;
	}

	if (TDS_MAJOR(login) == 0) {
		unsigned int i;
		TDSSAVECONTEXT save_ctx;
		const TDSCONTEXT *old_ctx = tds_get_ctx(tds);
		typedef void (*env_chg_func_t) (TDSSOCKET * tds, int type, char *oldval, char *newval);
		env_chg_func_t old_env_chg = tds->env_chg_func;

		init_save_context(&save_ctx, old_ctx);
		tds_set_ctx(tds, &save_ctx.ctx);
		tds->env_chg_func = tds_save_env;

		for (i = 0; i < TDS_VECTOR_SIZE(versions); ++i) {
			int orig_size = tds->conn->env.block_size;
			login->tds_version = versions[i];
			reset_save_context(&save_ctx);

			erc = tds_connect(tds, login, p_oserr);
			if (TDS_FAILED(erc)) {
				tds_close_socket(tds);
				if (tds->conn->env.block_size != orig_size)
					tds_realloc_socket(tds, orig_size);
			}
			
			if (erc != -TDSEFCON)	/* TDSEFCON indicates wrong TDS version */
				break;
			if (login->server_is_valid)
				break;
		}
		
		tds->env_chg_func = old_env_chg;
		tds_set_ctx(tds, old_ctx);
		replay_save_context(tds, &save_ctx);
		free_save_context(&save_ctx);
		
		if (TDS_FAILED(erc))
			tdserror(tds_get_ctx(tds), tds, -erc, *p_oserr);

		return erc;
	}
	

	/*
	 * If a dump file has been specified, start logging
	 */
	if (login->dump_file != NULL && !tdsdump_isopen()) {
		if (login->debug_flags)
			tds_debug_flags = login->debug_flags;
		tdsdump_open(login->dump_file);
	}

	tds->login = login;

	tds->conn->tds_version = login->tds_version;

	/* set up iconv if not already initialized*/
	if (tds->conn->char_convs[client2ucs2]->to.cd == (iconv_t) -1) {
		if (!tds_dstr_isempty(&login->client_charset)) {
			if (TDS_FAILED(tds_iconv_open(tds->conn, tds_dstr_cstr(&login->client_charset), login->use_utf16)))
				return -TDSEICONVAVAIL;
		}
	}

	connect_timeout = login->connect_timeout;

	/* Jeff's hack - begin */
	tds->query_timeout = connect_timeout ? connect_timeout : login->query_timeout;
	/* end */

	/* verify that ip_addr is not empty */
	if (login->ip_addrs == NULL) {
		tdserror(tds_get_ctx(tds), tds, TDSEUHST, 0 );
		tdsdump_log(TDS_DBG_ERROR, "IP address pointer is empty\n");
		if (!tds_dstr_isempty(&login->server_name)) {
			tdsdump_log(TDS_DBG_ERROR, "Server %s not found!\n", tds_dstr_cstr(&login->server_name));
		} else {
			tdsdump_log(TDS_DBG_ERROR, "No server specified!\n");
		}
		return -TDSECONN;
	}

	tds->conn->capabilities = login->capabilities;

reroute:
	tds_ssl_deinit(tds->conn);
	erc = TDSEINTF;
	orig_port = login->port;
	for (addrs = login->ip_addrs; addrs != NULL; addrs = addrs->ai_next) {

		/*
		 * By some reasons ftds forms 3 linked tds_addrinfo (addrs
		 * variable here) for one server address. The structures
		 * differs in their ai_socktype and ai_protocol field
		 * values. Typically the combinations are:
		 * ai_socktype     | ai_protocol
		 * -----------------------------
		 * 1 (SOCK_STREAM) | 6  (tcp)
		 * 2 (SOCK_DGRAM)  | 17 (udp)
		 * 3 (SOCK_RAW)    | 0  (ip)
		 *
		 * Later on these fields are not used and dtds always
		 * creates a tcp socket. In case if there is a connection
		 * problem this behavior leads to 3 tries with the provided
		 * timeout which basically multiplies the spent time
		 * without any good result. So it was decided to skip the
		 * non tcp addresses.
		 *
		 * NOTE: on Windows exactly one tds_addrinfo structure is
		 *	 formed and it has 0 in both ai_socktype and
		 *	 ai_protocol fields. So skipping is conditional for
		 *	 non-Windows platforms
		 */
#ifndef _WIN32
		if (addrs->ai_socktype != SOCK_STREAM)
			continue;
#endif

		login->port = orig_port;

		if (!IS_TDS50(tds->conn) && !tds_dstr_isempty(&login->instance_name) && !login->port)
			login->port = tds7_get_instance_port(addrs, tds_dstr_cstr(&login->instance_name));

		if (login->port >= 1) {
			if ((erc = tds_open_socket(tds, addrs, login->port, connect_timeout, p_oserr)) == TDSEOK)
				break;
		} else {
			erc = TDSECONN;
		}
	}

	if (erc != TDSEOK) {
		if (login->port < 1)
			tdsdump_log(TDS_DBG_ERROR, "invalid port number\n");

		tdserror(tds_get_ctx(tds), tds, erc, *p_oserr);
		return -erc;
	}
		
	/*
	 * Beyond this point, we're connected to the server.  We know we have a valid TCP/IP address+socket pair.  
	 * Although network errors *might* happen, most problems from here on out will be TDS-level errors, 
	 * either TDS version problems or authentication problems.  
	 */
		
	tds_set_state(tds, TDS_IDLE);
	tds->conn->spid = -1;

	/* discard possible previous authentication */
	if (tds->conn->authentication) {
		tds->conn->authentication->free(tds->conn, tds->conn->authentication);
		tds->conn->authentication = NULL;
	}

	if (IS_TDS71_PLUS(tds->conn)) {
		erc = tds71_do_login(tds, login);
		db_selected = true;
	} else if (IS_TDS7_PLUS(tds->conn)) {
		erc = tds7_send_login(tds, login);
		db_selected = true;
	} else {
		tds->out_flag = TDS_LOGIN;

		/* SAP ASE 15.0+ SSL mode encrypts entire connection (like stunnel) */
		if (login->encryption_level == TDS_ENCRYPTION_STRICT)
			TDS_PROPAGATE(tds_ssl_init(tds, true));

		erc = tds_send_login(tds, login);
	}
	if (TDS_FAILED(erc) || TDS_FAILED(tds_process_login_tokens(tds))) {
		tdsdump_log(TDS_DBG_ERROR, "login packet %s\n", TDS_SUCCEED(erc)? "accepted":"rejected");
		tds_close_socket(tds);
		tdserror(tds_get_ctx(tds), tds, TDSEFCON, 0); 	/* "TDS server connection failed" */
		return -TDSEFCON;
	}

	/* need to do rerouting */
	if (IS_TDS71_PLUS(tds->conn)
	    && !tds_dstr_isempty(&login->routing_address) && login->routing_port) {
		TDSRET ret;
		char *server_name = NULL;

		tds_close_socket(tds);
		/* only one redirection is allowed */
		if (rerouted) {
			tdserror(tds_get_ctx(tds), tds, TDSEFCON, 0);
			return -TDSEFCON;
		}
		if (asprintf(&server_name, "%s,%d", tds_dstr_cstr(&login->routing_address), login->routing_port) < 0) {
			tdserror(tds_get_ctx(tds), tds, TDSEFCON, 0);
			return -TDSEMEM;
		}
		if (!tds_dstr_set(&login->server_name, server_name)) {
			free(server_name);
			tdserror(tds_get_ctx(tds), tds, TDSEFCON, 0);
			return -TDSEMEM;
		}
		login->mars = orig_mars;
		login->port = login->routing_port;
		ret = tds_lookup_host_set(tds_dstr_cstr(&login->routing_address), &login->ip_addrs);
		login->routing_port = 0;
		tds_dstr_free(&login->routing_address);
		if (TDS_FAILED(ret)) {
			tdserror(tds_get_ctx(tds), tds, TDSEFCON, 0);
			return -TDSEFCON;
		}
		rerouted = true;
		goto reroute;
	}

#if ENABLE_ODBC_MARS
	/* initialize SID */
	if (IS_TDS72_PLUS(tds->conn) && login->mars) {
		TDS72_SMP_HEADER *p;

		tds_extra_assert(tds->sid == 0);
		tds_extra_assert(tds->conn->sessions[0] == tds);
		tds_extra_assert(tds->send_packet != NULL);
		tds_extra_assert(!tds->send_packet->next);

		tds->conn->mars = 1;

		/* start session with a SMP SYN */
		if (TDS_FAILED(tds_append_syn(tds)))
			return -TDSEMEM;

		/* reallocate send_packet */
		if (!tds_realloc_socket(tds, tds->out_buf_max))
			return -TDSEMEM;

		/* start SMP DATA header */
		p = (TDS72_SMP_HEADER *) tds->send_packet->buf;
		p->signature = TDS72_SMP;
		p->type = TDS_SMP_DATA;

		tds_init_write_buf(tds);
	}
#endif

	erc = tds_setup_connection(tds, login, !db_selected, true);
	/* try one query at a time, some servers do not support some queries */
	if (TDS_FAILED(erc))
		erc = tds_setup_connection(tds, login, !db_selected, false);
	TDS_PROPAGATE(erc);

	tds->query_timeout = login->query_timeout;
	tds->login = NULL;
	return TDS_SUCCESS;
}

TDSRET
tds_connect_and_login(TDSSOCKET * tds, TDSLOGIN * login)
{
	int oserr = 0;

	TDS_PROPAGATE(tds8_adjust_login(login));

	return tds_connect(tds, login, &oserr);
}

static void
tds_put_buf(TDSSOCKET * tds, const unsigned char *buf, size_t dsize, size_t ssize)
{
	size_t cpsize;

	cpsize = TDS_MIN(ssize, dsize);
	tds_put_n(tds, buf, cpsize);
	dsize -= cpsize;
	tds_put_n(tds, NULL, dsize);
	TDS_PUT_BYTE(tds, cpsize);
}

static void
tds_put_login_string(TDSSOCKET * tds, const char *buf, size_t n)
{
	const size_t buf_len = buf ? strlen(buf) : 0;
	tds_put_buf(tds, (const unsigned char *) buf, n, buf_len);
}

#define tds_put_login_string(tds, buf, n) do { \
	TDS_COMPILE_CHECK(range, (n) > 0 && (n) < 256); \
	tds_put_login_string(tds, buf, (n)); \
} while(0)

static TDSRET
tds_send_login(TDSSOCKET * tds, const TDSLOGIN * login)
{
	static const unsigned char le1[] = { 0x03, 0x01, 0x06, 0x0a, 0x09, 0x01 };
	static const unsigned char le2[] = { 0x00, 13, 17 };

	/*
	 * capabilities are now part of the tds structure.
	 * unsigned char capabilities[]= {0x01,0x07,0x03,109,127,0xFF,0xFF,0xFF,0xFE,0x02,0x07,0x00,0x00,0x0A,104,0x00,0x00,0x00};
	 */
	/*
	 * This is the original capabilities packet we were working with (sqsh)
	 * unsigned char capabilities[]= {0x01,0x07,0x03,109,127,0xFF,0xFF,0xFF,0xFE,0x02,0x07,0x00,0x00,0x0A,104,0x00,0x00,0x00};
	 * original with 4.x messages
	 * unsigned char capabilities[]= {0x01,0x07,0x03,109,127,0xFF,0xFF,0xFF,0xFE,0x02,0x07,0x00,0x00,0x00,120,192,0x00,0x0D};
	 * This is isql 11.0.3
	 * unsigned char capabilities[]= {0x01,0x07,0x00,96, 129,207, 0xFF,0xFE,62,  0x02,0x07,0x00,0x00,0x00,120,192,0x00,0x0D};
	 * like isql but with 5.0 messages
	 * unsigned char capabilities[]= {0x01,0x07,0x00,96, 129,207, 0xFF,0xFE,62,  0x02,0x07,0x00,0x00,0x00,120,192,0x00,0x00};
	 */

	unsigned char protocol_version[4];
	unsigned char program_version[4];
	unsigned char sec_flags = 0;
	bool use_kerberos = false;

	char blockstr[16];

	TDS_TINYINT encryption_level = login->encryption_level;

	/* override lservname field for ASA servers */	
	const char *lservname = getenv("ASA_DATABASE")? getenv("ASA_DATABASE") : tds_dstr_cstr(&login->server_name);

	if (strchr(tds_dstr_cstr(&login->user_name), '\\') != NULL) {
		tdsdump_log(TDS_DBG_ERROR, "NT login not supported using TDS 4.x or 5.0\n");
		return TDS_FAIL;
	}
	if (tds_dstr_isempty(&login->user_name)) {
		if (!IS_TDS50(tds->conn)) {
			tdsdump_log(TDS_DBG_ERROR, "Kerberos login not supported using TDS 4.x\n");
			return TDS_FAIL;
		}

#ifdef ENABLE_KRB5
		/* try kerberos */
		sec_flags = TDS5_SEC_LOG_SECSESS;
		use_kerberos = true;
		tds->conn->authentication = tds_gss_get_auth(tds);
		if (!tds->conn->authentication)
			return TDS_FAIL;
#else
		tdsdump_log(TDS_DBG_ERROR, "requested GSS authentication but not compiled in\n");
		return TDS_FAIL;
#endif
	}
	if (encryption_level == TDS_ENCRYPTION_DEFAULT)
		encryption_level = TDS_ENCRYPTION_OFF;
	if (!use_kerberos && encryption_level != TDS_ENCRYPTION_OFF) {
		if (!IS_TDS50(tds->conn)) {
			tdsdump_log(TDS_DBG_ERROR, "Encryption not supported using TDS 4.x\n");
			return TDS_FAIL;
		}
		tds->conn->authentication = tds5_negotiate_get_auth(tds);
		if (!tds->conn->authentication)
			return TDS_FAIL;
	}

	if (IS_TDS42(tds->conn)) {
		memcpy(protocol_version, "\004\002\000\000", 4);
		memcpy(program_version, "\004\002\000\000", 4);
	} else if (IS_TDS46(tds->conn)) {
		memcpy(protocol_version, "\004\006\000\000", 4);
		memcpy(program_version, "\004\002\000\000", 4);
	} else if (IS_TDS50(tds->conn)) {
		memcpy(protocol_version, "\005\000\000\000", 4);
		memcpy(program_version, "\005\000\000\000", 4);
	} else {
		tdsdump_log(TDS_DBG_SEVERE, "Unknown protocol version!\n");
		return TDS_FAIL;
	}
	/*
	 * the following code is adapted from  Arno Pedusaar's 
	 * (psaar@fenar.ee) MS-SQL Client. His was a much better way to
	 * do this, (well...mine was a kludge actually) so here's mostly his
	 */

	tds_put_login_string(tds, tds_dstr_cstr(&login->client_host_name), TDS_MAXNAME);	/* client host name */
	tds_put_login_string(tds, tds_dstr_cstr(&login->user_name), TDS_MAXNAME);	/* account name */
	/* account password */
	if (encryption_level != TDS_ENCRYPTION_OFF) {
		tds_put_login_string(tds, NULL, TDS_MAXNAME);
	} else {
		tds_put_login_string(tds, tds_dstr_cstr(&login->password), TDS_MAXNAME);
	}
	sprintf(blockstr, "%d", (int) getpid());
	tds_put_login_string(tds, blockstr, TDS_MAXNAME);	/* host process */
	tds_put_n(tds, le1, 6);
	tds_put_byte(tds, !login->bulk_copy);
	tds_put_n(tds, NULL, 2);
	if (IS_TDS42(tds->conn)) {
		tds_put_int(tds, 512);
	} else {
		tds_put_int(tds, 0);
	}
	tds_put_n(tds, NULL, 3);
	tds_put_login_string(tds, tds_dstr_cstr(&login->app_name), TDS_MAXNAME);
	tds_put_login_string(tds, lservname, TDS_MAXNAME);
	if (IS_TDS42(tds->conn)) {
		tds_put_login_string(tds, tds_dstr_cstr(&login->password), 255);
	} else if (encryption_level != TDS_ENCRYPTION_OFF) {
		tds_put_n(tds, NULL, 256);
	} else {
		size_t len = tds_dstr_len(&login->password);
		if (len > 253)
			len = 0;
		tds_put_byte(tds, 0);
		TDS_PUT_BYTE(tds, len);
		tds_put_n(tds, tds_dstr_cstr(&login->password), len);
		tds_put_n(tds, NULL, 253 - len);
		TDS_PUT_BYTE(tds, len + 2);
	}

	tds_put_n(tds, protocol_version, 4);	/* TDS version; { 0x04,0x02,0x00,0x00 } */
	tds_put_login_string(tds, tds_dstr_cstr(&login->library), TDS_PROGNLEN);	/* client program name */
	if (IS_TDS42(tds->conn)) {
		tds_put_int(tds, 0);
	} else {
		tds_put_n(tds, program_version, 4);	/* program version ? */
	}
	tds_put_n(tds, le2, 3);
	tds_put_login_string(tds, tds_dstr_cstr(&login->language), TDS_MAXNAME);	/* language */
	tds_put_byte(tds, login->suppress_language);

	/* oldsecure(2), should be zero, used by old software */
	tds_put_n(tds, NULL, 2);
	/* seclogin(1) bitmask */
	if (sec_flags == 0 && encryption_level != TDS_ENCRYPTION_OFF)
		sec_flags = TDS5_SEC_LOG_ENCRYPT2|TDS5_SEC_LOG_ENCRYPT3;
	tds_put_byte(tds, sec_flags);
	/* secbulk(1)
	 * halogin(1) type of ha login
	 * hasessionid(6) id of session to reconnect
	 * secspare(2) not used
	 */
	tds_put_n(tds, NULL, 10);

	/* use empty charset to handle conversions on client */
	tds_put_login_string(tds, "", TDS_MAXNAME);	/* charset */
	/* this is a flag, mean that server should use character set provided by client */
	/* TODO notify charset change ?? what's correct meaning ?? -- freddy77 */
	tds_put_byte(tds, 1);

	/* network packet size */
	if (login->block_size < 65536u && login->block_size >= 512)
		sprintf(blockstr, "%d", login->block_size);
	else
		strcpy(blockstr, "512");
	tds_put_login_string(tds, blockstr, TDS_PKTLEN);

	if (IS_TDS42(tds->conn)) {
		tds_put_n(tds, NULL, 8);
	} else if (IS_TDS46(tds->conn)) {
		tds_put_n(tds, NULL, 4);
	} else if (IS_TDS50(tds->conn)) {
		/* just padding to 8 bytes */
		tds_put_n(tds, NULL, 4);

		/* send capabilities */
		tds_put_byte(tds, TDS_CAPABILITY_TOKEN);
		tds_put_smallint(tds, sizeof(tds->conn->capabilities));
		tds_put_n(tds, &tds->conn->capabilities, sizeof(tds->conn->capabilities));
	}

#ifdef ENABLE_KRB5
	if (use_kerberos)
		tds5_gss_send(tds);
#endif

	return tds_flush_packet(tds);
}

/**
 * tds7_send_login() -- Send a TDS 7.0 login packet
 * TDS 7.0 login packet is vastly different and so gets its own function
 * \returns the return value is ignored by the caller. :-/
 */
static TDSRET
tds7_send_login(TDSSOCKET * tds, const TDSLOGIN * login)
{
	static const unsigned char 
		client_progver[] = {   6, 0x83, 0xf2, 0xf8 }, 

		connection_id[] = { 0x00, 0x00, 0x00, 0x00 }, 
		collation[] = { 0x36, 0x04, 0x00, 0x00 };

	enum {
		tds70Version = 0x70000000,
		tds71Version = 0x71000001,
		tds72Version = 0x72090002,
		tds73Version = 0x730B0003,
		tds74Version = 0x74000004,
	};
	TDS_UCHAR sql_type_flag = 0x00;
	TDS_INT time_zone = -120;
	TDS_INT tds7version = tds70Version;

	unsigned int block_size = 4096;
	
	unsigned char option_flag1 = TDS_SET_LANG_ON | TDS_USE_DB_NOTIFY | TDS_INIT_DB_FATAL;
	unsigned char option_flag2 = login->option_flag2;
	unsigned char option_flag3 = 0;

	unsigned char hwaddr[6];
	size_t packet_size, current_pos;
	TDSRET rc;

	void *data = NULL;
	TDSDYNAMICSTREAM data_stream;
	TDSSTATICINSTREAM input;

	const char *user_name = tds_dstr_cstr(&login->user_name);
	unsigned char *pwd;

	/* FIXME: These should be TDS_SMALLINT. */
	size_t user_name_len = strlen(user_name);
	unsigned int auth_len = 0;

	static const char ext_data[] =
		"\x0a\x01\x00\x00\x00\x01"	/* Enable UTF-8 */
		"\xff";
	size_t ext_len = IS_TDS74_PLUS(tds->conn) ? sizeof(ext_data) - 1 : 0;

	/* fields */
	enum {
		HOST_NAME,
		USER_NAME,
		PASSWORD,
		APP_NAME,
		SERVER_NAME,
		EXTENSION,
		LIBRARY_NAME,
		LANGUAGE,
		DATABASE_NAME,
		DB_FILENAME,
		NEW_PASSWORD,
		NUM_DATA_FIELDS
	};
	struct {
		const void *ptr;
		size_t pos, len, limit;
	} data_fields[NUM_DATA_FIELDS], *field;

	tds->out_flag = TDS7_LOGIN;

	current_pos = packet_size = IS_TDS72_PLUS(tds->conn) ? 86 + 8 : 86;	/* ? */

	/* check ntlm */
#ifdef HAVE_SSPI
	if (strchr(user_name, '\\') != NULL || user_name_len == 0) {
		tdsdump_log(TDS_DBG_INFO2, "using SSPI authentication for '%s' account\n", user_name);
		tds->conn->authentication = tds_sspi_get_auth(tds);
		if (!tds->conn->authentication)
			return TDS_FAIL;
		auth_len = tds->conn->authentication->packet_len;
		packet_size += auth_len;
#else
	if (strchr(user_name, '\\') != NULL) {
		tdsdump_log(TDS_DBG_INFO2, "using NTLM authentication for '%s' account\n", user_name);
		tds->conn->authentication = tds_ntlm_get_auth(tds);
		if (!tds->conn->authentication)
			return TDS_FAIL;
		auth_len = tds->conn->authentication->packet_len;
		packet_size += auth_len;
	} else if (user_name_len == 0) {
# ifdef ENABLE_KRB5
		/* try kerberos */
		tdsdump_log(TDS_DBG_INFO2, "using GSS authentication\n");
		tds->conn->authentication = tds_gss_get_auth(tds);
		if (!tds->conn->authentication)
			return TDS_FAIL;
		auth_len = tds->conn->authentication->packet_len;
		packet_size += auth_len;
# else
		tdsdump_log(TDS_DBG_ERROR, "requested GSS authentication but not compiled in\n");
		return TDS_FAIL;
# endif
#endif
	}


	/* initialize ouput buffer for strings */
	TDS_PROPAGATE(tds_dynamic_stream_init(&data_stream, &data, 0));

#define SET_FIELD_DSTR(field, dstr, len_limit) do { \
	data_fields[field].ptr = tds_dstr_cstr(&(dstr)); \
	data_fields[field].len = tds_dstr_len(&(dstr)); \
	data_fields[field].limit = (len_limit) * 2; \
	} while(0)

	/* setup data fields */
	memset(data_fields, 0, sizeof(data_fields));
	SET_FIELD_DSTR(HOST_NAME, login->client_host_name, 128);
	if (!tds->conn->authentication) {
		SET_FIELD_DSTR(USER_NAME, login->user_name, 128);
		SET_FIELD_DSTR(PASSWORD, login->password, 128);
	}
	SET_FIELD_DSTR(APP_NAME, login->app_name, 128);
	SET_FIELD_DSTR(SERVER_NAME, login->server_name, 128);
	SET_FIELD_DSTR(LIBRARY_NAME, login->library, 128);
	SET_FIELD_DSTR(LANGUAGE, login->language, 128);
	SET_FIELD_DSTR(DATABASE_NAME, login->database, 128);
	SET_FIELD_DSTR(DB_FILENAME, login->db_filename, 260);
	if (IS_TDS72_PLUS(tds->conn) && login->use_new_password) {
		option_flag3 |= TDS_CHANGE_PASSWORD;
		SET_FIELD_DSTR(NEW_PASSWORD, login->new_password, 128);
	}
	if (ext_len)
		option_flag3 |= TDS_EXTENSION;

	/* convert data fields */
	for (field = data_fields; field < data_fields + TDS_VECTOR_SIZE(data_fields); ++field) {
		size_t data_pos;

		data_pos = data_stream.size;
		field->pos = current_pos + data_pos;
		if (field->len) {
			tds_staticin_stream_init(&input, field->ptr, field->len);
			rc = tds_convert_stream(tds, tds->conn->char_convs[client2ucs2], to_server, &input.stream, &data_stream.stream);
			if (TDS_FAILED(rc)) {
				free(data);
				return TDS_FAIL;
			}
		} else if (ext_len && field == &data_fields[EXTENSION]) {
			/* reserve 4 bytes in the stream to put the extention offset */
			if (data_stream.stream.write(&data_stream.stream, 4) != 4) {
				free(data);
				return TDS_FAIL;
			}
			field->len = 4;
			continue;
		}
		data_stream.size = TDS_MIN(data_stream.size, data_pos + field->limit);
		data_stream.stream.write(&data_stream.stream, 0);
		field->len = data_stream.size - data_pos;
	}
	pwd = (unsigned char *) data + data_fields[PASSWORD].pos - current_pos;
	tds7_crypt_pass(pwd, data_fields[PASSWORD].len, pwd);
	pwd = (unsigned char *) data + data_fields[NEW_PASSWORD].pos - current_pos;
	tds7_crypt_pass(pwd, data_fields[NEW_PASSWORD].len, pwd);
	packet_size += data_stream.size;
	if (ext_len) {
		packet_size += ext_len;
		pwd = (unsigned char *) data + data_fields[EXTENSION].pos - current_pos;
		TDS_PUT_UA4LE(pwd, current_pos + data_stream.size + auth_len);
	}

#if !defined(TDS_DEBUG_LOGIN)
	tdsdump_log(TDS_DBG_INFO2, "quietly sending TDS 7+ login packet\n");
	do { TDSDUMP_OFF_ITEM off_item;
	tdsdump_off(&off_item);
#endif
	TDS_PUT_INT(tds, packet_size);
	switch (login->tds_version) {
	case 0x700:
		tds7version = tds70Version;
		break;
	case 0x701:
		tds7version = tds71Version;
		break;
	case 0x702:
		tds7version = tds72Version;
		break;
	case 0x703:
		tds7version = tds73Version;
		break;
	case 0x800:
		/* for TDS 8.0 version should be ignored and ALPN used,
		 * practically clients/servers usually set this to 7.4 */
	case 0x704:
		tds7version = tds74Version;
		break;
	default:
		assert(0 && 0x700 <= login->tds_version && login->tds_version <= 0x704);
	}
	
	tds_put_int(tds, tds7version);

	if (4096 <= login->block_size && login->block_size < 65536u)
		block_size = login->block_size;

	tds_put_int(tds, block_size);	/* desired packet size being requested by client */

	if (block_size > tds->out_buf_max)
		tds_realloc_socket(tds, block_size);

	tds_put_n(tds, client_progver, sizeof(client_progver));	/* client program version ? */

	tds_put_int(tds, getpid());	/* process id of this process */

	tds_put_n(tds, connection_id, sizeof(connection_id));

	if (!login->bulk_copy)
		option_flag1 |= TDS_DUMPLOAD_OFF;
		
	tds_put_byte(tds, option_flag1);

	if (tds->conn->authentication)
		option_flag2 |= TDS_INTEGRATED_SECURITY_ON;

	tds_put_byte(tds, option_flag2);

	if (login->readonly_intent && IS_TDS71_PLUS(tds->conn))
		sql_type_flag |= TDS_READONLY_INTENT;
	tds_put_byte(tds, sql_type_flag);

	if (IS_TDS73_PLUS(tds->conn))
		option_flag3 |= TDS_UNKNOWN_COLLATION_HANDLING;
	tds_put_byte(tds, option_flag3);

	tds_put_int(tds, time_zone);
	tds_put_n(tds, collation, sizeof(collation));

#define PUT_STRING_FIELD_PTR(field) do { \
	TDS_PUT_SMALLINT(tds, data_fields[field].pos); \
	TDS_PUT_SMALLINT(tds, data_fields[field].len / 2u); \
	} while(0)

	/* host name */
	PUT_STRING_FIELD_PTR(HOST_NAME);
	if (tds->conn->authentication) {
		tds_put_int(tds, 0);
		tds_put_int(tds, 0);
	} else {
		/* username */
		PUT_STRING_FIELD_PTR(USER_NAME);
		/* password */
		PUT_STRING_FIELD_PTR(PASSWORD);
	}
	/* app name */
	PUT_STRING_FIELD_PTR(APP_NAME);
	/* server name */
	PUT_STRING_FIELD_PTR(SERVER_NAME);
	/* extensions */
	if (ext_len) {
		TDS_PUT_SMALLINT(tds, data_fields[EXTENSION].pos);
		tds_put_smallint(tds, 4);
	} else {
		tds_put_int(tds, 0);
	}
	/* library name */
	PUT_STRING_FIELD_PTR(LIBRARY_NAME);
	/* language  - kostya@warmcat.excom.spb.su */
	PUT_STRING_FIELD_PTR(LANGUAGE);
	/* database name */
	PUT_STRING_FIELD_PTR(DATABASE_NAME);

	/* MAC address */
	tds_getmac(tds_get_s(tds), hwaddr);
	tds_put_n(tds, hwaddr, 6);

	/* authentication stuff */
	TDS_PUT_SMALLINT(tds, current_pos + data_stream.size);
	TDS_PUT_SMALLINT(tds, TDS_MIN(auth_len, 0xffffu));

	/* db file */
	PUT_STRING_FIELD_PTR(DB_FILENAME);

	if (IS_TDS72_PLUS(tds->conn)) {
		/* new password */
		PUT_STRING_FIELD_PTR(NEW_PASSWORD);

		/* SSPI long */
		tds_put_int(tds, auth_len >= 0xffffu ? auth_len : 0);
	}

	tds_put_n(tds, data, data_stream.size);

	if (tds->conn->authentication)
		tds_put_n(tds, tds->conn->authentication->packet, auth_len);

	if (ext_len)
		tds_put_n(tds, ext_data, ext_len);

	rc = tds_flush_packet(tds);

#if !defined(TDS_DEBUG_LOGIN)
	tdsdump_on(&off_item);
	} while(0);
#endif

	free(data);
	return rc;
}

/**
 * tds7_crypt_pass() -- 'encrypt' TDS 7.0 style passwords.
 * the calling function is responsible for ensuring crypt_pass is at least 
 * 'len' characters
 */
static void
tds7_crypt_pass(const unsigned char *clear_pass, size_t len, unsigned char *crypt_pass)
{
	size_t i;

	for (i = 0; i < len; i++)
		crypt_pass[i] = ((clear_pass[i] << 4) | (clear_pass[i] >> 4)) ^ 0xA5;
}

#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
static inline TDS_TINYINT
tds7_get_encryption_byte(TDS_TINYINT encryption_level)
{
	switch (encryption_level) {
	/* The observation working with Microsoft SQL Server is that
	   OFF did not mean off, and you would end up with encryption
	   turned on. Therefore when the freetds.conf says encrypt = off
	   we really want no encryption, and claiming lack of support
	   works for that. Note that the configuration default in this
	   subroutine always been request due to code above that
	   tests for TDS_ENCRYPTION_DEFAULT.
	*/
	case TDS_ENCRYPTION_OFF:
	case TDS_ENCRYPTION_STRICT:
		return TDS7_ENCRYPT_NOT_SUP;
	case TDS_ENCRYPTION_REQUIRE:
		return TDS7_ENCRYPT_ON;
	}
	return TDS7_ENCRYPT_OFF;
}
#endif

static TDSRET
tds71_do_login(TDSSOCKET * tds, TDSLOGIN* login)
{
	int i, pkt_len;
	const char *instance_name = tds_dstr_isempty(&login->instance_name) ? "MSSQLServer" : tds_dstr_cstr(&login->instance_name);
	TDS_USMALLINT instance_name_len = strlen(instance_name) + 1;
	TDS_CHAR crypt_flag;
	unsigned int start_pos = 21;
	TDSRET ret;
	bool mars_replied = false;

#define START_POS 21
#define UI16BE(n) ((n) >> 8), ((n) & 0xffu)
#define SET_UI16BE(i,n) TDS_PUT_UA2BE(&buf[i],n)
	TDS_UCHAR buf[] = {
		/* netlib version */
		0, UI16BE(START_POS), UI16BE(6),
		/* encryption */
		1, UI16BE(START_POS + 6), UI16BE(1),
		/* instance */
		2, UI16BE(START_POS + 6 + 1), UI16BE(0),
		/* process id */
		3, UI16BE(0), UI16BE(4),
		/* MARS enables */
		4, UI16BE(0), UI16BE(1),
		/* end */
		0xff
	};
	static const TDS_UCHAR netlib8[] = { 8, 0, 1, 0x55, 0, 0 };
	static const TDS_UCHAR netlib9[] = { 9, 0, 0,    0, 0, 0 };

	TDS_UCHAR *p;

	TDS_TINYINT encryption_level = login->encryption_level;

	SET_UI16BE(13, instance_name_len);
	if (!IS_TDS72_PLUS(tds->conn)) {
		SET_UI16BE(16, START_POS + 6 + 1 + instance_name_len);
		/* strip MARS setting */
		buf[20] = 0xff;
	} else {
		start_pos += 5;
#undef  START_POS
#define START_POS 26
		SET_UI16BE(1, START_POS);
		SET_UI16BE(6, START_POS + 6);
		SET_UI16BE(11, START_POS + 6 + 1);
		SET_UI16BE(16, START_POS + 6 + 1 + instance_name_len);
		SET_UI16BE(21, START_POS + 6 + 1 + instance_name_len + 4);
	}

	assert(start_pos >= 21 && start_pos <= sizeof(buf));
	assert(buf[start_pos-1] == 0xff);

	if (encryption_level == TDS_ENCRYPTION_DEFAULT)
		encryption_level = TDS_ENCRYPTION_REQUEST;

	/* all encrypted */
	if (encryption_level == TDS_ENCRYPTION_STRICT)
		TDS_PROPAGATE(tds_ssl_init(tds, true));

	/*
	 * fix a problem with mssql2k which doesn't like
	 * packet splitted during SSL handshake
	 */
	if (tds->out_buf_max < 4096)
		tds_realloc_socket(tds, 4096);

	/* do prelogin */
	tds->out_flag = TDS71_PRELOGIN;

	tds_put_n(tds, buf, start_pos);
	/* netlib version */
	tds_put_n(tds, IS_TDS72_PLUS(tds->conn) ? netlib9 : netlib8, 6);
	/* encryption */
#if !defined(HAVE_GNUTLS) && !defined(HAVE_OPENSSL)
	/* not supported */
	tds_put_byte(tds, TDS7_ENCRYPT_NOT_SUP);
#else
	tds_put_byte(tds, tds7_get_encryption_byte(encryption_level));
#endif
	/* instance */
	tds_put_n(tds, instance_name, instance_name_len);
	/* pid */
	tds_put_int(tds, getpid());
	/* MARS (1 enabled) */
	if (IS_TDS72_PLUS(tds->conn))
#if ENABLE_ODBC_MARS
		tds_put_byte(tds, login->mars);
	login->mars = 0;
#else
		tds_put_byte(tds, 0);
#endif
	TDS_PROPAGATE(tds_flush_packet(tds));

	/* now process reply from server */
	ret = tds_read_packet(tds);
	if (ret <= 0 || tds->in_flag != TDS_REPLY)
		return TDS_FAIL;
	login->server_is_valid = 1;
	pkt_len = tds->in_len - tds->in_pos;

	/* the only thing we care is flag */
	p = tds->in_buf + tds->in_pos;
	/* default 2, no certificate, no encryption */
	crypt_flag = TDS7_ENCRYPT_NOT_SUP;
	for (i = 0;; i += 5) {
		TDS_UCHAR type;
		int off, len;

		if (i >= pkt_len)
			return TDS_FAIL;
		type = p[i];
		if (type == 0xff)
			break;
		/* check packet */
		if (i+4 >= pkt_len)
			return TDS_FAIL;
		off = TDS_GET_UA2BE(&p[i+1]);
		len = TDS_GET_UA2BE(&p[i+3]);
		if (off > pkt_len || (off+len) > pkt_len)
			return TDS_FAIL;
		if (type == 1 && len >= 1) {
			crypt_flag = p[off];
		}
		if (IS_TDS72_PLUS(tds->conn) && type == 4 && len >= 1) {
			mars_replied = true;
#if ENABLE_ODBC_MARS
			login->mars = !!p[off];
#endif
		}
	}
	/* we readed all packet */
	tds->in_pos += pkt_len;
	/* TODO some mssql version do not set last packet, update tds according */

	tdsdump_log(TDS_DBG_INFO1, "detected crypt flag %d\n", crypt_flag);

	if (!mars_replied && encryption_level != TDS_ENCRYPTION_STRICT)
		tds->conn->tds_version = 0x701;

	/* if server does not have certificate or TLS already setup do normal login */
	if (crypt_flag == TDS7_ENCRYPT_NOT_SUP || encryption_level == TDS_ENCRYPTION_STRICT) {
		/* unless we wanted encryption and got none, then fail */
		if (encryption_level == TDS_ENCRYPTION_REQUIRE)
			return TDS_FAIL;

		return tds7_send_login(tds, login);
	}

	/*
	 * if server has a certificate it requires at least a crypted login
	 * (even if data is not encrypted)
	 */

	/* here we have to do encryption ... */

	TDS_PROPAGATE(tds_ssl_init(tds, false));

	/* server just encrypt the first packet */
	if (crypt_flag == TDS7_ENCRYPT_OFF)
		tds->conn->encrypt_single_packet = 1;

	ret = tds7_send_login(tds, login);

	/* if flag is TDS7_ENCRYPT_OFF(0) it means that after login server continue not encrypted */
	if (crypt_flag == TDS7_ENCRYPT_OFF || TDS_FAILED(ret))
		tds_ssl_deinit(tds->conn);

	return ret;
}

