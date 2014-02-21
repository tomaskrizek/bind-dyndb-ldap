/*
 * Authors: Martin Nagy <mnagy@redhat.com>
 *          Adam Tkac <atkac@redhat.com>
 *          Jiri Kuncar <jkuncar@redhat.com>
 *
 * Copyright (C) 2008, 2009  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 or later
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <dns/dynamic_db.h>
#include <dns/diff.h>
#include <dns/journal.h>
#include <dns/rbt.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/ttl.h>
#include <dns/view.h>
#include <dns/zone.h>
#include <dns/db.h>
#include <dns/zt.h>
#include <dns/byaddr.h>
#include <dns/forward.h>
#include <dns/soa.h>
#include <dns/update.h>

#include <isc/buffer.h>
#include <isc/dir.h>
#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/region.h>
#include <isc/rwlock.h>
#include <isc/task.h>
#include <isc/thread.h>
#include <isc/time.h>
#include <isc/util.h>
#include <isc/netaddr.h>
#include <isc/parseint.h>
#include <isc/timer.h>
#include <isc/serial.h>
#include <isc/string.h>

#include <alloca.h>
#define LDAP_DEPRECATED 1
#include <ldap.h>
#include <limits.h>
#include <sasl/sasl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <netdb.h>

#include "acl.h"
#include "fs.h"
#include "krb5_helper.h"
#include "ldap_convert.h"
#include "ldap_driver.h"
#include "ldap_entry.h"
#include "ldap_helper.h"
#include "log.h"
#include "rdlist.h"
#include "semaphore.h"
#include "settings.h"
#include "str.h"
#include "syncrepl.h"
#include "util.h"
#include "zone_manager.h"
#include "zone_register.h"
#include "rbt_helper.h"
#include "fwd_register.h"

const enum_txt_assoc_t forwarder_policy_txts[] = {
	{ dns_fwdpolicy_none,	"none"	},
	{ dns_fwdpolicy_first,	"first"	},
	{ dns_fwdpolicy_only,	"only"	},
	{ -1,			NULL	} /* end marker */
};

#define LDAP_OPT_CHECK(r, ...)						\
	do {								\
		if ((r) != LDAP_OPT_SUCCESS) {				\
			log_error(__VA_ARGS__);				\
			goto cleanup;					\
		}							\
	} while (0)

/*
 * LDAP related typedefs and structs.
 */

/*
 * Note about locking in this source.
 *
 * ldap_instance_t structure is equal to dynamic-db {}; statement in named.conf.
 * Attributes in ldap_instance_t are be modified in new_ldap_instance function,
 * which means server is started or reloaded (running single-thread).
 * Before modifying at other places, switch to single-thread mode via
 * isc_task_beginexclusive() and then return back via isc_task_endexclusive()!
 *
 * ldap_connection_t structure represents connection to the LDAP database and
 * per-connection specific data. Access is controlled via
 * ldap_connection_t->lock and ldap_pool_t->conn_semaphore. Each read
 * or write access to ldap_connection_t structure (except create/destroy)
 * must acquire the semaphore and the lock.
 */

typedef struct ldap_qresult	ldap_qresult_t;
typedef struct ldap_connection  ldap_connection_t;
typedef struct ldap_pool	ldap_pool_t;
typedef struct ldap_auth_pair	ldap_auth_pair_t;
typedef struct settings		settings_t;

/* Authentication method. */
typedef enum ldap_auth {
	AUTH_INVALID = 0,
	AUTH_NONE,
	AUTH_SIMPLE,
	AUTH_SASL,
} ldap_auth_t;

struct ldap_auth_pair {
	enum ldap_auth value;	/* Value actually passed to ldap_bind(). */
	char *name;	/* String representation used in configuration file */
};

/* These are typedefed in ldap_helper.h */
struct ldap_instance {
	isc_mem_t		*mctx;

	/* These are needed for zone creation. */
	const char *		db_name;
	dns_view_t		*view;
	dns_zonemgr_t		*zmgr;

	/* Pool of LDAP connections */
	ldap_pool_t		*pool;

	/* Our own list of zones. */
	zone_register_t		*zone_register;
	fwd_register_t		*fwd_register;

	/* krb5 kinit mutex */
	isc_mutex_t		kinit_lock;

	isc_task_t		*task;
	isc_thread_t		watcher;
	isc_boolean_t		exiting;

	/* Settings. */
	settings_set_t		*local_settings;
	settings_set_t		*global_settings;
	dns_forwarders_t	orig_global_forwarders; /* from named.conf */

	sync_ctx_t		*sctx;
};

struct ldap_pool {
	isc_mem_t		*mctx;
	/* List of LDAP connections. */
	unsigned int		connections; /* number of connections */
	semaphore_t		conn_semaphore;
	ldap_connection_t	**conns;

};

struct ldap_connection {
	isc_mem_t		*mctx;
	isc_mutex_t		lock;

	LDAP			*handle;
	int			msgid;

	/* For reconnection logic. */
	isc_time_t		next_reconnect;
	unsigned int		tries;
};

/**
 * Result from single LDAP query.
 */
struct ldap_qresult {
	isc_mem_t		*mctx;
	ld_string_t		*query_string;
	LDAPMessage		*result;
	ldap_entrylist_t	ldap_entries;
};

/*
 * Constants.
 */

extern const char *ldapdb_impname;

/* Supported authentication types. */
const ldap_auth_pair_t supported_ldap_auth[] = {
	{ AUTH_NONE,	"none"		},
	{ AUTH_SIMPLE,	"simple"	},
	{ AUTH_SASL,	"sasl"		},
	{ AUTH_INVALID, NULL		},
};

#define LDAPDB_EVENT_SYNCREPL_UPDATE	(LDAPDB_EVENTCLASS + 1)

typedef struct ldap_syncreplevent ldap_syncreplevent_t;
struct ldap_syncreplevent {
	ISC_EVENT_COMMON(ldap_syncreplevent_t);
	isc_mem_t *mctx;
	char *dbname;
	char *dn;
	char *prevdn;
	int chgtype;
	ldap_entry_t *entry;
};

extern const settings_set_t settings_default_set;

/** Local configuration file */
static const setting_t settings_local_default[] = {
	{ "uri",			no_default_string	},
	{ "connections",		no_default_uint		},
	{ "reconnect_interval",		no_default_uint		},
	{ "timeout",			no_default_uint		},
	{ "cache_ttl",			no_default_string	}, /* No longer supported */
	{ "base",			no_default_string	},
	{ "auth_method",		no_default_string	},
	{ "auth_method_enum",		no_default_uint		},
	{ "bind_dn",			no_default_string	},
	{ "password",			no_default_string	},
	{ "krb5_principal",		no_default_string	},
	{ "sasl_mech",			no_default_string	},
	{ "sasl_user",			no_default_string	},
	{ "sasl_auth_name",		no_default_string	},
	{ "sasl_realm",			no_default_string	},
	{ "sasl_password",		no_default_string	},
	{ "krb5_keytab",		no_default_string	},
	{ "fake_mname",			no_default_string	},
	{ "zone_refresh",		no_default_string	}, /* No longer supported */
	{ "psearch",			no_default_string	}, /* No longer supported */
	{ "ldap_hostname",		no_default_string	},
	{ "sync_ptr",			no_default_boolean	},
	{ "dyn_update",			no_default_boolean	},
	{ "serial_autoincrement",	no_default_string	}, /* No longer supported */
	{ "verbose_checks",		no_default_boolean	},
	{ "directory",			no_default_string	},
	end_of_settings
};

/** Global settings from idnsConfig object. */
static setting_t settings_global_default[] = {
	{ "dyn_update",		no_default_boolean	},
	{ "sync_ptr",		no_default_boolean	},
	end_of_settings
};

/*
 * Forward declarations.
 */

/* TODO: reorganize this stuff & clean it up. */
static isc_result_t new_ldap_connection(ldap_pool_t *pool,
					ldap_connection_t **ldap_connp) ATTR_NONNULLS;
static void destroy_ldap_connection(ldap_connection_t **ldap_connp) ATTR_NONNULLS;

static isc_result_t findrdatatype_or_create(isc_mem_t *mctx,
		ldapdb_rdatalist_t *rdatalist, dns_rdataclass_t rdclass,
		dns_rdatatype_t rdtype, dns_ttl_t ttl, dns_rdatalist_t **rdlistp) ATTR_NONNULLS;
static isc_result_t add_soa_record(isc_mem_t *mctx, dns_name_t *origin,
		ldap_entry_t *entry, ldapdb_rdatalist_t *rdatalist,
		const char *fake_mname) ATTR_NONNULLS;
static isc_result_t parse_rdata(isc_mem_t *mctx, ldap_entry_t *entry,
		dns_rdataclass_t rdclass, dns_rdatatype_t rdtype,
		dns_name_t *origin, const char *rdata_text,
		dns_rdata_t **rdatap) ATTR_NONNULLS;
static isc_result_t
ldap_parse_rrentry(isc_mem_t *mctx, ldap_entry_t *entry, dns_name_t *origin,
		   const char *fake_mname, ldapdb_rdatalist_t *rdatalist);

static isc_result_t ldap_connect(ldap_instance_t *ldap_inst,
		ldap_connection_t *ldap_conn, isc_boolean_t force) ATTR_NONNULLS;
static isc_result_t ldap_reconnect(ldap_instance_t *ldap_inst,
		ldap_connection_t *ldap_conn, isc_boolean_t force) ATTR_NONNULLS;
static isc_result_t handle_connection_error(ldap_instance_t *ldap_inst,
		ldap_connection_t *ldap_conn, isc_boolean_t force) ATTR_NONNULLS;
static isc_result_t ldap_query(ldap_instance_t *ldap_inst, ldap_connection_t *ldap_conn,
		   ldap_qresult_t **ldap_qresultp, const char *base, int scope, char **attrs,
		   int attrsonly, const char *filter, ...) ATTR_NONNULL(1, 3, 4, 8);
static isc_result_t ldap_query_create(isc_mem_t *mctx, ldap_qresult_t **ldap_qresultp) ATTR_NONNULLS;
static void ldap_query_free(isc_boolean_t prepare_reuse, ldap_qresult_t **ldap_qresultp) ATTR_NONNULLS;

/* Functions for writing to LDAP. */
static isc_result_t ldap_modify_do(ldap_instance_t *ldap_inst,
		const char *dn, LDAPMod **mods, isc_boolean_t delete_node) ATTR_NONNULLS;
static isc_result_t ldap_rdttl_to_ldapmod(isc_mem_t *mctx,
		dns_rdatalist_t *rdlist, LDAPMod **changep) ATTR_NONNULLS;
static isc_result_t ldap_rdatalist_to_ldapmod(isc_mem_t *mctx,
		dns_rdatalist_t *rdlist, LDAPMod **changep, int mod_op) ATTR_NONNULLS;

static isc_result_t ldap_rdata_to_char_array(isc_mem_t *mctx,
		dns_rdata_t *rdata_head, char ***valsp) ATTR_NONNULLS;
static void free_char_array(isc_mem_t *mctx, char ***valsp) ATTR_NONNULLS;
static isc_result_t modify_ldap_common(dns_name_t *owner, ldap_instance_t *ldap_inst,
		dns_rdatalist_t *rdlist, int mod_op, isc_boolean_t delete_node) ATTR_NONNULLS;

/* Functions for maintaining pool of LDAP connections */
static isc_result_t ldap_pool_create(isc_mem_t *mctx, unsigned int connections,
		ldap_pool_t **poolp) ATTR_NONNULLS;
static void ldap_pool_destroy(ldap_pool_t **poolp);
static isc_result_t ldap_pool_getconnection(ldap_pool_t *pool,
		ldap_connection_t ** conn) ATTR_NONNULLS;
static void ldap_pool_putconnection(ldap_pool_t *pool,
		ldap_connection_t ** conn) ATTR_NONNULLS;
static isc_result_t ldap_pool_connect(ldap_pool_t *pool,
		ldap_instance_t *ldap_inst) ATTR_NONNULLS;

/* Persistent updates watcher */
static isc_threadresult_t
ldap_syncrepl_watcher(isc_threadarg_t arg) ATTR_NONNULLS;

#define PRINT_BUFF_SIZE 10 /* for unsigned int 2^32 */
isc_result_t
validate_local_instance_settings(ldap_instance_t *inst, settings_set_t *set) {
	isc_result_t result;

	isc_uint32_t uint;
	const char *sasl_mech = NULL;
	const char *sasl_user = NULL;
	const char *sasl_realm = NULL;
	const char *sasl_password = NULL;
	const char *krb5_principal = NULL;
	const char *bind_dn = NULL;
	const char *password = NULL;
	const char *dir_name = NULL;
	isc_boolean_t dir_default;
	ld_string_t *buff = NULL;

	/* handle cache_ttl, psearch, serial_autoincrement, and zone_refresh
	 * in special way */
	const char *obsolete_value = NULL;
	char *obsolete_options[] = {"cache_ttl", "psearch",
				    "serial_autoincrement", "zone_refresh",
				    NULL};

	char print_buff[PRINT_BUFF_SIZE];
	const char *auth_method_str = NULL;
	ldap_auth_t auth_method_enum = AUTH_INVALID;

	if (strlen(inst->db_name) <= 0) {
		log_error("LDAP instance name cannot be empty");
		CLEANUP_WITH(ISC_R_UNEXPECTEDEND);
	}

	/* Use instance name as default working directory */
	CHECK(str_new(inst->mctx, &buff));
	CHECK(setting_get_str("directory", inst->local_settings, &dir_name));
	dir_default = (strcmp(dir_name, "") == 0);
	if (dir_default == ISC_TRUE) {
		CHECK(str_cat_char(buff, "dyndb-ldap/"));
		CHECK(str_cat_char(buff, inst->db_name));
	} else
		CHECK(str_cat_char(buff, dir_name));

	if (str_buf(buff)[str_len(buff) - 1] != '/')
		CHECK(str_cat_char(buff, "/"));

	if (strcmp(dir_name, str_buf(buff)) != 0)
		CHECK(setting_set("directory", inst->local_settings,
				  str_buf(buff), inst->task));
	str_destroy(&buff);
	dir_name = NULL;
	CHECK(setting_get_str("directory", inst->local_settings, &dir_name));

	/* Make sure that working directory exists */
	CHECK(fs_dirs_create(dir_name));

	/* Set timer for deadlock detection inside semaphore_wait_timed . */
	CHECK(setting_get_uint("timeout", set, &uint));
	if (semaphore_wait_timeout.seconds < uint*SEM_WAIT_TIMEOUT_MUL)
		semaphore_wait_timeout.seconds = uint*SEM_WAIT_TIMEOUT_MUL;

	CHECK(setting_get_uint("connections", set, &uint));
	if (uint < 2) {
		log_error("at least two connections are required");
		/* watcher needs one and update_*() requests second connection */
		CLEANUP_WITH(ISC_R_RANGE);
	}

	/* Select authentication method. */
	CHECK(setting_get_str("auth_method", set, &auth_method_str));
	auth_method_enum = AUTH_INVALID;
	for (int i = 0; supported_ldap_auth[i].name != NULL; i++) {
		if (!strcasecmp(auth_method_str, supported_ldap_auth[i].name)) {
			auth_method_enum = supported_ldap_auth[i].value;
			break;
		}
	}
	if (auth_method_enum == AUTH_INVALID) {
		log_error("unknown authentication method '%s'",
			  auth_method_str);
		CLEANUP_WITH(ISC_R_FAILURE);
	}
	CHECK(isc_string_printf(print_buff, PRINT_BUFF_SIZE, "%u", auth_method_enum));
	CHECK(setting_set("auth_method_enum", inst->local_settings, print_buff,
			  inst->task));

	/* check we have the right data when SASL/GSSAPI is selected */
	CHECK(setting_get_str("sasl_mech", set, &sasl_mech));
	CHECK(setting_get_str("krb5_principal", set, &krb5_principal));
	CHECK(setting_get_str("sasl_user", set, &sasl_user));
	CHECK(setting_get_str("sasl_realm", set, &sasl_realm));
	CHECK(setting_get_str("sasl_password", set, &sasl_password));
	CHECK(setting_get_str("bind_dn", set, &bind_dn));
	CHECK(setting_get_str("password", set, &password));

	if (auth_method_enum != AUTH_SIMPLE &&
	   (strlen(bind_dn) != 0 || strlen(password) != 0)) {
		log_error("options 'bind_dn' and 'password' are allowed only "
			  "for auth_method 'simple'");
		CLEANUP_WITH(ISC_R_FAILURE);
	}

	if (auth_method_enum == AUTH_SIMPLE &&
	    (strlen(bind_dn) == 0 || strlen(password) == 0)) {
		log_error("auth_method 'simple' requires 'bind_dn' and 'password'");
		log_info("for anonymous bind please use auth_method 'none'");
		CLEANUP_WITH(ISC_R_FAILURE);
	}

	if (auth_method_enum != AUTH_SASL &&
	   (strlen(sasl_realm) != 0 || strlen(sasl_user) != 0 ||
	    strlen(sasl_password) != 0 || strlen(krb5_principal) != 0)) {
		log_error("options 'sasl_realm', 'sasl_user', 'sasl_password' "
			  "and 'krb5_principal' are effective only with "
			  "auth_method 'sasl'");
		CLEANUP_WITH(ISC_R_FAILURE);
	}

	if ((auth_method_enum == AUTH_SASL) &&
	    (strcasecmp(sasl_mech, "GSSAPI") == 0)) {
		if ((krb5_principal == NULL) || (strlen(krb5_principal) == 0)) {
			if ((sasl_user == NULL) || (strlen(sasl_user) == 0)) {
				char hostname[HOST_NAME_MAX];
				if (gethostname(hostname, HOST_NAME_MAX) != 0) {
					log_error("SASL mech GSSAPI defined "
						  "but krb5_principal and "
						  "sasl_user are empty and"
						  "gethostname() failed");
					CLEANUP_WITH(ISC_R_FAILURE);
				} else {
					CHECK(str_new(inst->mctx, &buff));
					CHECK(str_sprintf(buff,
							  "DNS/%s", hostname));
					log_debug(2, "SASL mech GSSAPI defined "
						  "but krb5_principal and "
						  "sasl_user are empty, using "
						  "default '%s'",
						  str_buf(buff));
					CHECK(setting_set("krb5_principal", set,
							  str_buf(buff),
							  inst->task));
				}
			} else {
				CHECK(setting_set("krb5_principal", set,
						  sasl_user,
						  inst->task));
			}
		}
	} else if (auth_method_enum == AUTH_SASL) {
		log_info("SASL mechanisms other than GSSAPI+Kerberos "
			 "are untested; expect problems");
	}

	for (char **option = obsolete_options; *option != NULL; option++) {
		CHECK(setting_get_str(*option, set, &obsolete_value));
		if (memcmp("", obsolete_value, 1) != 0)
			log_error("option '%s' is not supported, ignoring", *option);
	}

	if (settings_set_isfilled(set) != ISC_TRUE)
		result = ISC_R_FAILURE;

cleanup:
	str_destroy(&buff);
	if (result != ISC_R_SUCCESS)
		log_error_r("LDAP config validation failed for database '%s'",
			    inst->db_name);
	return result;
}
#undef PRINT_BUFF_SIZE

#define PRINT_BUFF_SIZE 255
isc_result_t
new_ldap_instance(isc_mem_t *mctx, const char *db_name,
		  const char * const *argv, dns_dyndb_arguments_t *dyndb_args,
		  isc_task_t *task, ldap_instance_t **ldap_instp)
{
	isc_result_t result;
	ldap_instance_t *ldap_inst;
	dns_view_t *view = NULL;
	dns_forwarders_t *orig_global_forwarders = NULL;
	isc_uint32_t connections;
	char settings_name[PRINT_BUFF_SIZE];

	REQUIRE(ldap_instp != NULL && *ldap_instp == NULL);

	CHECKED_MEM_GET_PTR(mctx, ldap_inst);
	ZERO_PTR(ldap_inst);
	isc_mem_attach(mctx, &ldap_inst->mctx);

	ldap_inst->db_name = db_name;
	view = dns_dyndb_get_view(dyndb_args);
	dns_view_attach(view, &ldap_inst->view);
	ldap_inst->zmgr = dns_dyndb_get_zonemgr(dyndb_args);
	ISC_LIST_INIT(ldap_inst->orig_global_forwarders.addrs);
	ldap_inst->task = task;
	ldap_inst->watcher = 0;
	CHECK(sync_ctx_init(ldap_inst->mctx, task, &ldap_inst->sctx));

	isc_string_printf_truncate(settings_name, PRINT_BUFF_SIZE,
				   SETTING_SET_NAME_LOCAL " for database %s",
				   db_name);
	CHECK(settings_set_create(mctx, settings_local_default,
	      sizeof(settings_local_default), settings_name,
	      &settings_default_set, &ldap_inst->local_settings));

	isc_string_printf_truncate(settings_name, PRINT_BUFF_SIZE,
				   SETTING_SET_NAME_GLOBAL " for database %s",
				   db_name);
	CHECK(settings_set_create(mctx, settings_global_default,
	      sizeof(settings_global_default), settings_name,
	      ldap_inst->local_settings, &ldap_inst->global_settings));

	CHECK(settings_set_fill(ldap_inst->local_settings, argv, task));
	CHECK(validate_local_instance_settings(ldap_inst, ldap_inst->local_settings));
	if (settings_set_isfilled(ldap_inst->global_settings) != ISC_TRUE)
		CLEANUP_WITH(ISC_R_FAILURE);

	CHECK(setting_get_uint("connections", ldap_inst->local_settings, &connections));

	CHECK(zr_create(mctx, ldap_inst, ldap_inst->global_settings,
			&ldap_inst->zone_register));
	CHECK(fwdr_create(ldap_inst->mctx, &ldap_inst->fwd_register));

	CHECK(isc_mutex_init(&ldap_inst->kinit_lock));

	/* copy global forwarders setting for configuration roll back in
	 * configure_zone_forwarders() */
	result = dns_fwdtable_find(ldap_inst->view->fwdtable, dns_rootname,
				   &orig_global_forwarders);
	if (result == ISC_R_SUCCESS) {
		isc_sockaddr_t *addr;
		isc_sockaddr_t *new_addr;
		for (addr = ISC_LIST_HEAD(orig_global_forwarders->addrs);
		     addr != NULL;
		     addr = ISC_LIST_NEXT(addr, link)) {
			CHECKED_MEM_GET_PTR(mctx, new_addr);
			*new_addr = *addr;
			ISC_LINK_INIT(new_addr, link);
			ISC_LIST_APPEND(ldap_inst->orig_global_forwarders.addrs,
					new_addr, link);
		}
		ldap_inst->orig_global_forwarders.fwdpolicy =
				orig_global_forwarders->fwdpolicy;

	} else if (result == ISC_R_NOTFOUND) {
		/* global forwarders are not configured */
		ldap_inst->orig_global_forwarders.fwdpolicy = dns_fwdpolicy_none;
	} else {
		goto cleanup;
	}

	CHECK(ldap_pool_create(mctx, connections, &ldap_inst->pool));
	CHECK(ldap_pool_connect(ldap_inst->pool, ldap_inst));

	/* Start the watcher thread */
	result = isc_thread_create(ldap_syncrepl_watcher, ldap_inst,
				   &ldap_inst->watcher);
	if (result != ISC_R_SUCCESS) {
		log_error("Failed to create syncrepl watcher thread");
		goto cleanup;
	}

cleanup:
	if (result != ISC_R_SUCCESS)
		destroy_ldap_instance(&ldap_inst);
	else
		*ldap_instp = ldap_inst;

	return result;
}
#undef PRINT_BUFF_SIZE

void
destroy_ldap_instance(ldap_instance_t **ldap_instp)
{
	ldap_instance_t *ldap_inst;
	const char *db_name;
	isc_sockaddr_t *addr;

	REQUIRE(ldap_instp != NULL);

	ldap_inst = *ldap_instp;
	if (ldap_inst == NULL)
		return;

	db_name = ldap_inst->db_name; /* points to DB instance: outside ldap_inst */

	if (ldap_inst->watcher != 0) {
		ldap_inst->exiting = ISC_TRUE;
		/*
		 * Wake up the watcher thread. This might look like a hack
		 * but isc_thread_t is actually pthread_t and libisc don't
		 * have any isc_thread_kill() func.
		 *
		 * We use SIGUSR1 to not to interfere with any signal
		 * used by BIND itself.
		 */
		REQUIRE(pthread_kill(ldap_inst->watcher, SIGUSR1) == 0);
		RUNTIME_CHECK(isc_thread_join(ldap_inst->watcher, NULL)
			      == ISC_R_SUCCESS);
		ldap_inst->watcher = 0;
	}

	/* Unregister all zones already registered in BIND. */
	zr_destroy(&ldap_inst->zone_register);
	fwdr_destroy(&ldap_inst->fwd_register);

	ldap_pool_destroy(&ldap_inst->pool);
	dns_view_detach(&ldap_inst->view);

	DESTROYLOCK(&ldap_inst->kinit_lock);

	while (!ISC_LIST_EMPTY(ldap_inst->orig_global_forwarders.addrs)) {
		addr = ISC_LIST_HEAD(ldap_inst->orig_global_forwarders.addrs);
		ISC_LIST_UNLINK(ldap_inst->orig_global_forwarders.addrs, addr, link);
		SAFE_MEM_PUT_PTR(ldap_inst->mctx, addr);
	}

	settings_set_free(&ldap_inst->global_settings);
	settings_set_free(&ldap_inst->local_settings);

	sync_ctx_free(&ldap_inst->sctx);

	MEM_PUT_AND_DETACH(ldap_inst);

	*ldap_instp = NULL;
	log_debug(1, "LDAP instance '%s' destroyed", db_name);
}

static isc_result_t ATTR_NONNULLS
new_ldap_connection(ldap_pool_t *pool, ldap_connection_t **ldap_connp)
{
	isc_result_t result;
	ldap_connection_t *ldap_conn;

	REQUIRE(pool != NULL);
	REQUIRE(ldap_connp != NULL && *ldap_connp == NULL);

	CHECKED_MEM_GET_PTR(pool->mctx, ldap_conn);
	ZERO_PTR(ldap_conn);

	result = isc_mutex_init(&ldap_conn->lock);
	if (result != ISC_R_SUCCESS) {
		SAFE_MEM_PUT_PTR(pool->mctx, ldap_conn);
		return result;
	}

	isc_mem_attach(pool->mctx, &ldap_conn->mctx);

	*ldap_connp = ldap_conn;

	return ISC_R_SUCCESS;

cleanup:
	destroy_ldap_connection(&ldap_conn);

	return result;
}

static void
destroy_ldap_connection(ldap_connection_t **ldap_connp)
{
	ldap_connection_t *ldap_conn;

	REQUIRE(ldap_connp != NULL);

	ldap_conn = *ldap_connp;
	if (ldap_conn == NULL)
		return;

	DESTROYLOCK(&ldap_conn->lock);
	if (ldap_conn->handle != NULL)
		ldap_unbind_ext_s(ldap_conn->handle, NULL, NULL);

	MEM_PUT_AND_DETACH(*ldap_connp);
}

/* Test if the existing zone is 'empty zone' per RFC 6303. */
static isc_boolean_t ATTR_NONNULLS
zone_isempty(isc_mem_t *mctx, dns_zone_t *zone) {
	char **argv = NULL;
	isc_boolean_t result = ISC_FALSE;

	if (dns_zone_getdbtype(zone, &argv, mctx) != ISC_R_SUCCESS)
		CLEANUP_WITH(ISC_FALSE);

	if (argv[0] != NULL && strcmp("_builtin", argv[0]) == 0 &&
	    argv[1] != NULL && strcmp("empty", argv[1]) == 0) {
		result = ISC_TRUE;
	} else {
		result = ISC_FALSE;
	}
	isc_mem_free(mctx, argv);

cleanup:
	return result;
}

/**
 * Delete a zone from plain BIND. LDAP zones require further steps for complete
 * removal, like deletion from zone register etc.
 *
 * @pre A zone pointer has to be attached to *zonep.
 *
 * @returns Values returned by dns_zt_unmount().
 */
static isc_result_t ATTR_NONNULLS
delete_bind_zone(dns_zt_t *zt, dns_zone_t **zonep) {
	dns_zone_t *zone;
	dns_db_t *dbp = NULL;
	dns_zonemgr_t *zmgr;
	isc_result_t result;

	REQUIRE (zonep != NULL && *zonep != NULL);

	zone = *zonep;

	/* Do not unload partially loaded zones, they have uninitialized
	 * structures. */
	if (dns_zone_getdb(zone, &dbp) == ISC_R_SUCCESS) {
		dns_db_detach(&dbp); /* dns_zone_getdb() attaches DB implicitly */
		dns_zone_unload(zone);
		dns_zone_log(zone, ISC_LOG_INFO, "shutting down");
	} else {
		dns_zone_log(zone, ISC_LOG_DEBUG(1), "not loaded - unload skipped");
	}

	result = dns_zt_unmount(zt, zone);
	if (result == ISC_R_NOTFOUND) /* zone wasn't part of a view */
		result = ISC_R_SUCCESS;
	zmgr = dns_zone_getmgr(zone);
	if (zmgr != NULL)
		dns_zonemgr_releasezone(zmgr, zone);
	dns_zone_detach(zonep);

	return result;
}

isc_result_t
cleanup_zone_files(dns_zone_t *zone) {
	isc_result_t result;
	isc_boolean_t failure = ISC_FALSE;
	const char *filename = NULL;
	dns_zone_t *raw = NULL;

	dns_zone_getraw(zone, &raw);
	if (raw != NULL) {
		result = cleanup_zone_files(raw);
		dns_zone_detach(&raw);
		failure = (result != ISC_R_SUCCESS);
	}

	filename = dns_zone_getfile(zone);
	result = fs_file_remove(filename);
	failure = failure || (result != ISC_R_SUCCESS);

	filename = dns_zone_getjournal(zone);
	result = fs_file_remove(filename);
	failure = failure || (result != ISC_R_SUCCESS);

	if (failure == ISC_TRUE)
		dns_zone_log(zone, ISC_LOG_ERROR,
			     "unable to remove files, expect problems");

	if (failure == ISC_TRUE && result == ISC_R_SUCCESS)
		result = ISC_R_FAILURE;

	return result;
}

/**
 * Remove zone files and journal files associated with all zones in ZR.
 */
static isc_result_t
cleanup_files(ldap_instance_t *inst) {
	isc_result_t result;
	rbt_iterator_t *iter = NULL;
	dns_zone_t *zone = NULL;
	DECLARE_BUFFERED_NAME(name);

	INIT_BUFFERED_NAME(name);
	CHECK(zr_rbt_iter_init(inst->zone_register, &iter, &name));
	do {
		CHECK(zr_get_zone_ptr(inst->zone_register, &name, &zone));
		cleanup_zone_files(zone);
		dns_zone_detach(&zone);

		INIT_BUFFERED_NAME(name);
		CHECK(rbt_iter_next(&iter, &name));
	} while (result == ISC_R_SUCCESS);

cleanup:
	if (result == ISC_R_NOTFOUND || result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
	return result;
}

/*
 * Create a new zone with origin 'name'. The zone will be added to the
 * ldap_inst->view.
 */
static isc_result_t ATTR_NONNULLS
create_zone(ldap_instance_t *ldap_inst, dns_name_t *name, dns_zone_t **zonep)
{
	isc_result_t result;
	dns_zone_t *zone = NULL;
	const char *argv[2];
	sync_state_t sync_state;
	isc_task_t *task = NULL;

	REQUIRE(ldap_inst != NULL);
	REQUIRE(name != NULL);
	REQUIRE(zonep != NULL && *zonep == NULL);

	argv[0] = ldapdb_impname;
	argv[1] = ldap_inst->db_name;

	result = dns_view_findzone(ldap_inst->view, name, &zone);
	if (result != ISC_R_NOTFOUND) {
		char zone_name[DNS_NAME_FORMATSIZE];
		dns_name_format(name, zone_name, DNS_NAME_FORMATSIZE);

		if (result != ISC_R_SUCCESS) {
			log_error_r("dns_view_findzone() failed while "
				    "searching for zone '%s'", zone_name);
		} else { /* zone already exists */
			if (zone_isempty(ldap_inst->mctx, zone) == ISC_TRUE) {
				result = delete_bind_zone(ldap_inst->view->zonetable,
							  &zone);
				if (result != ISC_R_SUCCESS)
					log_error_r("failed to create new zone "
						    "'%s': unable to unload "
						    "automatic empty zone",
						    zone_name);
				else
					log_info("automatic empty zone %s "
						 "unloaded", zone_name);

			} else {
				result = ISC_R_EXISTS;
				log_error_r("failed to create new zone '%s'",
					    zone_name);
			}
		}
		if (result != ISC_R_SUCCESS)
			goto cleanup;
	}

	CHECK(dns_zone_create(&zone, ldap_inst->mctx));
	CHECK(dns_zone_setorigin(zone, name));
	dns_zone_setclass(zone, dns_rdataclass_in);
	dns_zone_settype(zone, dns_zone_master);
	CHECK(dns_zone_setdbtype(zone, 2, argv));
	CHECK(dns_zonemgr_managezone(ldap_inst->zmgr, zone));
	sync_state_get(ldap_inst->sctx, &sync_state);
	if (sync_state == sync_init) {
		dns_zone_gettask(zone, &task);
		CHECK(sync_task_add(ldap_inst->sctx, task));
		isc_task_detach(&task);
	}

	*zonep = zone;
	return ISC_R_SUCCESS;

cleanup:
	if (dns_zone_getmgr(zone) != NULL)
		dns_zonemgr_releasezone(ldap_inst->zmgr, zone);
	if (zone != NULL)
		dns_zone_detach(&zone);
	if (task != NULL)
		isc_task_detach(&task);

	return result;
}

static isc_result_t ATTR_NONNULLS
load_zone(dns_zone_t *zone) {
	isc_result_t result;
	isc_boolean_t zone_dynamic;
	isc_uint32_t serial;

	result = dns_zone_load(zone);
	if (result != ISC_R_SUCCESS && result != DNS_R_UPTODATE
	    && result != DNS_R_DYNAMIC && result != DNS_R_CONTINUE)
		goto cleanup;
	zone_dynamic = (result == DNS_R_DYNAMIC);

	CHECK(dns_zone_getserial2(zone, &serial));
	dns_zone_log(zone, ISC_LOG_INFO, "loaded serial %u", serial);

	if (zone_dynamic)
		dns_zone_notify(zone);

cleanup:
	return result;
}

/**
 * Add zone to the view defined in inst->view.
 */
static isc_result_t ATTR_NONNULLS
publish_zone(isc_task_t *task, ldap_instance_t *inst, dns_zone_t *zone)
{
	isc_result_t result;
	isc_boolean_t freeze = ISC_FALSE;
	dns_zone_t *zone_in_view = NULL;
	dns_view_t *view_in_zone = NULL;
	isc_boolean_t unlock = ISC_FALSE;

	REQUIRE(ISCAPI_TASK_VALID(task));
	REQUIRE(inst != NULL);
	REQUIRE(zone != NULL);

	/* Return success if the zone is already in the view as expected. */
	result = dns_view_findzone(inst->view, dns_zone_getorigin(zone),
				   &zone_in_view);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND)
		goto cleanup;

	view_in_zone = dns_zone_getview(zone);
	if (view_in_zone != NULL) {
		/* Zone has a view set -> view should contain the same zone. */
		if (zone_in_view == zone) {
			/* Zone is already published in the right view. */
			CLEANUP_WITH(ISC_R_SUCCESS);
		} else {
			dns_zone_log(zone, ISC_LOG_ERROR, "zone->view doesn't "
				     "match data in the view");
			CLEANUP_WITH(ISC_R_UNEXPECTED);
		}
	} else if (zone_in_view != NULL) {
		dns_zone_log(zone, ISC_LOG_ERROR, "cannot publish zone: view "
			     "already contains another zone with this name");
		CLEANUP_WITH(ISC_R_UNEXPECTED);
	} /* else if (zone_in_view == NULL && view_in_zone == NULL)
	     Publish the zone. */

	result = isc_task_beginexclusive(task);
	RUNTIME_CHECK(result == ISC_R_SUCCESS ||
		      result == ISC_R_LOCKBUSY);
	unlock = (result == ISC_R_SUCCESS);

	if (inst->view->frozen) {
		freeze = ISC_TRUE;
		dns_view_thaw(inst->view);
	}

	dns_zone_setview(zone, inst->view);
	CHECK(dns_view_addzone(inst->view, zone));

cleanup:
	if (zone_in_view != NULL)
		dns_zone_detach(&zone_in_view);
	if (freeze)
		dns_view_freeze(inst->view);
	if (unlock)
		isc_task_endexclusive(task);

	return result;
}

/**
 * Add all zones in zone register to DNS view specified in inst->view
 * and load zones.
 */
isc_result_t
activate_zones(isc_task_t *task, ldap_instance_t *inst) {
	isc_result_t result;
	isc_boolean_t loaded;
	rbt_iterator_t *iter = NULL;
	dns_zone_t *zone = NULL;
	DECLARE_BUFFERED_NAME(name);
	unsigned int published_cnt = 0;
	unsigned int total_cnt = 0;

	INIT_BUFFERED_NAME(name);
	CHECK(zr_rbt_iter_init(inst->zone_register, &iter, &name));
	do {
		++total_cnt;
		CHECK(zr_get_zone_ptr(inst->zone_register, &name, &zone));
		/*
		 * Don't bother if load fails, server will return
		 * SERVFAIL for queries beneath this zone. This is
		 * admin's problem.
		 */
		result = load_zone(zone);
		loaded = (result == ISC_R_SUCCESS);
		if (loaded == ISC_FALSE)
			dns_zone_log(zone, ISC_LOG_ERROR,
				     "unable to load zone: %s",
				     dns_result_totext(result));

		result = publish_zone(task, inst, zone);
		if (result != ISC_R_SUCCESS)
			dns_zone_log(zone, ISC_LOG_ERROR,
				     "cannot add zone to view: %s",
				     dns_result_totext(result));
		else if (loaded == ISC_TRUE)
			++published_cnt;
		dns_zone_detach(&zone);

		INIT_BUFFERED_NAME(name);
		CHECK(rbt_iter_next(&iter, &name));
	} while (result == ISC_R_SUCCESS);

cleanup:
	log_info("%u zones from LDAP instance '%s' loaded (%u zones defined)",
		 published_cnt, inst->db_name, total_cnt);
	return result;
}


static isc_result_t ATTR_NONNULLS
configure_zone_acl(isc_mem_t *mctx, dns_zone_t *zone,
		void (acl_setter)(dns_zone_t *zone, dns_acl_t *acl),
		const char *aclstr, acl_type_t type) {
	isc_result_t result;
	isc_result_t result2;
	dns_acl_t *acl = NULL;
	const char *type_txt = NULL;

	result = acl_from_ldap(mctx, aclstr, type, &acl);
	if (result != ISC_R_SUCCESS) {
		result2 = get_enum_description(acl_type_txts, type, &type_txt);
		if (result2 != ISC_R_SUCCESS) {
			log_bug("invalid acl type %u", type);
			type_txt = "<unknown>";
		}

		dns_zone_logc(zone, DNS_LOGCATEGORY_SECURITY, ISC_LOG_ERROR,
			      "%s policy is invalid: %s; configuring most "
			      "restrictive %s policy as possible",
			      type_txt, isc_result_totext(result), type_txt);
		result2 = acl_from_ldap(mctx, "", type, &acl);
		if (result2 != ISC_R_SUCCESS) {
			dns_zone_logc(zone, DNS_LOGCATEGORY_SECURITY, ISC_LOG_CRITICAL,
				      "cannot configure restrictive %s policy: %s",
				      type_txt, isc_result_totext(result2));
			FATAL_ERROR(__FILE__, __LINE__,
				    "insecure state detected");
		}
	}
	acl_setter(zone, acl);

	if (acl != NULL)
		dns_acl_detach(&acl);

	return result;
}

/* In BIND9 terminology "ssu" means "Simple Secure Update" */
static isc_result_t ATTR_NONNULLS
configure_zone_ssutable(dns_zone_t *zone, const char *update_str)
{
	isc_result_t result;
	isc_result_t result2;

	REQUIRE(zone != NULL);

	/*
	 * This is meant only for debugging.
	 * DANGEROUS: Do not leave uncommented!
	 */
#if 0 
	{
		dns_acl_t *any;
		dns_acl_any(dns_zone_getmctx(zone), &any);
		dns_zone_setupdateacl(zone, any);
		dns_acl_detach(&any);
	}

	return ISC_R_SUCCESS;
#endif

	/* Set simple update table. */
	result = acl_configure_zone_ssutable(update_str, zone);
	if (result != ISC_R_SUCCESS) {
		dns_zone_logc(zone, DNS_LOGCATEGORY_SECURITY, ISC_LOG_ERROR,
			      "disabling all updates because of error in "
			      "update policy configuration: %s",
			      isc_result_totext(result));
		result2 = acl_configure_zone_ssutable("", zone);
		if (result2 != ISC_R_SUCCESS) {
			dns_zone_logc(zone, DNS_LOGCATEGORY_SECURITY, ISC_LOG_CRITICAL,
				      "cannot disable all updates: %s",
				      isc_result_totext(result2));
			FATAL_ERROR(__FILE__, __LINE__,
				    "insecure state detected");
		}
	}

	return result;
}

static isc_result_t ATTR_NONNULLS
delete_forwarding_table(ldap_instance_t *inst, dns_name_t *name,
			const char *msg_obj_type, const char *dn) {
	isc_result_t result;

	result = dns_fwdtable_delete(inst->view->fwdtable, name);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
		log_error_r("%s '%s': failed to delete forwarders",
			    msg_obj_type, dn);
		return result;
	} else {
		return ISC_R_SUCCESS; /* ISC_R_NOTFOUND = nothing to delete */
	}
}

/* Delete zone by dns zone name */
isc_result_t
ldap_delete_zone2(ldap_instance_t *inst, dns_name_t *name, isc_boolean_t lock,
		  isc_boolean_t preserve_forwarding)
{
	isc_result_t result;
	isc_result_t isforward = ISC_R_NOTFOUND;
	isc_boolean_t unlock = ISC_FALSE;
	isc_boolean_t freeze = ISC_FALSE;
	dns_zone_t *zone = NULL;
	dns_zone_t *foundzone = NULL;
	char zone_name_char[DNS_NAME_FORMATSIZE];

	dns_name_format(name, zone_name_char, DNS_NAME_FORMATSIZE);
	log_debug(1, "deleting zone '%s'", zone_name_char);
	if (lock) {
		result = isc_task_beginexclusive(inst->task);
		RUNTIME_CHECK(result == ISC_R_SUCCESS ||
			      result == ISC_R_LOCKBUSY);
		if (result == ISC_R_SUCCESS)
			unlock = ISC_TRUE;
	}

	if (!preserve_forwarding) {
		CHECK(delete_forwarding_table(inst, name, "zone",
					      zone_name_char));
		isforward = fwdr_zone_ispresent(inst->fwd_register, name);
		if (isforward == ISC_R_SUCCESS)
			CHECK(fwdr_del_zone(inst->fwd_register, name));
	}

	result = zr_get_zone_ptr(inst->zone_register, name, &zone);
	if (result == ISC_R_NOTFOUND || result == DNS_R_PARTIALMATCH) {
		if (isforward == ISC_R_SUCCESS)
			log_info("forward zone '%s': shutting down", zone_name_char);
		log_debug(1, "zone '%s' not found in zone register", zone_name_char);
		result = dns_view_flushcache(inst->view);
		goto cleanup;
	} else if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = dns_view_findzone(inst->view, name, &foundzone);
	if (result == ISC_R_SUCCESS) {
		/* foundzone != zone indicates a bug */
		RUNTIME_CHECK(foundzone == zone);
		dns_zone_detach(&foundzone);

		if (lock) {
			dns_view_thaw(inst->view);
			freeze = ISC_TRUE;
		}
	} /* else: zone wasn't in a view */

	CHECK(delete_bind_zone(inst->view->zonetable, &zone));
	CHECK(zr_del_zone(inst->zone_register, name));

cleanup:
	if (freeze)
		dns_view_freeze(inst->view);
	if (unlock)
		isc_task_endexclusive(inst->task);

	return result;
}

/* Delete zone */
static isc_result_t ATTR_NONNULLS
ldap_delete_zone(ldap_instance_t *inst, const char *dn, isc_boolean_t lock,
		 isc_boolean_t preserve_forwarding)
{
	isc_result_t result;
	dns_name_t name;
	dns_name_init(&name, NULL);
	
	CHECK(dn_to_dnsname(inst->mctx, dn, &name, NULL));

	result = ldap_delete_zone2(inst, &name, lock, preserve_forwarding);

cleanup:
	if (dns_name_dynamic(&name))
		dns_name_free(&name, inst->mctx);

	return result;
}

/**
 * Read forwarding policy (from idnsForwardingPolicy attribute) and
 * list of forwarders (from idnsForwarders multi-value attribute)
 * and update forwarding settings for given zone.
 *
 * Enable forwarding if forwarders are specified and policy is not 'none'.
 * Disable forwarding if forwarding policy is 'none' or list of forwarders
 * is empty.
 *
 * Invalid forwarders are skipped, forwarding will be enabled if at least
 * one valid forwarder is defined. Global forwarders will be used if all
 * defined forwarders are invalid or list of forwarders is not present at all.
 *
 * @retval ISC_R_SUCCESS  Forwarding was enabled.
 * @retval ISC_R_DISABLED Forwarding was disabled.
 * @retval ISC_R_UNEXPECTEDTOKEN Forwarding policy is invalid
 *                               or all specified forwarders are invalid.
 * @retval ISC_R_NOMEMORY
 * @retval others	  Some RBT manipulation errors including ISC_R_FAILURE.
 */
static isc_result_t ATTR_NONNULLS
configure_zone_forwarders(ldap_entry_t *entry, ldap_instance_t *inst, 
                          dns_name_t *name)
{
	const char *dn = entry->dn;
	isc_result_t result;
	isc_result_t orig_result;
	ldap_valuelist_t values;
	ldap_value_t *value;
	isc_sockaddrlist_t addrs;
	isc_boolean_t is_global_config;
	isc_boolean_t fwdtbl_deletion_requested = ISC_TRUE;
	isc_boolean_t fwdtbl_update_requested = ISC_FALSE;
	dns_forwarders_t *old_setting = NULL;
	dns_fixedname_t foundname;
	dns_zone_t *zone = NULL;
	const char *msg_use_global_fwds;
	const char *msg_obj_type;
	const char *msg_forwarders_not_def;
	const char *msg_forward_policy = NULL;
	/**
	 * BIND forward policies are "first" (default) or "only".
	 * We invented option "none" which disables forwarding for zone
	 * regardless idnsForwarders attribute and global forwarders.
	 */
	dns_fwdpolicy_t fwdpolicy = dns_fwdpolicy_first;

	REQUIRE(entry != NULL && inst != NULL && name != NULL);
	ISC_LIST_INIT(addrs);
	dns_fixedname_init(&foundname);
	if (dns_name_equal(name, dns_rootname)) {
		is_global_config = ISC_TRUE;
		msg_obj_type = "global configuration";
		msg_use_global_fwds = "; global forwarders will be disabled";
		msg_forwarders_not_def = "; global forwarders from "
					 "configuration file will be used";
	} else {
		is_global_config = ISC_FALSE;
		msg_obj_type = "zone";
		msg_use_global_fwds = "; global forwarders will be used "
				      "(if they are configured)";
		msg_forwarders_not_def = msg_use_global_fwds;
	}

	/*
	 * Fetch forward policy.
	 */
	result = ldap_entry_getvalues(entry, "idnsForwardPolicy", &values);
	if (result == ISC_R_SUCCESS) {
		value = HEAD(values);
		if (value != NULL && value->value != NULL) {
			if (strcasecmp(value->value, "only") == 0)
				fwdpolicy = dns_fwdpolicy_only;
			else if (strcasecmp(value->value, "first") == 0)
				fwdpolicy = dns_fwdpolicy_first;
			else if (strcasecmp(value->value, "none") == 0)
				fwdpolicy = dns_fwdpolicy_none;
			else {
				log_error("%s '%s': invalid value '%s' in "
					  "idnsForwardPolicy attribute; "
					  "valid values: first, only, none"
					  "%s",
					  msg_obj_type, dn, value->value,
					  msg_use_global_fwds);
				CLEANUP_WITH(ISC_R_UNEXPECTEDTOKEN);
			}
		}
	}

	if (fwdpolicy == dns_fwdpolicy_none) {
		ISC_LIST_INIT(values); /* ignore idnsForwarders in LDAP */
	} else {
		result = ldap_entry_getvalues(entry, "idnsForwarders", &values);
		if (result == ISC_R_NOTFOUND || EMPTY(values)) {
			log_debug(5, "%s '%s': idnsForwarders attribute is "
				  "not present%s", msg_obj_type, dn,
				  msg_forwarders_not_def);
			if (is_global_config) {
				ISC_LIST_INIT(values);
				addrs = inst->orig_global_forwarders.addrs;
				fwdpolicy = inst->orig_global_forwarders.fwdpolicy;
			} else {
				CLEANUP_WITH(ISC_R_DISABLED);
			}
		}
	}

	CHECK(get_enum_description(forwarder_policy_txts, fwdpolicy,
				   &msg_forward_policy));
	log_debug(5, "%s '%s': forward policy is '%s'", msg_obj_type, dn,
		  msg_forward_policy);

	for (value = HEAD(values); value != NULL; value = NEXT(value, link)) {
		isc_sockaddr_t *addr = NULL;
		char forwarder_txt[ISC_SOCKADDR_FORMATSIZE];

		if (acl_parse_forwarder(value->value, inst->mctx, &addr)
				!= ISC_R_SUCCESS) {
			log_error("%s '%s': could not parse forwarder '%s'",
					msg_obj_type, dn, value->value);
			continue;
		}

		ISC_LINK_INIT(addr, link);
		ISC_LIST_APPEND(addrs, addr, link);
		isc_sockaddr_format(addr, forwarder_txt, ISC_SOCKADDR_FORMATSIZE);
		log_debug(5, "%s '%s': adding forwarder '%s'", msg_obj_type,
			  dn, forwarder_txt);
	}

	if (fwdpolicy != dns_fwdpolicy_none && ISC_LIST_EMPTY(addrs)) {
		log_debug(5, "%s '%s': all idnsForwarders are invalid%s",
			  msg_obj_type, dn, msg_use_global_fwds);
		CLEANUP_WITH(ISC_R_UNEXPECTEDTOKEN);
	} else if (fwdpolicy == dns_fwdpolicy_none) {
		log_debug(5, "%s '%s': forwarding explicitly disabled "
			  "(policy 'none', ignoring global forwarders)",
			  msg_obj_type, dn);
	}

	/* Check for old and new forwarding settings equality. */
	result = dns_fwdtable_find2(inst->view->fwdtable, name,
				    dns_fixedname_name(&foundname),
				    &old_setting);
	if (result == ISC_R_SUCCESS &&
	   (dns_name_equal(name, dns_fixedname_name(&foundname)) == ISC_TRUE)) {
		isc_sockaddr_t *s1, *s2;

		if (fwdpolicy != old_setting->fwdpolicy)
			fwdtbl_update_requested = ISC_TRUE;

		/* Check address lists item by item. */
		for (s1 = ISC_LIST_HEAD(addrs), s2 = ISC_LIST_HEAD(old_setting->addrs);
		     s1 != NULL && s2 != NULL && !fwdtbl_update_requested;
		     s1 = ISC_LIST_NEXT(s1, link), s2 = ISC_LIST_NEXT(s2, link))
			if (!isc_sockaddr_equal(s1, s2))
				fwdtbl_update_requested = ISC_TRUE;

		if (!fwdtbl_update_requested && ((s1 != NULL) || (s2 != NULL)))
			fwdtbl_update_requested = ISC_TRUE;
	} else {
		fwdtbl_update_requested = ISC_TRUE;
		if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND)
			log_error_r("%s '%s': can't obtain old forwarding "
				    "settings", msg_obj_type, dn);
	}

	if (fwdtbl_update_requested) {
		/* Shutdown automatic empty zone if it is present. */
		result = dns_zt_find(inst->view->zonetable, name, 0, NULL,
				     &zone);
		if (result == ISC_R_SUCCESS || result == DNS_R_PARTIALMATCH) {
			if (zone_isempty(inst->mctx, zone)) {
				dns_zone_log(zone, ISC_LOG_INFO, "automatic "
					     "empty zone will be shut down "
					     "to enable forwarding");
				result = delete_bind_zone(inst->view->zonetable,
							  &zone);
			} else {
				dns_zone_detach(&zone);
				result = ISC_R_SUCCESS;
			}
		}
		if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND)
			goto cleanup;

		/* Something was changed - set forward table up. */
		CHECK(delete_forwarding_table(inst, name, msg_obj_type, dn));
		result = dns_fwdtable_add(inst->view->fwdtable, name, &addrs, fwdpolicy);
		if (result != ISC_R_SUCCESS)
			log_error_r("%s '%s': forwarding table update failed",
				    msg_obj_type, dn);
	} else {
		result = ISC_R_SUCCESS;
		log_debug(5, "%s '%s': forwarding table unmodified",
			  msg_obj_type, dn);
	}
	if (result == ISC_R_SUCCESS) {
		fwdtbl_deletion_requested = ISC_FALSE;
		if (fwdpolicy == dns_fwdpolicy_none)
			result = ISC_R_DISABLED;
	}

cleanup:
	if (ISC_LIST_HEAD(addrs) !=
	    ISC_LIST_HEAD(inst->orig_global_forwarders.addrs)) {
		while(!ISC_LIST_EMPTY(addrs)) {
			isc_sockaddr_t *addr = NULL;
			addr = ISC_LIST_HEAD(addrs);
			ISC_LIST_UNLINK(addrs, addr, link);
			SAFE_MEM_PUT_PTR(inst->mctx, addr);
		}
	}
	if (fwdtbl_deletion_requested) {
		orig_result = result;
		result = delete_forwarding_table(inst, name, msg_obj_type, dn);
		if (result == ISC_R_SUCCESS)
			result = orig_result;
	}
	if (fwdtbl_deletion_requested || fwdtbl_update_requested) {
		log_debug(5, "%s '%s': forwarder table was updated: %s",
			  msg_obj_type, dn, dns_result_totext(result));
		orig_result = result;
		result = dns_view_flushcache(inst->view);
		if (result == ISC_R_SUCCESS)
			result = orig_result;
	}
	return result;
}

/* Parse the config object entry */
static isc_result_t ATTR_NONNULLS
ldap_parse_configentry(ldap_entry_t *entry, ldap_instance_t *inst)
{
	isc_result_t result;

	/* BIND functions are thread safe, ldap instance 'inst' is locked
	 * inside setting* functions. */

	log_debug(3, "Parsing configuration object");

	/* idnsForwardPolicy change is handled by configure_zone_forwarders() */
	result = configure_zone_forwarders(entry, inst, dns_rootname);
	if (result != ISC_R_SUCCESS && result != ISC_R_DISABLED) {
		log_error_r("global forwarder could not be set up");
	}

	result = setting_update_from_ldap_entry("dyn_update",
						inst->global_settings,
						"idnsAllowDynUpdate",
						entry, inst->task);
	if (result != ISC_R_SUCCESS && result != ISC_R_IGNORE)
		goto cleanup;

	result = setting_update_from_ldap_entry("sync_ptr",
						inst->global_settings,
						"idnsAllowSyncPTR",
						entry, inst->task);
	if (result != ISC_R_SUCCESS && result != ISC_R_IGNORE)
		goto cleanup;

cleanup:
	/* Configuration errors are not fatal. */
	/* TODO: log something? */
	return ISC_R_SUCCESS;
}

/* Parse the forward zone entry */
static isc_result_t ATTR_NONNULLS
ldap_parse_fwd_zoneentry(ldap_entry_t *entry, ldap_instance_t *inst)
{
	const char *dn;
	dns_name_t name;
	char name_txt[DNS_NAME_FORMATSIZE];
	isc_result_t result;

	REQUIRE(entry != NULL);
	REQUIRE(inst != NULL);

	dns_name_init(&name, NULL);

	/* Derive the DNS name of the zone from the DN. */
	dn = entry->dn;
	CHECK(dn_to_dnsname(inst->mctx, dn, &name, NULL));

	result = configure_zone_forwarders(entry, inst, &name);
	if (result != ISC_R_DISABLED && result != ISC_R_SUCCESS) {
		log_error_r("forward zone '%s': could not configure forwarding", dn);
		goto cleanup;
	}

	result = fwdr_zone_ispresent(inst->fwd_register, &name);
	if (result == ISC_R_NOTFOUND) {
		CHECK(fwdr_add_zone(inst->fwd_register, &name));
		dns_name_format(&name, name_txt, DNS_NAME_FORMATSIZE);
		log_info("forward zone '%s': loaded", name_txt);
	}
	else if (result != ISC_R_SUCCESS)
		log_error_r("forward zone '%s': could not read forwarding register", dn);

cleanup:
	if (dns_name_dynamic(&name))
		dns_name_free(&name, inst->mctx);

	return result;
}

/**
 * Add all RRs from rdataset to the diff. Create strictly minimal diff.
 */
static isc_result_t
rdataset_to_diff(isc_mem_t *mctx, dns_diffop_t op, dns_name_t *name,
		dns_rdataset_t *rds, dns_diff_t *diff) {
	dns_difftuple_t *tp = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	dns_rdata_t rdata;

	for (result = dns_rdataset_first(rds);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rds)) {
		dns_rdata_init(&rdata);
		dns_rdataset_current(rds, &rdata);
		CHECK(dns_difftuple_create(mctx, op, name, rds->ttl, &rdata,
					   &tp));
		dns_diff_appendminimal(diff, &tp);
	}

cleanup:
	return result;
}

/**
 * Add all RRs from rdatalist to the diff. Create strictly minimal diff.
 */
static isc_result_t
rdatalist_to_diff(isc_mem_t *mctx, dns_diffop_t op, dns_name_t *name,
		  dns_rdatalist_t *rdatalist, dns_diff_t *diff) {
	dns_difftuple_t *tp = NULL;
	isc_result_t result = ISC_R_SUCCESS;

	for (dns_rdata_t *rd = HEAD(rdatalist->rdata);
			  rd != NULL;
			  rd = NEXT(rd, link)) {
		CHECK(dns_difftuple_create(mctx, op, name, rdatalist->ttl, rd,
					   &tp));
		dns_diff_appendminimal(diff, &tp);
	}

cleanup:
	return result;
}

/**
 * Compute minimal diff between rdatalist and rdataset iterator. This produces
 * minimal diff applicable to a database.
 */
static isc_result_t
diff_ldap_rbtdb(isc_mem_t *mctx, dns_name_t *name, ldapdb_rdatalist_t *ldap_rdatalist,
		    dns_rdatasetiter_t *rbt_rds_iter, dns_diff_t *diff) {
	isc_result_t result;
	dns_rdataset_t rbt_rds;
	dns_rdatalist_t *l;

	dns_rdataset_init(&rbt_rds);

	/* FIXME: rbt_rds_iter == NULL || ldap_rdatalist == NULL */
	for (result = dns_rdatasetiter_first(rbt_rds_iter);
	     result == ISC_R_SUCCESS;
	     result = dns_rdatasetiter_next(rbt_rds_iter)) {
		dns_rdatasetiter_current(rbt_rds_iter, &rbt_rds);
		result = rdataset_to_diff(mctx, DNS_DIFFOP_DEL, name, &rbt_rds,
					  diff);
		if (result != ISC_R_SUCCESS && result != ISC_R_NOMORE)
			goto cleanup;
		dns_rdataset_disassociate(&rbt_rds);
	}

	for (l = HEAD(*ldap_rdatalist);
	     l != NULL;
	     l = NEXT(l, link)) {
		result = rdatalist_to_diff(mctx, DNS_DIFFOP_ADD, name, l, diff);
		if (result != ISC_R_SUCCESS && result != ISC_R_NOMORE)
			goto cleanup;
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

cleanup:
	return result;
}

static isc_result_t ATTR_NONNULLS
configure_paths(isc_mem_t *mctx, ldap_instance_t *inst, dns_zone_t *zone,
		isc_boolean_t issecure) {
	isc_result_t result;
	ld_string_t *file_name = NULL;

	CHECK(zr_get_zone_path(mctx, ldap_instance_getsettings_local(inst),
			       dns_zone_getorigin(zone),
			       (issecure ? "signed" : "raw"), &file_name));
	CHECK(dns_zone_setfile(zone, str_buf(file_name)));
	CHECK(fs_file_remove(dns_zone_getfile(zone)));
	CHECK(fs_file_remove(dns_zone_getjournal(zone)));

cleanup:
	str_destroy(&file_name);
	return result;
}

/**
 * Process strictly minimal diff and detect if data were changed
 * and return latest SOA RR.
 *
 * @pre Input diff has to be minimal, i.e. it can't contain DEL & ADD operation
 *      for the same data under the same name and TTL.
 *
 * @pre If the tuple list contains SOA RR, then exactly one SOA RR deletion
 *      has to precede exactly one SOA RR addition.
 *      (Each SOA RR deletion has to have matching addition.)
 *
 * @param[in]	diff		Input diff. List of tuples can be empty.
 * @param[out]	soa_latest	Pointer to last added SOA RR from tuple list.
 *				Result can be NULL if there is no added SOA RR
 *				in the tuple list.
 * @param[out]	data_changed	ISC_TRUE if any data other than SOA serial were
 * 				changed. ISC_FALSE if nothing (except SOA
 * 				serial) was changed.
 *
 */
static isc_result_t ATTR_NONNULLS
diff_analyze_serial(dns_diff_t *diff, dns_difftuple_t **soa_latest,
		    isc_boolean_t *data_changed) {
	dns_difftuple_t *t = NULL;
	dns_rdata_t *del_soa = NULL; /* last seen SOA with op == DEL */
	dns_difftuple_t *tmp_tuple = NULL; /* tuple used for SOA comparison */
	isc_result_t result = ISC_R_SUCCESS;
	int ret;

	REQUIRE(DNS_DIFF_VALID(diff));
	REQUIRE(soa_latest != NULL && *soa_latest == NULL);
	REQUIRE(data_changed != NULL);

	*data_changed = ISC_FALSE;
	for (t = HEAD(diff->tuples);
	     t != NULL;
	     t = NEXT(t, link)) {
		INSIST(tmp_tuple == NULL);
		if (t->rdata.type != dns_rdatatype_soa)
			*data_changed = ISC_TRUE;
		else { /* SOA is always special case */
			if (t->op == DNS_DIFFOP_DEL ||
			    t->op == DNS_DIFFOP_DELRESIGN) {
				/* delete operation has to precede add */
				INSIST(del_soa == NULL);
				del_soa = &t->rdata;
			} else if (t->op == DNS_DIFFOP_ADD ||
				   t->op == DNS_DIFFOP_ADDRESIGN) {
				/* add operation has to follow a delete */
				*soa_latest = t;

				/* we are adding SOA without preceding delete
				 * -> we are initializing new empty zone */
				if (del_soa == NULL) {
					*data_changed = ISC_TRUE;
				} else if (*data_changed == ISC_FALSE) {
					/* detect if fields other than serial
					 * were changed (compute only if necessary) */
					CHECK(dns_difftuple_copy(t, &tmp_tuple));
					dns_soa_setserial(dns_soa_getserial(del_soa),
							  &tmp_tuple->rdata);
					ret = dns_rdata_compare(del_soa,
								&tmp_tuple->rdata);
					*data_changed = ISC_TF(ret != 0);
				}
				if (tmp_tuple != NULL)
					dns_difftuple_free(&tmp_tuple);
				/* re-start the SOA delete-add search cycle */
				del_soa = NULL;
			} else {
				INSIST("unexpected diff: op != ADD || DEL"
				       == NULL);
			}
		}
	}
	/* SOA deletions & additions has to create self-contained couples */
	INSIST(del_soa == NULL && tmp_tuple == NULL);

cleanup:
	if (tmp_tuple != NULL)
		dns_difftuple_free(&tmp_tuple);
	return result;
}

/**
 * Increment SOA serial in given diff tuple and return new numeric value.
 *
 * @pre Soa_tuple operation is ADD or ADDRESIGN and RR type is SOA.
 *
 * @param[in]		method
 * @param[in,out]	soa_tuple	Latest SOA RR in diff.
 * @param[out]		new_serial	SOA serial after incrementation.
 */
static isc_result_t
update_soa_serial(dns_updatemethod_t method, dns_difftuple_t *soa_tuple,
		  isc_uint32_t *new_serial) {
	isc_uint32_t serial;

	REQUIRE(DNS_DIFFTUPLE_VALID(soa_tuple));
	REQUIRE(soa_tuple->op == DNS_DIFFOP_ADD ||
		soa_tuple->op == DNS_DIFFOP_ADDRESIGN);
	REQUIRE(soa_tuple->rdata.type == dns_rdatatype_soa);
	REQUIRE(new_serial != NULL);

	serial = dns_soa_getserial(&soa_tuple->rdata);
	serial = dns_update_soaserial(serial, method);
	dns_soa_setserial(serial, &soa_tuple->rdata);
	*new_serial = serial;

	return ISC_R_SUCCESS;
}

/**
 * Replace SOA serial in LDAP for given zone.
 *
 * @param[in]	inst
 * @param[in]	zone	Zone name.
 * @param[in]	serial	New serial.
 *
 */
static isc_result_t ATTR_NONNULLS
ldap_replace_serial(ldap_instance_t *inst, dns_name_t *zone,
		    isc_uint32_t serial) {
	isc_result_t result;
#define MAX_SERIAL_LENGTH sizeof("4294967295") /* SOA serial is isc_uint32_t */
	char serial_char[MAX_SERIAL_LENGTH];
	char *values[2] = { serial_char, NULL };
	LDAPMod change;
	LDAPMod *changep[2] = { &change, NULL };
	ld_string_t *dn = NULL;

	REQUIRE(inst != NULL);

	CHECK(str_new(inst->mctx, &dn));
	CHECK(dnsname_to_dn(inst->zone_register, zone, dn));

	change.mod_op = LDAP_MOD_REPLACE;
	change.mod_type = "idnsSOAserial";
	change.mod_values = values;
	CHECK(isc_string_printf(serial_char, MAX_SERIAL_LENGTH, "%u", serial));

	CHECK(ldap_modify_do(inst, str_buf(dn), changep, ISC_FALSE));

cleanup:
	str_destroy(&dn);
	return result;
#undef MAX_SERIAL_LENGTH
}

/* Parse the master zone entry */
static isc_result_t ATTR_NONNULLS
ldap_parse_master_zoneentry(ldap_entry_t *entry, ldap_instance_t *inst,
			    isc_task_t *task)
{
	const char *dn;
	ldap_valuelist_t values;
	dns_name_t name;
	dns_zone_t *zone = NULL;
	dns_zone_t *zone_raw = NULL;
	isc_result_t result;
	isc_boolean_t unlock = ISC_FALSE;
	isc_boolean_t new_zone = ISC_FALSE;
	isc_boolean_t configured = ISC_FALSE;
	isc_boolean_t ssu_changed;
	ldapdb_rdatalist_t rdatalist;
	settings_set_t *zone_settings = NULL;
	const char *fake_mname = NULL;
	isc_boolean_t data_changed;
	isc_boolean_t ldap_writeback;
	isc_uint32_t curr_serial;
	isc_uint32_t new_serial;

	dns_db_t *rbtdb = NULL;
	dns_db_t *ldapdb = NULL;
	dns_diff_t diff;
	dns_dbversion_t *version = NULL;
	/* RBTDB's origin node cannot be detached until the node is non-empty.
	 * This is workaround for possible bug in bind-9.9.3-P2. */
	dns_dbnode_t *node = NULL;
	dns_rdatasetiter_t *rbt_rds_iterator = NULL;
	dns_difftuple_t *soa_tuple = NULL;
	isc_boolean_t soa_tuple_alloc = ISC_FALSE;

	sync_state_t sync_state;
	dns_journal_t *journal = NULL;
	char *journal_filename = NULL;

	dns_diff_init(inst->mctx, &diff);

	REQUIRE(entry != NULL);
	REQUIRE(inst != NULL);

	dns_name_init(&name, NULL);
	INIT_LIST(rdatalist);

	/* Derive the dns name of the zone from the DN. */
	dn = entry->dn;
	CHECK(dn_to_dnsname(inst->mctx, dn, &name, NULL));

	result = isc_task_beginexclusive(task);
	RUNTIME_CHECK(result == ISC_R_SUCCESS || result == ISC_R_LOCKBUSY);
	if (result == ISC_R_SUCCESS)
		unlock = ISC_TRUE;

	/*
	 * TODO: Remove this hack, most probably before Fedora 20.
	 * Forwarding has top priority hence when the forwarders are properly
	 * set up all others attributes are ignored.
	 */
	result = configure_zone_forwarders(entry, inst, &name);
	if (result != ISC_R_DISABLED) {
		if (result == ISC_R_SUCCESS) {
			/* forwarding was enabled for the zone
			 * => zone type was changed to "forward"
			 * => delete "master" zone */
			CHECK(ldap_delete_zone2(inst, &name, ISC_FALSE,
						ISC_TRUE));
		}
		/* DO NOT CHANGE ANYTHING ELSE after forwarders are set up! */
		goto cleanup;
	}
	/* No forwarders are used. Zone was removed from fwdtable.
	 * Load the zone. */

	/* Check if we are already serving given zone */
	result = zr_get_zone_ptr(inst->zone_register, &name, &zone);
	if (result == ISC_R_NOTFOUND || result == DNS_R_PARTIALMATCH) {
		CHECK(create_zone(inst, &name, &zone));
		CHECK(configure_paths(inst->mctx, inst, zone, ISC_FALSE));
		CHECK(zr_add_zone(inst->zone_register, zone, dn));
		new_zone = ISC_TRUE;
		log_debug(2, "created zone %p: %s", zone, dn);
	} else if (result != ISC_R_SUCCESS)
		goto cleanup;

	CHECK(zr_get_zone_settings(inst->zone_register, &name, &zone_settings));

	result = setting_update_from_ldap_entry("dyn_update", zone_settings,
				       "idnsAllowDynUpdate", entry, inst->task);
	if (result != ISC_R_SUCCESS && result != ISC_R_IGNORE)
		goto cleanup;
	ssu_changed = (result == ISC_R_SUCCESS);

	result = setting_update_from_ldap_entry("sync_ptr", zone_settings,
				       "idnsAllowSyncPTR", entry, inst->task);
	if (result != ISC_R_SUCCESS && result != ISC_R_IGNORE)
		goto cleanup;

	result = setting_update_from_ldap_entry("update_policy", zone_settings,
				       "idnsUpdatePolicy", entry, inst->task);
	if (result != ISC_R_SUCCESS && result != ISC_R_IGNORE)
		goto cleanup;

	if (result == ISC_R_SUCCESS || ssu_changed) {
		isc_boolean_t ssu_enabled;
		const char *ssu_policy = NULL;

		log_debug(2, "Setting SSU table for %p: %s", zone, dn);
		CHECK(setting_get_bool("dyn_update", zone_settings, &ssu_enabled));
		if (ssu_enabled) {
			/* Get the update policy and update the zone with it. */
			CHECK(setting_get_str("update_policy", zone_settings,
					      &ssu_policy));
			CHECK(configure_zone_ssutable(zone, ssu_policy));
		} else {
			/* Empty policy will prevent the update from reaching
			 * LDAP driver and error will be logged. */
			CHECK(configure_zone_ssutable(zone, ""));
		}
	}

	/* Fetch allow-query and allow-transfer ACLs */
	log_debug(2, "Setting allow-query for %p: %s", zone, dn);
	result = ldap_entry_getvalues(entry, "idnsAllowQuery", &values);
	if (result == ISC_R_SUCCESS) {
		CHECK(configure_zone_acl(inst->mctx, zone, &dns_zone_setqueryacl,
					 HEAD(values)->value, acl_type_query));
	} else {
		log_debug(2, "allow-query not set");
		dns_zone_clearqueryacl(zone);
	}

	log_debug(2, "Setting allow-transfer for %p: %s", zone, dn);
	result = ldap_entry_getvalues(entry, "idnsAllowTransfer", &values);
	if (result == ISC_R_SUCCESS) {
		CHECK(configure_zone_acl(inst->mctx, zone, &dns_zone_setxfracl,
					 HEAD(values)->value, acl_type_transfer));
	} else {
		log_debug(2, "allow-transfer not set");
		dns_zone_clearxfracl(zone);
	}

	sync_state_get(inst->sctx, &sync_state);
	if (new_zone == ISC_TRUE && sync_state == sync_finished)
		CHECK(publish_zone(task, inst, zone));

	/* synchronize zone origin with LDAP */
	CHECK(setting_get_str("fake_mname", inst->local_settings,
			      &fake_mname));
	CHECK(ldap_parse_rrentry(inst->mctx, entry, &name, fake_mname,
				 &rdatalist));

	CHECK(zr_get_zone_dbs(inst->zone_register, &name, &ldapdb, &rbtdb));
	CHECK(dns_db_newversion(ldapdb, &version));
	CHECK(dns_db_getoriginnode(rbtdb, &node));
	result = dns_db_allrdatasets(rbtdb, node, version, 0,
				     &rbt_rds_iterator);
	if (result == ISC_R_SUCCESS) {
		CHECK(diff_ldap_rbtdb(inst->mctx, &name, &rdatalist,
				      rbt_rds_iterator, &diff));
		dns_rdatasetiter_destroy(&rbt_rds_iterator);
	} else if (result != ISC_R_NOTFOUND)
		goto cleanup;

	/* New zone doesn't have serial defined yet. */
	if (new_zone == ISC_FALSE)
		CHECK(dns_db_getsoaserial(rbtdb, version, &curr_serial));

	/* Detect if SOA serial is affected by the update or not.
	 * Always bump serial in case of re-synchronization. */
	CHECK(diff_analyze_serial(&diff, &soa_tuple, &data_changed));
	if (data_changed == ISC_TRUE || sync_state != sync_finished) {
		if (soa_tuple == NULL) {
			/* The diff doesn't contain new SOA serial
			 * => generate new serial and write it back to LDAP. */
			ldap_writeback = ISC_TRUE;
			soa_tuple_alloc = ISC_TRUE;
			CHECK(dns_db_createsoatuple(ldapdb, version, inst->mctx,
						    DNS_DIFFOP_DEL, &soa_tuple));
			dns_diff_appendminimal(&diff, &soa_tuple);
			CHECK(dns_db_createsoatuple(ldapdb, version, inst->mctx,
						    DNS_DIFFOP_ADD, &soa_tuple));
			CHECK(update_soa_serial(dns_updatemethod_unixtime,
						soa_tuple, &new_serial));
			dns_diff_appendminimal(&diff, &soa_tuple);
		} else if (new_zone == ISC_TRUE || sync_state != sync_finished ||
			   isc_serial_le(dns_soa_getserial(&soa_tuple->rdata),
					 curr_serial)) {
			/* The diff tries to send SOA serial back!
			 * => generate new serial and write it back to LDAP.
			 * Force serial update if we are adding a new zone. */
			ldap_writeback = ISC_TRUE;
			CHECK(update_soa_serial(dns_updatemethod_unixtime,
						soa_tuple, &new_serial));
		} else {
			/* The diff contains new serial already
			 * => do nothing. */
			ldap_writeback = ISC_FALSE;
		}

	} else {/* if (data_changed == ISC_FALSE) */
		ldap_writeback = ISC_FALSE;
		if (soa_tuple == NULL) {
			/* The diff is empty => do nothing. */
			INSIST(EMPTY(diff.tuples));
		} else if (isc_serial_le(dns_soa_getserial(&soa_tuple->rdata),
					 curr_serial)) {
			/* Attempt to move serial backwards without any data
			 * => ignore it. */
			dns_diff_clear(&diff);
		}/* else:
		  * The diff contains new serial already
		  * => do nothing. */
	}

#if RBTDB_DEBUG >= 2
	dns_diff_print(&diff, stdout);
#else
	dns_diff_print(&diff, NULL);
#endif
	if (ldap_writeback == ISC_TRUE) {
		dns_zone_log(zone, ISC_LOG_DEBUG(5), "writing new zone serial "
			     "%u to LDAP", new_serial);
		result = ldap_replace_serial(inst, &name, new_serial);
		if (result != ISC_R_SUCCESS)
			dns_zone_log(zone, ISC_LOG_ERROR,
				     "serial (%u) write back to LDAP failed",
				     new_serial);
	}

	if (!EMPTY(diff.tuples)) {
		if (sync_state == sync_finished && new_zone == ISC_FALSE) {
			/* write the transaction to journal */
			dns_zone_getraw(zone, &zone_raw);
			if (zone_raw == NULL)
				journal_filename = dns_zone_getjournal(zone);
			else
				journal_filename = dns_zone_getjournal(zone_raw);
			CHECK(dns_journal_open(inst->mctx, journal_filename,
					       DNS_JOURNAL_CREATE, &journal));
			CHECK(dns_journal_write_transaction(journal, &diff));
		}

		/* commit */
		CHECK(dns_diff_apply(&diff, rbtdb, version));
		dns_db_closeversion(ldapdb, &version, ISC_TRUE);
	}

	/* Make sure that zone has at least SOA record. */
	if (new_zone == ISC_FALSE
	    || (data_changed == ISC_TRUE && soa_tuple != NULL))
		configured = ISC_TRUE;

	/* Do zone load only if the initial LDAP synchronization is done. */
	if (sync_state == sync_finished && data_changed == ISC_TRUE)
		CHECK(load_zone(zone));

cleanup:
	dns_diff_clear(&diff);
	if (soa_tuple_alloc == ISC_TRUE && soa_tuple != NULL)
		dns_difftuple_free(&soa_tuple);
	if (rbt_rds_iterator != NULL)
		dns_rdatasetiter_destroy(&rbt_rds_iterator);
	if (node != NULL)
		dns_db_detachnode(rbtdb, &node);
	if (rbtdb != NULL && version != NULL)
		dns_db_closeversion(ldapdb, &version, ISC_FALSE); /* rollback */
	if (rbtdb != NULL)
		dns_db_detach(&rbtdb);
	if (journal != NULL)
		dns_journal_destroy(&journal);
	if (ldapdb != NULL)
		dns_db_detach(&ldapdb);
	if (new_zone && !configured) { /* Failure in ACL parsing or so. */
		log_error_r("zone '%s': publishing failed, rolling back due to",
			    entry->dn);
		result = ldap_delete_zone2(inst, &name, ISC_TRUE, ISC_FALSE);
		if (result != ISC_R_SUCCESS)
			log_error_r("zone '%s': rollback failed: ", entry->dn);
	}
	if (unlock)
		isc_task_endexclusive(task);
	if (dns_name_dynamic(&name))
		dns_name_free(&name, inst->mctx);
	if (zone != NULL)
		dns_zone_detach(&zone);
	ldapdb_rdatalist_destroy(inst->mctx, &rdatalist);

	return result;
}

static isc_result_t
findrdatatype_or_create(isc_mem_t *mctx, ldapdb_rdatalist_t *rdatalist,
			dns_rdataclass_t rdclass, dns_rdatatype_t rdtype,
			dns_ttl_t ttl, dns_rdatalist_t **rdlistp)
{
	isc_result_t result;
	dns_rdatalist_t *rdlist = NULL;

	REQUIRE(rdatalist != NULL);
	REQUIRE(rdlistp != NULL);

	*rdlistp = NULL;

	result = ldapdb_rdatalist_findrdatatype(rdatalist, rdtype, &rdlist);
	if (result != ISC_R_SUCCESS) {
		CHECKED_MEM_GET_PTR(mctx, rdlist);

		dns_rdatalist_init(rdlist);
		rdlist->rdclass = rdclass;
		rdlist->type = rdtype;
		rdlist->ttl = ttl;
		APPEND(*rdatalist, rdlist, link);
	} else {
		/*
		 * No support for different TTLs yet.
		 */
		if (rdlist->ttl != ttl) {
			log_error("different TTLs in single rdata list "
				  "are not supported");
			CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
		}
	}

	*rdlistp = rdlist;
	return ISC_R_SUCCESS;

cleanup:
	SAFE_MEM_PUT_PTR(mctx, rdlist);

	return result;
}

/*
 * ldapdb_rdatalist_t related functions.
 */
isc_result_t
ldapdb_rdatalist_findrdatatype(ldapdb_rdatalist_t *rdatalist,
			       dns_rdatatype_t rdtype,
			       dns_rdatalist_t **rdlistp)
{
	dns_rdatalist_t *rdlist;

	REQUIRE(rdatalist != NULL);
	REQUIRE(rdlistp != NULL && *rdlistp == NULL);

	rdlist = HEAD(*rdatalist);
	while (rdlist != NULL && rdlist->type != rdtype) {
		rdlist = NEXT(rdlist, link);
	}

	*rdlistp = rdlist;

	return (rdlist == NULL) ? ISC_R_NOTFOUND : ISC_R_SUCCESS;
}

void
ldapdb_rdatalist_destroy(isc_mem_t *mctx, ldapdb_rdatalist_t *rdatalist)
{
	dns_rdatalist_t *rdlist;

	REQUIRE(rdatalist != NULL);

	while (!EMPTY(*rdatalist)) {
		rdlist = HEAD(*rdatalist);
		free_rdatalist(mctx, rdlist);
		UNLINK(*rdatalist, rdlist, link);
		SAFE_MEM_PUT_PTR(mctx, rdlist);
	}
}

void
free_rdatalist(isc_mem_t *mctx, dns_rdatalist_t *rdlist)
{
	dns_rdata_t *rdata;
	isc_region_t r;

	REQUIRE(rdlist != NULL);

	while (!EMPTY(rdlist->rdata)) {
		rdata = HEAD(rdlist->rdata);
		UNLINK(rdlist->rdata, rdata, link);
		dns_rdata_toregion(rdata, &r);
		isc_mem_put(mctx, r.base, r.length);
		SAFE_MEM_PUT_PTR(mctx, rdata);
	}
}
/**
 * @param rdatalist[in,out] Has to be empty initialized list.
 */
static isc_result_t
ldap_parse_rrentry(isc_mem_t *mctx, ldap_entry_t *entry, dns_name_t *origin,
		   const char *fake_mname, ldapdb_rdatalist_t *rdatalist)
{
	isc_result_t result;
	dns_rdataclass_t rdclass;
	ldap_entryclass_t objclass;
	dns_ttl_t ttl;
	dns_rdatatype_t rdtype;
	dns_rdata_t *rdata = NULL;
	dns_rdatalist_t *rdlist = NULL;
	ldap_attribute_t *attr;
	const char *dn = "<NULL entry>";
	const char *data_str = "<NULL data>";
	ld_string_t *data_buf = NULL;

	REQUIRE(EMPTY(*rdatalist));

	CHECK(str_new(mctx, &data_buf));
	CHECK(ldap_entry_getclass(entry, &objclass));
	if ((objclass & LDAP_ENTRYCLASS_MASTER) != 0)
		CHECK(add_soa_record(mctx, origin, entry, rdatalist, fake_mname));

	rdclass = ldap_entry_getrdclass(entry);
	ttl = ldap_entry_getttl(entry);

	for (result = ldap_entry_firstrdtype(entry, &attr, &rdtype);
	     result == ISC_R_SUCCESS;
	     result = ldap_entry_nextrdtype(entry, &attr, &rdtype)) {

		CHECK(findrdatatype_or_create(mctx, rdatalist, rdclass,
					      rdtype, ttl, &rdlist));
		for (result = ldap_attr_firstvalue(attr, data_buf);
		     result == ISC_R_SUCCESS;
		     result = ldap_attr_nextvalue(attr, data_buf)) {
			CHECK(parse_rdata(mctx, entry, rdclass,
					  rdtype, origin,
					  str_buf(data_buf), &rdata));
			APPEND(rdlist->rdata, rdata, link);
			rdata = NULL;
		}
		if (result != ISC_R_NOMORE)
			goto cleanup;
		rdlist = NULL;
	}
	if (result != ISC_R_NOMORE)
		goto cleanup;

	str_destroy(&data_buf);
	return ISC_R_SUCCESS;

cleanup:
	if (entry != NULL)
		dn = entry->dn;
	if (data_buf != NULL && str_len(data_buf) != 0)
		data_str = str_buf(data_buf);
	log_error_r("failed to parse RR entry: dn '%s': data '%s'", dn, data_str);
	str_destroy(&data_buf);
	return result;
}

isc_result_t
ldapdb_rdatalist_get(isc_mem_t *mctx, ldap_instance_t *ldap_inst, dns_name_t *name,
		     dns_name_t *origin, ldapdb_rdatalist_t *rdatalist)
{
	isc_result_t result;
	ldap_qresult_t *ldap_qresult = NULL;
	ldap_entry_t *entry;
	ld_string_t *string = NULL;
	const char *fake_mname = NULL;

	REQUIRE(ldap_inst != NULL);
	REQUIRE(name != NULL);
	REQUIRE(rdatalist != NULL);

	/* Perform ordinary LDAP query */
	INIT_LIST(*rdatalist);
	CHECK(str_new(mctx, &string));
	CHECK(dnsname_to_dn(ldap_inst->zone_register, name, string));

	CHECK(ldap_query(ldap_inst, NULL, &ldap_qresult, str_buf(string),
			 LDAP_SCOPE_BASE, NULL, 0, "(objectClass=idnsRecord)"));

	if (EMPTY(ldap_qresult->ldap_entries)) {
		result = ISC_R_NOTFOUND;
		goto cleanup;
	}

	CHECK(setting_get_str("fake_mname", ldap_inst->local_settings,
			      &fake_mname));
	for (entry = HEAD(ldap_qresult->ldap_entries);
		entry != NULL;
		entry = NEXT(entry, link)) {
		CHECK(ldap_parse_rrentry(mctx, entry, origin, fake_mname,
					 rdatalist));
	}

	if (EMPTY(*rdatalist))
		result = ISC_R_NOTFOUND;

cleanup:
	ldap_query_free(ISC_FALSE, &ldap_qresult);
	str_destroy(&string);

	if (result != ISC_R_SUCCESS)
		ldapdb_rdatalist_destroy(mctx, rdatalist);

	return result;
}

static isc_result_t
add_soa_record(isc_mem_t *mctx, dns_name_t *origin,
	       ldap_entry_t *entry, ldapdb_rdatalist_t *rdatalist,
	       const char *fake_mname)
{
	isc_result_t result;
	ld_string_t *string = NULL;
	dns_rdataclass_t rdclass;
	dns_rdata_t *rdata = NULL;
	dns_rdatalist_t *rdlist = NULL;

	CHECK(str_new(mctx, &string));

	CHECK(ldap_entry_getfakesoa(entry, fake_mname, string));
	rdclass = ldap_entry_getrdclass(entry);
	CHECK(parse_rdata(mctx, entry, rdclass, dns_rdatatype_soa, origin,
			  str_buf(string), &rdata));

	CHECK(findrdatatype_or_create(mctx, rdatalist, rdclass, dns_rdatatype_soa,
				      ldap_entry_getttl(entry), &rdlist));

	APPEND(rdlist->rdata, rdata, link);

cleanup:
	str_destroy(&string);
	if (result != ISC_R_SUCCESS)
		SAFE_MEM_PUT_PTR(mctx, rdata);

	return result;
}

static isc_result_t
parse_rdata(isc_mem_t *mctx, ldap_entry_t *entry,
	    dns_rdataclass_t rdclass, dns_rdatatype_t rdtype,
	    dns_name_t *origin, const char *rdata_text, dns_rdata_t **rdatap)
{
	isc_result_t result;
	isc_consttextregion_t text;
	isc_buffer_t lex_buffer;
	isc_region_t rdatamem;
	dns_rdata_t *rdata;

	REQUIRE(entry != NULL);
	REQUIRE(rdata_text != NULL);
	REQUIRE(rdatap != NULL);

	rdata = NULL;
	rdatamem.base = NULL;

	text.base = rdata_text;
	text.length = strlen(text.base);

	isc_buffer_init(&lex_buffer, (char *)text.base, text.length);
	isc_buffer_add(&lex_buffer, text.length);
	isc_buffer_setactive(&lex_buffer, text.length);

	CHECK(isc_lex_openbuffer(entry->lex, &lex_buffer));

	isc_buffer_init(&entry->rdata_target, entry->rdata_target_mem,
			DNS_RDATA_MAXLENGTH);
	CHECK(dns_rdata_fromtext(NULL, rdclass, rdtype, entry->lex, origin,
				 0, mctx, &entry->rdata_target, NULL));

	CHECKED_MEM_GET_PTR(mctx, rdata);
	dns_rdata_init(rdata);

	rdatamem.length = isc_buffer_usedlength(&entry->rdata_target);
	CHECKED_MEM_GET(mctx, rdatamem.base, rdatamem.length);

	memcpy(rdatamem.base, isc_buffer_base(&entry->rdata_target),
	       rdatamem.length);
	dns_rdata_fromregion(rdata, rdclass, rdtype, &rdatamem);

	isc_lex_close(entry->lex);

	*rdatap = rdata;
	return ISC_R_SUCCESS;

cleanup:
	isc_lex_close(entry->lex);
	SAFE_MEM_PUT_PTR(mctx, rdata);
	if (rdatamem.base != NULL)
		isc_mem_put(mctx, rdatamem.base, rdatamem.length);

	return result;
}

/**
 * @param ldap_conn    A LDAP connection structure obtained via ldap_get_connection().
 * @param ldap_qresult New ldap_qresult structure will be allocated and pointer
 *                     to it will be returned through this parameter. The result
 *                     has to be freed by caller via ldap_query_free().
 */
static isc_result_t
ldap_query(ldap_instance_t *ldap_inst, ldap_connection_t *ldap_conn,
	   ldap_qresult_t **ldap_qresultp, const char *base, int scope, char **attrs,
	   int attrsonly, const char *filter, ...)
{
	va_list ap;
	isc_result_t result;
	ldap_qresult_t *ldap_qresult = NULL;
	int cnt;
	int ret;
	int ldap_err_code;
	int once = 0;
	isc_boolean_t autoconn = (ldap_conn == NULL);

	REQUIRE(ldap_inst != NULL);
	REQUIRE(base != NULL);
	REQUIRE(ldap_qresultp != NULL && *ldap_qresultp == NULL);

	CHECK(ldap_query_create(ldap_inst->mctx, &ldap_qresult));
	if (autoconn)
		CHECK(ldap_pool_getconnection(ldap_inst->pool, &ldap_conn));

	va_start(ap, filter);
	result = str_vsprintf(ldap_qresult->query_string, filter, ap);
	va_end(ap);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	log_debug(2, "querying '%s' with '%s'", base,
		  str_buf(ldap_qresult->query_string));

	if (ldap_conn->handle == NULL) {
		/*
		 * handle can be NULL when the first connection to LDAP wasn't
		 * successful
		 * TODO: handle this case inside ldap_pool_getconnection()?
		 */
		CHECK(handle_connection_error(ldap_inst, ldap_conn, ISC_FALSE));
	}

retry:
	ret = ldap_search_ext_s(ldap_conn->handle, base, scope,
				str_buf(ldap_qresult->query_string),
				attrs, attrsonly, NULL, NULL, NULL,
				LDAP_NO_LIMIT, &ldap_qresult->result);
	if (ret == 0) {
		ldap_conn->tries = 0;
		cnt = ldap_count_entries(ldap_conn->handle, ldap_qresult->result);
		log_debug(2, "entry count: %d", cnt);

		result = ldap_entrylist_create(ldap_conn->mctx,
					       ldap_conn->handle,
					       ldap_qresult->result,
					       &ldap_qresult->ldap_entries);
		if (result != ISC_R_SUCCESS) {
			log_error("failed to save LDAP query results");
			goto cleanup;
		}
		/* LDAP call suceeded, errors from ldap_entrylist_create() will be
		 * handled in cleanup section */

	} else { /* LDAP error - continue with error handler */
		result = ISC_R_FAILURE;
		ret = ldap_get_option(ldap_conn->handle, LDAP_OPT_RESULT_CODE,
					  (void *)&ldap_err_code);
		if (ret == LDAP_OPT_SUCCESS && ldap_err_code == LDAP_NO_SUCH_OBJECT) {
			result = ISC_R_NOTFOUND;
		} else if (!once) {
			/* some error happened during ldap_search, try to recover */
			once++;
			result = handle_connection_error(ldap_inst, ldap_conn,
							 ISC_FALSE);
			if (result == ISC_R_SUCCESS)
				goto retry;
		}
	}

cleanup:
	if (autoconn)
		ldap_pool_putconnection(ldap_inst->pool, &ldap_conn);
	if (result != ISC_R_SUCCESS) {
		ldap_query_free(ISC_FALSE, &ldap_qresult);
	} else {
		*ldap_qresultp = ldap_qresult;
	}
	return result;
}

/**
 * Allocate and initialize new ldap_qresult structure.
 * @param[out] ldap_qresultp Newly allocated ldap_qresult structure.
 * @return ISC_R_SUCCESS or ISC_R_NOMEMORY (from CHECKED_MEM_GET_PTR)
 */
static isc_result_t
ldap_query_create(isc_mem_t *mctx, ldap_qresult_t **ldap_qresultp) {
	ldap_qresult_t *ldap_qresult = NULL;
	isc_result_t result;

	CHECKED_MEM_GET_PTR(mctx, ldap_qresult);
	ZERO_PTR(ldap_qresult);
	ldap_qresult->mctx = mctx;
	INIT_LIST(ldap_qresult->ldap_entries);
	CHECK(str_new(mctx, &ldap_qresult->query_string));

	*ldap_qresultp = ldap_qresult;
	return ISC_R_SUCCESS;

cleanup:
	if (ldap_qresult != NULL) {
		str_destroy(&ldap_qresult->query_string);
		SAFE_MEM_PUT_PTR(mctx, ldap_qresult);
	}

	return result;
}

/**
 * Free LDAP query result. Can free the whole structure or internal parts only.
 * Freeing internal parts is suitable before reusing the structure.
 * @param[in] prepare_reuse ISC_TRUE implies freeing internal parts,
 *                          but not the whole structure.
 * @param[in,out] ldap_qresultp Pointer to freed query. Will be set to NULL
 *                              if prepare_reuse == ISC_FALSE.
 */
static void
ldap_query_free(isc_boolean_t prepare_reuse, ldap_qresult_t **ldap_qresultp)
{
	ldap_qresult_t *qresult;
	REQUIRE(ldap_qresultp != NULL);

	qresult = *ldap_qresultp;

	if (qresult == NULL)
		return;

	if (qresult->result) {
		ldap_msgfree(qresult->result);
		qresult->result = NULL;
	}

	ldap_entrylist_destroy(qresult->mctx, &qresult->ldap_entries);

	if (prepare_reuse) {
		str_clear(qresult->query_string);
		INIT_LIST(qresult->ldap_entries);
	} else { /* free the whole structure */
		str_destroy(&qresult->query_string);
		SAFE_MEM_PUT_PTR(qresult->mctx, qresult);
		*ldap_qresultp = NULL;
	}
}

/* FIXME: Tested with SASL/GSSAPI/KRB5 only */
static int ATTR_NONNULL(3)
ldap_sasl_interact(LDAP *ld, unsigned flags, void *defaults, void *sin)
{
	sasl_interact_t *in;
	ldap_instance_t *ldap_inst = defaults;
	int ret = LDAP_OTHER;
	isc_result_t result;

	REQUIRE(ldap_inst != NULL);
	UNUSED(flags);

	if (ld == NULL || sin == NULL)
		return LDAP_PARAM_ERROR;

	log_debug(4, "doing interactive bind");
	for (in = sin; in != NULL && in->id != SASL_CB_LIST_END; in++) {
		switch (in->id) {
		case SASL_CB_USER:
			log_debug(4, "got request for SASL_CB_USER");
			CHECK(setting_get_str("sasl_user",
					      ldap_inst->global_settings,
					      (const char **)&in->result));
			in->len = strlen(in->result);
			ret = LDAP_SUCCESS;
			break;
		case SASL_CB_GETREALM:
			log_debug(4, "got request for SASL_CB_GETREALM");
			CHECK(setting_get_str("sasl_realm",
					      ldap_inst->global_settings,
					      (const char **)&in->result));
			in->len = strlen(in->result);
			ret = LDAP_SUCCESS;
			break;
		case SASL_CB_AUTHNAME:
			log_debug(4, "got request for SASL_CB_AUTHNAME");
			CHECK(setting_get_str("sasl_auth_name",
					      ldap_inst->global_settings,
					      (const char **)&in->result));
			in->len = strlen(in->result);
			ret = LDAP_SUCCESS;
			break;
		case SASL_CB_PASS:
			log_debug(4, "got request for SASL_CB_PASS");
			CHECK(setting_get_str("sasl_password",
					      ldap_inst->global_settings,
					      (const char **)&in->result));
			in->len = strlen(in->result);
			ret = LDAP_SUCCESS;
			break;
		default:
			goto cleanup;
		}
	}

	return ret;

cleanup:
	in->result = NULL;
	in->len = 0;
	return LDAP_OTHER;
}

/*
 * Initialize the LDAP handle and bind to the server. Needed authentication
 * credentials and settings are available from the ldap_inst.
 */
static isc_result_t
ldap_connect(ldap_instance_t *ldap_inst, ldap_connection_t *ldap_conn,
	     isc_boolean_t force)
{
	LDAP *ld = NULL;
	int ret;
	int version;
	struct timeval timeout;
	isc_result_t result = ISC_R_FAILURE;
	const char *uri = NULL;
	const char *ldap_hostname = NULL;
	isc_uint32_t timeout_sec;

	REQUIRE(ldap_inst != NULL);
	REQUIRE(ldap_conn != NULL);

	CHECK(setting_get_str("uri", ldap_inst->local_settings, &uri));
	ret = ldap_initialize(&ld, uri);
	if (ret != LDAP_SUCCESS) {
		log_error("LDAP initialization failed: %s",
			  ldap_err2string(ret));
		CLEANUP_WITH(ISC_R_FAILURE);
	}

	version = LDAP_VERSION3;
	ret = ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
	LDAP_OPT_CHECK(ret, "failed to set LDAP version");

	CHECK(setting_get_uint("timeout", ldap_inst->global_settings,
			       &timeout_sec));
	timeout.tv_sec = timeout_sec;
	timeout.tv_usec = 0;

	ret = ldap_set_option(ld, LDAP_OPT_TIMEOUT, &timeout);
	LDAP_OPT_CHECK(ret, "failed to set timeout");

	CHECK(setting_get_str("ldap_hostname", ldap_inst->local_settings,
			      &ldap_hostname));
	if (strlen(ldap_hostname) > 0) {
		ret = ldap_set_option(ld, LDAP_OPT_HOST_NAME, ldap_hostname);
		LDAP_OPT_CHECK(ret, "failed to set LDAP_OPT_HOST_NAME");
	}

	if (ldap_conn->handle != NULL)
		ldap_unbind_ext_s(ldap_conn->handle, NULL, NULL);
	ldap_conn->handle = ld;
	ld = NULL; /* prevent double-unbind from ldap_reconnect() and cleanup: */

	CHECK(ldap_reconnect(ldap_inst, ldap_conn, force));
	return result;

cleanup:
	if (ld != NULL)
		ldap_unbind_ext_s(ld, NULL, NULL);

	/* Make sure handle is NULL. */
	if (ldap_conn->handle != NULL) {
		ldap_unbind_ext_s(ldap_conn->handle, NULL, NULL);
		ldap_conn->handle = NULL;
	}

	return result;
}

static isc_result_t
ldap_reconnect(ldap_instance_t *ldap_inst, ldap_connection_t *ldap_conn,
	       isc_boolean_t force)
{
	isc_result_t result;
	int ret = 0;
	const char *bind_dn = NULL;
	const char *password = NULL;
	const char *uri = NULL;
	const char *sasl_mech = NULL;
	const char *krb5_principal = NULL;
	const char *krb5_keytab = NULL;
	ldap_auth_t auth_method_enum = AUTH_INVALID;
	isc_uint32_t reconnect_interval;

	if (force)
		goto force_reconnect;

	if (ldap_conn->tries > 0) {
		isc_time_t now;
		int time_cmp;

		result = isc_time_now(&now);
		time_cmp = isc_time_compare(&now, &ldap_conn->next_reconnect);
		if (result == ISC_R_SUCCESS && time_cmp < 0)
			return ISC_R_SOFTQUOTA;
	}

	/* Set the next possible reconnect time. */
	{
		isc_interval_t delay;
		unsigned int i;
		unsigned int seconds;
		const unsigned int intervals[] = { 2, 5, 20, UINT_MAX };
		const size_t ntimes = sizeof(intervals) / sizeof(intervals[0]);

		i = ISC_MIN(ntimes - 1, ldap_conn->tries);
		CHECK(setting_get_uint("reconnect_interval",
				       ldap_inst->global_settings,
				       &reconnect_interval));
		seconds = ISC_MIN(intervals[i], reconnect_interval);
		isc_interval_set(&delay, seconds, 0);
		isc_time_nowplusinterval(&ldap_conn->next_reconnect, &delay);
	}

	ldap_conn->tries++;
force_reconnect:
	CHECK(setting_get_str("uri", ldap_inst->local_settings, &uri));
	log_debug(2, "trying to establish LDAP connection to %s", uri);

	CHECK(setting_get_uint("auth_method_enum", ldap_inst->local_settings,
			       &auth_method_enum));
	switch (auth_method_enum) {
	case AUTH_NONE:
		ret = ldap_simple_bind_s(ldap_conn->handle, NULL, NULL);
		break;
	case AUTH_SIMPLE:
		CHECK(setting_get_str("bind_dn", ldap_inst->global_settings, &bind_dn));
		CHECK(setting_get_str("password", ldap_inst->global_settings, &password));
		ret = ldap_simple_bind_s(ldap_conn->handle, bind_dn, password);
		break;
	case AUTH_SASL:
		CHECK(setting_get_str("sasl_mech", ldap_inst->local_settings,
				      &sasl_mech));
		if (strcmp(sasl_mech, "GSSAPI") == 0) {
			CHECK(setting_get_str("krb5_principal",
					      ldap_inst->local_settings,
					      &krb5_principal));
			CHECK(setting_get_str("krb5_keytab",
					      ldap_inst->local_settings,
					      &krb5_keytab));
			LOCK(&ldap_inst->kinit_lock);
			result = get_krb5_tgt(ldap_inst->mctx,
					      krb5_principal,
					      krb5_keytab);
			UNLOCK(&ldap_inst->kinit_lock);
			if (result != ISC_R_SUCCESS)
				return ISC_R_NOTCONNECTED;
		}

		log_debug(4, "trying interactive bind using '%s' mechanism",
			  sasl_mech);
		ret = ldap_sasl_interactive_bind_s(ldap_conn->handle, NULL,
						   sasl_mech,
						   NULL, NULL, LDAP_SASL_QUIET,
						   ldap_sasl_interact,
						   ldap_inst);
		break;
	case AUTH_INVALID:
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				"invalid auth_method_enum value %u",
				 auth_method_enum);
		break;

	default:
		log_bug("unsupported authentication mechanism");
		ret = LDAP_OTHER;
		break;
	}

	if (ret != LDAP_SUCCESS) {
		log_ldap_error(ldap_conn->handle, "bind to LDAP server failed");

		/*
		 * Clean the connection handle.
		 */
		if (ldap_conn->handle != NULL) {
			ldap_unbind_ext_s(ldap_conn->handle, NULL, NULL);
			ldap_conn->handle = NULL;
		}

		switch (ret) {
		case LDAP_INVALID_CREDENTIALS:
			return ISC_R_NOPERM;
		case LDAP_SERVER_DOWN:
			return ISC_R_NOTCONNECTED;
		case LDAP_TIMEOUT:
			return ISC_R_TIMEDOUT;
		default:
			return ISC_R_FAILURE;
		}
	} else
		log_debug(2, "bind to LDAP server successful");

	ldap_conn->tries = 0;

	return ISC_R_SUCCESS;

cleanup:
	return result;
}

static isc_result_t
handle_connection_error(ldap_instance_t *ldap_inst, ldap_connection_t *ldap_conn,
			isc_boolean_t force)
{
	int ret;
	int err_code;
	isc_result_t result = ISC_R_FAILURE;

	REQUIRE(ldap_conn != NULL);

	if (ldap_conn->handle == NULL)
		goto reconnect;

	ret = ldap_get_option(ldap_conn->handle, LDAP_OPT_RESULT_CODE,
				(void *)&err_code);
	if (ret != LDAP_OPT_SUCCESS) {
		log_error("handle_connection_error failed to obtain ldap error code");
		goto reconnect;
	}

	switch (err_code) {
	case LDAP_NO_SUCH_OBJECT:
		ldap_conn->tries = 0;
		result = ISC_R_SUCCESS;
		break;
	case LDAP_TIMEOUT:
		log_error("LDAP query timed out. Try to adjust \"timeout\" parameter");
		result = ISC_R_TIMEDOUT;
		break;
	case LDAP_INVALID_DN_SYNTAX:
	case LDAP_INVALID_SYNTAX:
	case LDAP_FILTER_ERROR:
		log_ldap_error(ldap_conn->handle, "invalid syntax in "
			       "handle_connection_error indicates a bug");
		result = ISC_R_UNEXPECTEDTOKEN;
		break;
	default:
		/* Try to reconnect on other errors. */
		log_ldap_error(ldap_conn->handle, "connection error");
reconnect:
		if (ldap_conn->handle == NULL)
			log_error("connection to the LDAP server was lost");
		result = ldap_connect(ldap_inst, ldap_conn, force);
		if (result == ISC_R_SUCCESS)
			log_info("successfully reconnected to LDAP server");
		break;
	}

	return result;
}

static isc_result_t
ldap_modify_do(ldap_instance_t *ldap_inst, const char *dn, LDAPMod **mods,
		isc_boolean_t delete_node)
{
	int ret;
	int err_code;
	const char *operation_str;
	isc_boolean_t once = ISC_FALSE;
	isc_result_t result;
	ldap_connection_t *ldap_conn = NULL;

	REQUIRE(dn != NULL);
	REQUIRE(mods != NULL);
	REQUIRE(ldap_inst != NULL);

	/* Any mod_op can be ORed with LDAP_MOD_BVALUES. */
	if ((mods[0]->mod_op & ~LDAP_MOD_BVALUES) == LDAP_MOD_ADD)
		operation_str = "modifying(add)";
	else if ((mods[0]->mod_op & ~LDAP_MOD_BVALUES) == LDAP_MOD_DELETE)
		operation_str = "modifying(del)";
	else if ((mods[0]->mod_op & ~LDAP_MOD_BVALUES) == LDAP_MOD_REPLACE)
		operation_str = "modifying(replace)";
	else {
		operation_str = "modifying(unknown operation)";
		log_bug("%s: 0x%x", operation_str, mods[0]->mod_op);
		CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
	}

	CHECK(ldap_pool_getconnection(ldap_inst->pool, &ldap_conn));
	if (ldap_conn->handle == NULL) {
		/*
		 * handle can be NULL when the first connection to LDAP wasn't
		 * successful
		 * TODO: handle this case inside ldap_pool_getconnection()?
		 */
retry:
		once = ISC_TRUE;
		CHECK(handle_connection_error(ldap_inst, ldap_conn, ISC_FALSE));
	}

	if (delete_node) {
		log_debug(2, "deleting whole node: '%s'", dn);
		ret = ldap_delete_ext_s(ldap_conn->handle, dn, NULL, NULL);
	} else {
		log_debug(2, "writing to '%s': %s", dn, operation_str);
		ret = ldap_modify_ext_s(ldap_conn->handle, dn, mods, NULL, NULL);
	}

	result = (ret == LDAP_SUCCESS) ? ISC_R_SUCCESS : ISC_R_FAILURE;
	if (ret == LDAP_SUCCESS)
		goto cleanup;

	LDAP_OPT_CHECK(ldap_get_option(ldap_conn->handle, LDAP_OPT_RESULT_CODE,
			&err_code), "ldap_modify_do(%s) failed to obtain ldap error code",
			operation_str);

	/* If there is no object yet, create it with an ldap add operation. */
	if ((mods[0]->mod_op & ~LDAP_MOD_BVALUES) == LDAP_MOD_ADD &&
	     err_code == LDAP_NO_SUCH_OBJECT) {
		int i;
		LDAPMod **new_mods;
		char *obj_str[] = { "idnsRecord", NULL };
		LDAPMod obj_class = {
			0, "objectClass", { .modv_strvals = obj_str },
		};

		/*
		 * Create a new array of LDAPMod structures. We will change
		 * the mod_op member of each one to 0 (but preserve
		 * LDAP_MOD_BVALUES. Additionally, we also need to specify
		 * the objectClass attribute.
		 */
		for (i = 0; mods[i]; i++)
			mods[i]->mod_op &= LDAP_MOD_BVALUES;
		new_mods = alloca((i + 2) * sizeof(LDAPMod *));
		memcpy(new_mods, mods, i * sizeof(LDAPMod *));
		new_mods[i] = &obj_class;
		new_mods[i + 1] = NULL;

		ret = ldap_add_ext_s(ldap_conn->handle, dn, new_mods, NULL, NULL);
		result = (ret == LDAP_SUCCESS) ? ISC_R_SUCCESS : ISC_R_FAILURE;
		if (ret == LDAP_SUCCESS)
			goto cleanup;
		LDAP_OPT_CHECK(ldap_get_option(ldap_conn->handle, LDAP_OPT_RESULT_CODE,
				&err_code),
				"ldap_modify_do(add) failed to obtain ldap error code");
		operation_str = "adding";
	}

	log_ldap_error(ldap_conn->handle, "while %s entry '%s'", operation_str, dn);

	/* do not error out if we are trying to delete an
	 * unexisting attribute */
	if ((mods[0]->mod_op & ~LDAP_MOD_BVALUES) != LDAP_MOD_DELETE ||
	    err_code != LDAP_NO_SUCH_ATTRIBUTE) {
		result = ISC_R_FAILURE;
		if (once == ISC_FALSE) {
			log_error("retrying LDAP operation (%s) on entry '%s'",
				  operation_str, dn);
			goto retry;
		}
	}

cleanup:
	ldap_pool_putconnection(ldap_inst->pool, &ldap_conn);

	return result;
}

static void ATTR_NONNULLS
ldap_mod_free(isc_mem_t *mctx, LDAPMod **changep)
{
	LDAPMod *change;

	REQUIRE(changep != NULL);

	change = *changep;
	if (change == NULL)
		return;

	free_char_array(mctx, &change->mod_values);
	if (change->mod_type != NULL)
		SAFE_MEM_PUT(mctx, change->mod_type, LDAP_ATTR_FORMATSIZE);
	SAFE_MEM_PUT_PTR(mctx, change);

	*changep = NULL;
}

static isc_result_t ATTR_NONNULLS
ldap_mod_create(isc_mem_t *mctx, LDAPMod **changep)
{
	LDAPMod *change = NULL;
	isc_result_t result;

	REQUIRE(changep != NULL && *changep == NULL);

	CHECKED_MEM_GET_PTR(mctx, change);
	ZERO_PTR(change);
	CHECKED_MEM_GET(mctx, change->mod_type, LDAP_ATTR_FORMATSIZE);

	*changep = change;
	return ISC_R_SUCCESS;

cleanup:
	if (change != NULL)
		SAFE_MEM_PUT_PTR(mctx, change);

	return result;
}

static isc_result_t
ldap_rdatalist_to_ldapmod(isc_mem_t *mctx, dns_rdatalist_t *rdlist,
			  LDAPMod **changep, int mod_op)
{
	isc_result_t result;
	LDAPMod *change = NULL;
	char **vals = NULL;

	CHECK(ldap_mod_create(mctx, &change));
	CHECK(rdatatype_to_ldap_attribute(rdlist->type, change->mod_type,
					  LDAP_ATTR_FORMATSIZE));
	CHECK(ldap_rdata_to_char_array(mctx, HEAD(rdlist->rdata), &vals));

	change->mod_op = mod_op;
	change->mod_values = vals;

	*changep = change;
	return ISC_R_SUCCESS;

cleanup:
	ldap_mod_free(mctx, &change);

	return result;
}

static isc_result_t
ldap_rdata_to_char_array(isc_mem_t *mctx, dns_rdata_t *rdata_head,
			 char ***valsp)
{
	isc_result_t result;
	char **vals;
	unsigned int i;
	unsigned int rdata_count = 0;
	size_t vals_size;
	dns_rdata_t *rdata;

	REQUIRE(rdata_head != NULL);
	REQUIRE(valsp != NULL && *valsp == NULL);

	for (rdata = rdata_head; rdata != NULL; rdata = NEXT(rdata, link))
		rdata_count++;

	vals_size = (rdata_count + 1) * sizeof(char *);

	CHECKED_MEM_ALLOCATE(mctx, vals, vals_size);
	memset(vals, 0, vals_size);

	rdata = rdata_head;
	for (i = 0; i < rdata_count && rdata != NULL; i++) {
		DECLARE_BUFFER(buffer, DNS_RDATA_MAXLENGTH);
		isc_region_t region;

		/* Convert rdata to text. */
		INIT_BUFFER(buffer);
		CHECK(dns_rdata_totext(rdata, NULL, &buffer));
		isc_buffer_usedregion(&buffer, &region);

		/* Now allocate the string with the right size. */
		CHECKED_MEM_ALLOCATE(mctx, vals[i], region.length + 1);
		memcpy(vals[i], region.base, region.length);
		vals[i][region.length] = '\0';
		
		rdata = NEXT(rdata, link);
	}

	*valsp = vals;
	return ISC_R_SUCCESS;

cleanup:
	free_char_array(mctx, &vals);
	return result;
}

static void
free_char_array(isc_mem_t *mctx, char ***valsp)
{
	char **vals;
	unsigned int i;

	REQUIRE(valsp != NULL);

	vals = *valsp;
	if (vals == NULL)
		return;

	for (i = 0; vals[i] != NULL; i++)
		isc_mem_free(mctx, vals[i]);

	isc_mem_free(mctx, vals);
	*valsp = NULL;
}

static isc_result_t
ldap_rdttl_to_ldapmod(isc_mem_t *mctx, dns_rdatalist_t *rdlist,
		      LDAPMod **changep)
{
	LDAPMod *change = NULL;
	ld_string_t *ttlval = NULL;
	char **vals = NULL;
	isc_result_t result;

	REQUIRE(changep != NULL && *changep == NULL);

	CHECK(str_new(mctx, &ttlval));
	CHECK(str_sprintf(ttlval, "%d", rdlist->ttl));

	CHECK(ldap_mod_create(mctx, &change));
	change->mod_op = LDAP_MOD_REPLACE;
	CHECK(isc_string_copy(change->mod_type, LDAP_ATTR_FORMATSIZE, "dnsTTL"));

	CHECKED_MEM_ALLOCATE(mctx, vals, 2 * sizeof(char *));
	memset(vals, 0, 2 * sizeof(char *));
	change->mod_values = vals;

	CHECKED_MEM_ALLOCATE(mctx, vals[0], str_len(ttlval) + 1);
	memcpy(vals[0], str_buf(ttlval), str_len(ttlval) + 1);

	*changep = change;

cleanup:
	if (ttlval) str_destroy(&ttlval);
	if (change && result != ISC_R_SUCCESS) ldap_mod_free(mctx, &change);

	return result;
}

/*
 * Modify the SOA record of a zone, where DN of the zone is 'zone_dn'.
 * The SOA record is a special case because we need to update serial,
 * refresh, retry, expire and minimum attributes for each SOA record.
 */
static isc_result_t ATTR_NONNULLS
modify_soa_record(ldap_instance_t *ldap_inst, const char *zone_dn,
		  dns_rdata_t *rdata)
{
	isc_result_t result;
	dns_rdata_soa_t soa;
	LDAPMod change[5];
	LDAPMod *changep[6] = {
		&change[0], &change[1], &change[2], &change[3], &change[4],
		NULL
	};

	REQUIRE(ldap_inst != NULL);

/* all values in SOA record are isc_uint32_t, i.e. max. 2^32-1 */
#define MAX_SOANUM_LENGTH (10 + 1)
#define SET_LDAP_MOD(index, name) \
	change[index].mod_op = LDAP_MOD_REPLACE; \
	change[index].mod_type = "idnsSOA" #name; \
	change[index].mod_values = alloca(2 * sizeof(char *)); \
	change[index].mod_values[0] = alloca(MAX_SOANUM_LENGTH); \
	change[index].mod_values[1] = NULL; \
	CHECK(isc_string_printf(change[index].mod_values[0], \
		MAX_SOANUM_LENGTH, "%u", soa.name));

	dns_rdata_tostruct(rdata, (void *)&soa, ldap_inst->mctx);

	SET_LDAP_MOD(0, serial);
	SET_LDAP_MOD(1, refresh);
	SET_LDAP_MOD(2, retry);
	SET_LDAP_MOD(3, expire);
	SET_LDAP_MOD(4, minimum);

	dns_rdata_freestruct((void *)&soa);

	result = ldap_modify_do(ldap_inst, zone_dn, changep, ISC_FALSE);

cleanup:
	return result;

#undef MAX_SOANUM_LENGTH
#undef SET_LDAP_MOD
}


#define SYNCPTR_PREF    "PTR record synchronization "
#define SYNCPTR_FMTPRE  SYNCPTR_PREF "(%s) for A/AAAA '%s' "
#define SYNCPTR_FMTPOST ldap_modop_str(mod_op), a_name_str

static const char *
ldap_modop_str(unsigned int mod_op) {
	static const char *add = "addition";
	static const char *del = "deletion";

	switch (mod_op) {
	case LDAP_MOD_ADD:
		return add;

	case LDAP_MOD_DELETE:
		return del;

	default:
		INSIST("unsupported LDAP mod_op" == NULL);
		return NULL;
	}
}

static void
append_trailing_dot(char *str, unsigned int size) {
	unsigned int length = strlen(str);
	if (str[length] != '.') {
		REQUIRE(length + 1 < size);
		str[length] = '.';
		str[length+1] = '\0';
	}
}

static isc_result_t
ldap_find_ptr(ldap_instance_t *ldap_inst, const int af, const char *ip_str,
	      dns_name_t *ptr_name, ld_string_t *ptr_dn,
	      dns_name_t *zone_name) {
	isc_result_t result;
	const char *owner_zone_dn_ptr;
	isc_mem_t *mctx = ldap_inst->mctx;

	union {
		struct in_addr v4;
		struct in6_addr v6;
	} ip;
	isc_netaddr_t isc_ip; /* internal net address representation */

	/* Get string with IP address from change request
	 * and convert it to in_addr structure. */
	if (inet_pton(af, ip_str, &ip) != 1) {
		log_bug(SYNCPTR_PREF "could not convert IP address "
			"from string '%s'", ip_str);
		CLEANUP_WITH(ISC_R_UNEXPECTED);
	}

	/* Only copy data to isc_ip stucture. */
	switch (af) {
	case AF_INET:
		isc_netaddr_fromin(&isc_ip, &ip.v4);
		break;
	case AF_INET6:
		isc_netaddr_fromin6(&isc_ip, &ip.v6);
		break;
	default:
		log_bug("unsupported address family 0x%x", af);
		CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
		break;
	}

	/*
	 * Convert IP address to PTR record.
	 *
	 * @example
	 * 192.168.0.1 -> 1.0.168.192.in-addr.arpa
	 *
	 * @todo Check if it works for IPv6 correctly.
	 */
	CHECK(dns_byaddr_createptrname2(&isc_ip, 0, ptr_name));

	/* Get LDAP entry indentifier. */
	CHECK(dnsname_to_dn(ldap_inst->zone_register, ptr_name, ptr_dn));

	/*
	 * @example
	 * owner_dn_ptr = "idnsName=100.0.168, idnsname=192.in-addr.arpa,cn=dns,$SUFFIX"
	 * owner_zone_dn_ptr = "idnsname=192.in-addr.arpa,cn=dns,$SUFFIX"
	 */
	owner_zone_dn_ptr = strstr(str_buf(ptr_dn),", ") + 1;

	/* Get attribute "idnsAllowDynUpdate" for reverse zone or use default. */
	CHECK(dn_to_dnsname(mctx, owner_zone_dn_ptr, zone_name, NULL));

cleanup:
	return result;
}

/**
 * Check if PTR record's value in LDAP == name of the modified A/AAAA record.
 * Update will be refused if the PTR name contains multiple PTR records or
 * if the value in LDAP != expected name.
 *
 * @param[in] a_name     Name of modified A/AAAA record.
 * @param[in] a_name_str Name of modified A/AAAA record as NUL terminated string.
 * @param[in] ptr_name   Name of PTR record generated from IP address in A/AAAA.
 * @param[in] mod_op     LDAP_MOD_DELETE if A/AAAA record is being deleted
 *                       or LDAP_MOD_ADD if A/AAAA record is being added.
 * @param[out] delete_node Will be set to ISC_TRUE if the database node
 *                         is empty after PTR record deletion.
 *
 * @retval ISC_R_IGNORE  A and PTR records match, no change is required.
 * @retval ISC_R_SUCCESS Prerequisites fulfilled, update is allowed.
 * @retval other         Errors
 *
 * @code
 * ** A record deletion **
 * ; nsupdate command:
 * update delete www.example.com. IN A	192.0.2.1
 *
 * ; PTR update will be allowed if the zone contains following data:
 * www.example.com.		A	192.0.2.1
 * 1.2.0.192.in-addr.arpa.	PTR	www.example.com.
 *
 * ; PTR update will not be allowed if the zone contains following data:
 * www.example.com.		A	192.0.2.1
 * 1.2.0.192.in-addr.arpa.	PTR	mail.example.com.
 * @endcode
 *
 * @code
 * ** A record addition **
 * ; nsupdate command:
 * update add www.example.com. 3600 IN A 192.0.2.1
 *
 * ; PTR update will be allowed if the zone does not contain A and PTR record.
 *
 * ; PTR update will not be allowed if the zone contains following data:
 * 1.2.0.192.in-addr.arpa. 	PTR	mail.example.com.
 * @endcode
 */
static isc_result_t
ldap_sync_ptr_validate(ldap_instance_t *ldap_inst, dns_name_t *a_name,
		       const char *a_name_str, dns_name_t *ptr_name,
		       int mod_op, isc_boolean_t *delete_node) {
	isc_result_t result;
	isc_mem_t *mctx = ldap_inst->mctx;

	char ptr_name_str[DNS_NAME_FORMATSIZE+1];
	isc_boolean_t ptr_found;
	dns_rdata_ptr_t ptr_rdata;
	char ptr_rdata_str[DNS_NAME_FORMATSIZE+1];
	isc_boolean_t ptr_a_equal = ISC_FALSE; /* GCC requires initialization */

	ldapdb_rdatalist_t ldap_rdlist;
	dns_rdatalist_t *ptr_rdlist = NULL;

	ISC_LIST_INIT(ldap_rdlist);

	REQUIRE(mod_op == LDAP_MOD_DELETE || mod_op == LDAP_MOD_ADD);
	REQUIRE(a_name_str != NULL);

	/* Find PTR entry in LDAP. */
	ptr_found = ISC_FALSE;
	result = ldapdb_rdatalist_get(mctx, ldap_inst, ptr_name,
				      NULL, &ldap_rdlist);

	*delete_node = ISC_FALSE;
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
		log_error_r(SYNCPTR_FMTPRE "failed in ldapdb_rdatalist_get()",
			    SYNCPTR_FMTPOST);
		goto cleanup;
	}

	/* Find the value of PTR entry. */
	if (result == ISC_R_SUCCESS) {
		result = ldapdb_rdatalist_findrdatatype(&ldap_rdlist,
							dns_rdatatype_ptr,
							&ptr_rdlist);
		if (result == ISC_R_SUCCESS && HEAD(ptr_rdlist->rdata) != NULL) {
			if (HEAD(ptr_rdlist->rdata) != TAIL(ptr_rdlist->rdata)) {
				dns_name_format(ptr_name, ptr_name_str,
						DNS_NAME_FORMATSIZE);
				append_trailing_dot(ptr_name_str,
						    sizeof(ptr_name_str));
				log_error(SYNCPTR_FMTPRE
					  "failed: multiple PTR records under "
					  "name '%s' are not supported",
					  SYNCPTR_FMTPOST, ptr_name_str);
				CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
			}
			dns_rdata_tostruct(HEAD(ptr_rdlist->rdata), &ptr_rdata,
					   NULL);

			ptr_found = ISC_TRUE;

			/* Compare PTR value with name of the A/AAAA record. */
			if (dns_name_isabsolute(a_name) &&
			    dns_name_isabsolute(&ptr_rdata.ptr) &&
			    dns_name_equal(&ptr_rdata.ptr, a_name)) {
				ptr_a_equal = ISC_TRUE;
			} else {
				ptr_a_equal = ISC_FALSE;
				dns_name_format(ptr_name, ptr_name_str,
						DNS_NAME_FORMATSIZE);
				append_trailing_dot(ptr_name_str,
						    sizeof(ptr_name_str));
				dns_name_format(&ptr_rdata.ptr, ptr_rdata_str,
						DNS_NAME_FORMATSIZE);
				append_trailing_dot(ptr_rdata_str,
						    sizeof(ptr_rdata_str));
			}
		}
	}

	if (mod_op == LDAP_MOD_DELETE) {
		if (ptr_found == ISC_FALSE) {
			log_debug(3, SYNCPTR_FMTPRE "skipped: no PTR records "
				  "found", SYNCPTR_FMTPOST);
			CLEANUP_WITH(ISC_R_IGNORE);

		} else if (ptr_a_equal == ISC_FALSE) {
			log_error(SYNCPTR_FMTPRE "failed: "
				  "existing PTR record '%s' contains unexpected "
				  "value '%s' (value '%s' expected)",
				  SYNCPTR_FMTPOST, ptr_name_str, ptr_rdata_str,
				  a_name_str);
			CLEANUP_WITH(ISC_R_UNEXPECTEDTOKEN);

		} else if (HEAD(ldap_rdlist) == TAIL(ldap_rdlist)) {
			/* Exactly one PTR record was found and rdlist contains
			 * exactly one RRset, so the deleted PTR record
			 * is the only RR in the node. */
			REQUIRE(HEAD(ldap_rdlist)->type == dns_rdatatype_ptr);
			*delete_node = ISC_TRUE;
		}

	} else if (mod_op == LDAP_MOD_ADD && ptr_found == ISC_TRUE) {
		if (ptr_a_equal == ISC_TRUE) {
			log_debug(3, SYNCPTR_FMTPRE "skipped: PTR record with"
				  "desired value is already present",
				  SYNCPTR_FMTPOST);
			CLEANUP_WITH(ISC_R_IGNORE);

		} else {
			log_error(SYNCPTR_FMTPRE "failed: "
				  "existing PTR record '%s' contains unexpected "
				  "value '%s' (value '%s' or no value expected)",
				  SYNCPTR_FMTPOST, ptr_name_str, ptr_rdata_str,
				  a_name_str);
			CLEANUP_WITH(DNS_R_SINGLETON);
		}
	}

	result = ISC_R_SUCCESS;

cleanup:
	ldapdb_rdatalist_destroy(mctx, &ldap_rdlist);

	return result;
}

static isc_result_t
ldap_sync_ptr(ldap_instance_t *ldap_inst, dns_name_t *a_name,
		const int af, const char *ip_str, const int mod_op) {
	isc_result_t result;
	isc_mem_t *mctx = ldap_inst->mctx;

	char **vals = NULL;

	char a_name_str[DNS_NAME_FORMATSIZE+1];

	ld_string_t *ptr_dn = NULL;
	struct dns_fixedname ptr_name;
	LDAPMod *change[2] = { NULL };

	dns_name_t zone_name;
	settings_set_t *zone_settings = NULL;
	isc_boolean_t zone_dyn_update;

	isc_boolean_t delete_node;

	dns_name_init(&zone_name, NULL);
	dns_fixedname_init(&ptr_name);
	CHECK(str_new(mctx, &ptr_dn));

	/**
	 * Get string representation of PTR record value.
	 * @code
	 * a_name_str = "host.example.com."
	 * @endcode
	 */
	dns_name_format(a_name, a_name_str, DNS_NAME_FORMATSIZE);
	append_trailing_dot(a_name_str, sizeof(a_name_str));

	result = ldap_find_ptr(ldap_inst, af, ip_str,
			       dns_fixedname_name(&ptr_name), ptr_dn, &zone_name);
	if (result != ISC_R_SUCCESS) {
		log_error_r(SYNCPTR_FMTPRE "refused: "
			    "unable to find active reverse zone "
			    "for IP address '%s'", SYNCPTR_FMTPOST, ip_str);
		CLEANUP_WITH(ISC_R_NOTFOUND);
	}

	CHECK(zr_get_zone_settings(ldap_inst->zone_register, &zone_name,
				   &zone_settings));
	CHECK(setting_get_bool("dyn_update", zone_settings, &zone_dyn_update));
	if (!zone_dyn_update) {
		char zone_name_str[DNS_NAME_FORMATSIZE];
		dns_name_format(&zone_name, zone_name_str, DNS_NAME_FORMATSIZE);
		log_error(SYNCPTR_FMTPRE "refused: "
			  "IP address '%s' belongs to reverse zone '%s' "
			  "and dynamic updates are not allowed for that zone",
			  SYNCPTR_FMTPOST, ip_str, zone_name_str);
		CLEANUP_WITH(ISC_R_NOPERM);
	}

	result = ldap_sync_ptr_validate(ldap_inst, a_name, a_name_str,
					dns_fixedname_name(&ptr_name), mod_op,
					&delete_node);
	if (result == ISC_R_IGNORE)
		CLEANUP_WITH(ISC_R_SUCCESS);
	else if (result != ISC_R_SUCCESS)
		CLEANUP_WITH(DNS_R_SERVFAIL);

	/* Fill the LDAPMod change structure up. */
	CHECK(ldap_mod_create(mctx, &change[0]));

	/* Do the same action what has been done with A/AAAA record. */
	change[0]->mod_op = mod_op;
	CHECK(rdatatype_to_ldap_attribute(dns_rdatatype_ptr, change[0]->mod_type,
					  LDAP_ATTR_FORMATSIZE));

	CHECKED_MEM_ALLOCATE(mctx, vals, 2 * sizeof(char *));
	memset(vals, 0, 2 * sizeof(char *));
	change[0]->mod_values = vals;

	CHECKED_MEM_ALLOCATE(mctx, vals[0], strlen(a_name_str) + 1);
	memcpy(vals[0], a_name_str, strlen(a_name_str) + 1);

	/* Modify PTR record. */
	CHECK(ldap_modify_do(ldap_inst, str_buf(ptr_dn),
			     change, delete_node));

cleanup:
	if (dns_name_dynamic(&zone_name))
		dns_name_free(&zone_name, mctx);
	str_destroy(&ptr_dn);
	ldap_mod_free(mctx, &change[0]);

	return result;
}
#undef SYNCPTR_PREF
#undef SYNCPTR_FMTPRE
#undef SYNCPTR_FMTPOST

static isc_result_t
modify_ldap_common(dns_name_t *owner, ldap_instance_t *ldap_inst,
		   dns_rdatalist_t *rdlist, int mod_op, isc_boolean_t delete_node)
{
	isc_result_t result;
	isc_mem_t *mctx = ldap_inst->mctx;
	ld_string_t *owner_dn = NULL;
	LDAPMod *change[3] = { NULL };
	isc_boolean_t zone_sync_ptr;
	char **vals = NULL;
	dns_name_t zone_name;
	char *zone_dn = NULL;
	settings_set_t *zone_settings = NULL;
	int af; /* address family */

	/*
	 * Find parent zone entry and check if Dynamic Update is allowed.
	 * @todo Try the cache first and improve split: SOA records are problematic.
	 */
	dns_name_init(&zone_name, NULL);
	CHECK(str_new(mctx, &owner_dn));

	CHECK(dnsname_to_dn(ldap_inst->zone_register, owner, owner_dn));
	zone_dn = strstr(str_buf(owner_dn),", ");

	if (zone_dn == NULL) { /* SOA record; owner = zone => owner_dn = zone_dn */
		zone_dn = (char *)str_buf(owner_dn);
	} else {
		zone_dn += 1; /* skip whitespace */
	}

	CHECK(dn_to_dnsname(mctx, zone_dn, &zone_name, NULL));

	result = zr_get_zone_settings(ldap_inst->zone_register, &zone_name,
				      &zone_settings);
	if (result != ISC_R_SUCCESS) {
		if (result == ISC_R_NOTFOUND)
			log_debug(3, "update refused: "
				  "active zone '%s' not found", zone_dn);
		CLEANUP_WITH(DNS_R_NOTAUTH);
	}

	if (rdlist->type == dns_rdatatype_soa && mod_op == LDAP_MOD_DELETE)
		CLEANUP_WITH(ISC_R_SUCCESS);

	if (rdlist->type == dns_rdatatype_soa) {
		result = modify_soa_record(ldap_inst, str_buf(owner_dn),
					   HEAD(rdlist->rdata));
		goto cleanup;
	}

	CHECK(ldap_rdatalist_to_ldapmod(mctx, rdlist, &change[0], mod_op));
	if (mod_op == LDAP_MOD_ADD) {
		/* for now always replace the ttl on add */
		CHECK(ldap_rdttl_to_ldapmod(mctx, rdlist, &change[1]));
	}

	CHECK(ldap_modify_do(ldap_inst, str_buf(owner_dn), change, delete_node));

	/* Keep the PTR of corresponding A/AAAA record synchronized. */
	if (rdlist->type == dns_rdatatype_a || rdlist->type == dns_rdatatype_aaaa) {
		/*
		 * Look for zone "idnsAllowSyncPTR" attribute. If attribute do not exist,
		 * use global plugin configuration: option "sync_ptr"
		 */

		CHECK(setting_get_bool("sync_ptr", zone_settings, &zone_sync_ptr));
		if (!zone_sync_ptr) {
			log_debug(3, "sync PTR is disabled for zone '%s'", zone_dn);
			CLEANUP_WITH(ISC_R_SUCCESS);
		}
		log_debug(3, "sync PTR is enabled for zone '%s'", zone_dn);

		af = (rdlist->type == dns_rdatatype_a) ? AF_INET : AF_INET6;
		result = ldap_sync_ptr(ldap_inst, owner, af,
				       change[0]->mod_values[0], mod_op);
	}

cleanup:
	str_destroy(&owner_dn);
	ldap_mod_free(mctx, &change[0]);
	ldap_mod_free(mctx, &change[1]);
	free_char_array(mctx, &vals);
	dns_name_free(&zone_name, mctx);

	return result;
}

isc_result_t
write_to_ldap(dns_name_t *owner, ldap_instance_t *ldap_inst, dns_rdatalist_t *rdlist)
{
	return modify_ldap_common(owner, ldap_inst, rdlist, LDAP_MOD_ADD, ISC_FALSE);
}

isc_result_t
remove_values_from_ldap(dns_name_t *owner, ldap_instance_t *ldap_inst,
		 dns_rdatalist_t *rdlist, isc_boolean_t delete_node)
{
	return modify_ldap_common(owner, ldap_inst, rdlist, LDAP_MOD_DELETE,
				  delete_node);
}

isc_result_t
remove_attr_from_ldap(dns_name_t *owner, ldap_instance_t *ldap_inst,
		      const char *attr) {
	LDAPMod *change[2] = { NULL };
	ld_string_t *dn = NULL;
	isc_result_t result;

	CHECK(str_new(ldap_inst->mctx, &dn));

	CHECK(ldap_mod_create(ldap_inst->mctx, &change[0]));
	change[0]->mod_op = LDAP_MOD_DELETE;
	CHECK(isc_string_copy(change[0]->mod_type, LDAP_ATTR_FORMATSIZE, attr));
	change[0]->mod_vals.modv_strvals = NULL; /* delete all values from given attribute */

	CHECK(dnsname_to_dn(ldap_inst->zone_register,
			    owner, dn));
	CHECK(ldap_modify_do(ldap_inst, str_buf(dn), change, ISC_FALSE));

cleanup:
	ldap_mod_free(ldap_inst->mctx, &change[0]);
	str_destroy(&dn);
	return result;
}


isc_result_t
remove_entry_from_ldap(dns_name_t *owner, ldap_instance_t *ldap_inst) {
	ldap_connection_t *ldap_conn = NULL;
	ld_string_t *dn = NULL;
	int ret;
	isc_result_t result;

	CHECK(str_new(ldap_inst->mctx, &dn));
	CHECK(dnsname_to_dn(ldap_inst->zone_register, owner, dn));
	log_debug(2, "deleting whole node: '%s'", str_buf(dn));

	CHECK(ldap_pool_getconnection(ldap_inst->pool, &ldap_conn));
	if (ldap_conn->handle == NULL) {
		/*
		 * handle can be NULL when the first connection to LDAP wasn't
		 * successful
		 * TODO: handle this case inside ldap_pool_getconnection()?
		 */
		CHECK(ldap_connect(ldap_inst, ldap_conn, ISC_FALSE));
	}
	ret = ldap_delete_ext_s(ldap_conn->handle, str_buf(dn), NULL, NULL);
	result = (ret == LDAP_SUCCESS) ? ISC_R_SUCCESS : ISC_R_FAILURE;
	if (ret == LDAP_SUCCESS)
		goto cleanup;

	LDAP_OPT_CHECK(ldap_get_option(ldap_conn->handle, LDAP_OPT_RESULT_CODE,
		       &ret), "remove_entry_from_ldap failed to obtain "
		       "ldap error code");

	if (result != ISC_R_SUCCESS)
		log_ldap_error(ldap_conn->handle, "while deleting entry '%s'",
			       str_buf(dn));
cleanup:
	ldap_pool_putconnection(ldap_inst->pool, &ldap_conn);
	str_destroy(&dn);
	return result;
}


static isc_result_t
ldap_pool_create(isc_mem_t *mctx, unsigned int connections, ldap_pool_t **poolp)
{
	ldap_pool_t *pool;
	isc_result_t result;

	REQUIRE(poolp != NULL && *poolp == NULL);

	CHECKED_MEM_GET(mctx, pool, sizeof(*pool));
	ZERO_PTR(pool);
	isc_mem_attach(mctx, &pool->mctx);
	
	CHECK(semaphore_init(&pool->conn_semaphore, connections));
	CHECKED_MEM_GET(mctx, pool->conns,
			connections * sizeof(ldap_connection_t *));
	memset(pool->conns, 0, connections * sizeof(ldap_connection_t *));
	pool->connections = connections;

	*poolp = pool;

	return ISC_R_SUCCESS;

cleanup:
	ldap_pool_destroy(&pool);
	return result;
}

static void
ldap_pool_destroy(ldap_pool_t **poolp)
{
	ldap_pool_t *pool;
	ldap_connection_t *ldap_conn;
	unsigned int i;

	REQUIRE(poolp != NULL);

	pool = *poolp;
	if (pool == NULL)
		return;

	if (pool->conns != NULL) {
		for (i = 0; i < pool->connections; i++) {
			ldap_conn = pool->conns[i];
			if (ldap_conn != NULL)
				destroy_ldap_connection(&ldap_conn);
		}

		SAFE_MEM_PUT(pool->mctx, pool->conns,
			     pool->connections * sizeof(ldap_connection_t *));
	}

	semaphore_destroy(&pool->conn_semaphore);

	MEM_PUT_AND_DETACH(pool);
	*poolp = NULL;
}

static isc_result_t
ldap_pool_getconnection(ldap_pool_t *pool, ldap_connection_t ** conn)
{
	ldap_connection_t *ldap_conn = NULL;
	unsigned int i;
	isc_result_t result;

	REQUIRE(pool != NULL);
	REQUIRE(conn != NULL && *conn == NULL);
	ldap_conn = *conn;

	CHECK(semaphore_wait_timed(&pool->conn_semaphore));
	/* Following assertion is necessary to convince clang static analyzer
	 * that the loop is always entered. */
	REQUIRE(pool->connections > 0);
	for (i = 0; i < pool->connections; i++) {
		ldap_conn = pool->conns[i];
		if (isc_mutex_trylock(&ldap_conn->lock) == ISC_R_SUCCESS)
			break;
	}

	RUNTIME_CHECK(ldap_conn != NULL);

	*conn = ldap_conn;

cleanup:
	if (result != ISC_R_SUCCESS) {
		log_error("timeout in ldap_pool_getconnection(): try to raise "
				"'connections' parameter; potential deadlock?");
	}
	return result;
}

static void
ldap_pool_putconnection(ldap_pool_t *pool, ldap_connection_t **conn)
{
	REQUIRE(conn != NULL);
	ldap_connection_t *ldap_conn = *conn;

	if (ldap_conn == NULL)
		return;

	UNLOCK(&ldap_conn->lock);
	semaphore_signal(&pool->conn_semaphore);

	*conn = NULL;
}

static isc_result_t
ldap_pool_connect(ldap_pool_t *pool, ldap_instance_t *ldap_inst)
{
	isc_result_t result;
	ldap_connection_t *ldap_conn;
	unsigned int i;

	for (i = 0; i < pool->connections; i++) {
		ldap_conn = NULL;
		CHECK(new_ldap_connection(pool, &ldap_conn));
		result = ldap_connect(ldap_inst, ldap_conn, ISC_FALSE);
		/* Continue even if LDAP server is down */
		if (result != ISC_R_NOTCONNECTED && result != ISC_R_TIMEDOUT &&
		    result != ISC_R_SUCCESS) {
			goto cleanup;
		}
		pool->conns[i] = ldap_conn;
	}

	return ISC_R_SUCCESS;

cleanup:
	log_error_r("couldn't establish connection in LDAP connection pool");
	for (i = 0; i < pool->connections; i++) {
		destroy_ldap_connection(&pool->conns[i]);
	}
	return result;
}

#define LDAP_ENTRYCHANGE_ALL	(LDAP_SYNC_CAPI_ADD | LDAP_SYNC_CAPI_DELETE | LDAP_SYNC_CAPI_MODIFY)

#define SYNCREPL_ADD(chgtype) (chgtype == LDAP_SYNC_CAPI_ADD)
#define SYNCREPL_DEL(chgtype) (chgtype == LDAP_SYNC_CAPI_DELETE)
#define SYNCREPL_MOD(chgtype) (chgtype == LDAP_SYNC_CAPI_MODIFY)
/* SYNCREPL_MODDN: Change in DN can be detected only via UUID->DN mapping:
 * Map UUID to (remembered) DN and compare remembered DN with new one. */
/* SYNCREPL_ANY: Initial database dump should be detected via sync_ctx state:
 * All changes received before first 'intermediate' message contain initial
 * state of the database.
#define SYNCREPL_ANY(chgtype) ((chgtype & LDAP_ENTRYCHANGE_ALL) != 0)
 */

/*
 * update_zone routine is processed asynchronously so it cannot assume
 * anything about state of ldap_inst from where it was sent. The ldap_inst
 * could have been already destroyed due server reload. The safest
 * way how to handle zone update is to refetch ldap_inst,
 * perform query to LDAP and delete&add the zone. This is expensive
 * operation but zones don't change often.
 */
static void
update_zone(isc_task_t *task, isc_event_t *event)
{
	ldap_syncreplevent_t *pevent = (ldap_syncreplevent_t *)event;
	isc_result_t result ;
	ldap_instance_t *inst = NULL;
	ldap_entryclass_t objclass;
	isc_mem_t *mctx;
	dns_name_t prevname;
	dns_name_t currname;
	ldap_entry_t *entry = pevent->entry;
	ldap_valuelist_t values;
	isc_boolean_t zone_active = ISC_FALSE;

	mctx = pevent->mctx;
	dns_name_init(&currname, NULL);
	dns_name_init(&prevname, NULL);

	CHECK(manager_get_ldap_instance(pevent->dbname, &inst));
	CHECK(dn_to_dnsname(inst->mctx, pevent->dn, &currname, NULL));

	if (!SYNCREPL_DEL(pevent->chgtype)) {
		CHECK(ldap_entry_getvalues(entry, "idnsZoneActive", &values));
		if (HEAD(values) != NULL &&
		    strcasecmp(HEAD(values)->value, "TRUE") == 0)
			zone_active = ISC_TRUE;
	}

	if (zone_active) {
		CHECK(ldap_entry_getclass(entry, &objclass));
		if (objclass & LDAP_ENTRYCLASS_MASTER)
			CHECK(ldap_parse_master_zoneentry(entry, inst, task));
		else if (objclass & LDAP_ENTRYCLASS_FORWARD)
			CHECK(ldap_parse_fwd_zoneentry(entry, inst));

		/* This code is disabled because we don't have UUID->DN database yet.
		 if (SYNCREPL_MODDN(pevent->chgtype)) {
			if (dn_to_dnsname(inst->mctx, pevent->prevdn, &prevname, NULL)
					== ISC_R_SUCCESS) {
				CHECK(ldap_delete_zone(inst, pevent->prevdn,
				      ISC_TRUE, ISC_FALSE));
			} else {
				log_debug(5, "update_zone: old zone wasn't managed "
					     "by plugin, dn '%s'", pevent->prevdn);
			}

			// fill the cache with records from renamed zone //
			if (objclass & LDAP_ENTRYCLASS_MASTER) {
				CHECK(ldap_query(inst, NULL, &ldap_qresult_record, pevent->dn,
						LDAP_SCOPE_ONELEVEL, attrs_record, 0,
						"(objectClass=idnsRecord)"));

				for (entry_record = HEAD(ldap_qresult_record->ldap_entries);
						entry_record != NULL;
						entry_record = NEXT(entry_record, link)) {

					syncrepl_update(inst, entry_record, NULL);
				}
			}
		}
		*/
	} else {
		CHECK(ldap_delete_zone(inst, pevent->dn, ISC_TRUE, ISC_FALSE));
	}

cleanup:
	sync_concurr_limit_signal(inst->sctx);
	if (result != ISC_R_SUCCESS)
		log_error_r("update_zone (syncrepl) failed for '%s'. "
			  "Zones can be outdated, run `rndc reload`",
			  pevent->dn);

	if (dns_name_dynamic(&currname))
		dns_name_free(&currname, inst->mctx);
	if (dns_name_dynamic(&prevname))
		dns_name_free(&prevname, inst->mctx);
	isc_mem_free(mctx, pevent->dbname);
	if (pevent->prevdn != NULL)
		isc_mem_free(mctx, pevent->prevdn);
	isc_mem_free(mctx, pevent->dn);
	ldap_entry_destroy(mctx, &entry);
	isc_mem_detach(&mctx);
	isc_event_free(&event);
	isc_task_detach(&task);
}

static void
update_config(isc_task_t *task, isc_event_t *event)
{
	ldap_syncreplevent_t *pevent = (ldap_syncreplevent_t *)event;
	isc_result_t result;
	ldap_instance_t *inst = NULL;
	ldap_entry_t *entry = pevent->entry;
	isc_mem_t *mctx;

	mctx = pevent->mctx;

	CHECK(manager_get_ldap_instance(pevent->dbname, &inst));
	CHECK(ldap_parse_configentry(entry, inst));

cleanup:
	sync_concurr_limit_signal(inst->sctx);
	if (result != ISC_R_SUCCESS)
		log_error_r("update_config (syncrepl) failed for '%s'. "
			    "Configuration can be outdated, run `rndc reload`",
			    pevent->dn);

	ldap_entry_destroy(mctx, &entry);
	isc_mem_free(mctx, pevent->dbname);
	isc_mem_free(mctx, pevent->dn);
	isc_mem_detach(&mctx);
	isc_event_free(&event);
	isc_task_detach(&task);
}

/**
 * @brief Update record in cache.
 *
 * If it exists it is replaced with newer version.
 *
 * @param task Task indentifier.
 * @param event Internal data of type ldap_syncreplevent_t.
 */
static void
update_record(isc_task_t *task, isc_event_t *event)
{
	/* syncrepl event */
	ldap_syncreplevent_t *pevent = (ldap_syncreplevent_t *)event;
	isc_result_t result;
	ldap_instance_t *inst = NULL;
	isc_mem_t *mctx;
	dns_zone_t *zone_ptr = NULL;
	dns_zone_t *zone_raw = NULL;
	isc_boolean_t zone_found = ISC_FALSE;
	isc_boolean_t zone_reloaded = ISC_FALSE;
	isc_uint32_t serial;
	ldap_entry_t *entry = pevent->entry;
	const char *fake_mname = NULL;

	dns_db_t *rbtdb = NULL;
	dns_db_t *ldapdb = NULL;
	dns_diff_t diff;
	dns_difftuple_t *soa_tuple = NULL;

	dns_dbversion_t *version = NULL; /* version is shared between rbtdb and ldapdb */
	dns_dbnode_t *node = NULL; /* node is shared between rbtdb and ldapdb */
	dns_rdatasetiter_t *rbt_rds_iterator = NULL;

	dns_journal_t *journal = NULL;
	char *journal_filename = NULL;
	sync_state_t sync_state;

	mctx = pevent->mctx;
	dns_diff_init(mctx, &diff);

#ifdef RBTDB_DEBUG
	static unsigned int count = 0;
#endif

	/* Structure to be stored in the cache. */
	ldapdb_rdatalist_t rdatalist;
	INIT_LIST(rdatalist);

	/* Convert domain name from text to struct dns_name_t. */
	dns_name_t name;
	dns_name_t origin;
	dns_name_t prevname;
	dns_name_t prevorigin;
	dns_name_init(&name, NULL);
	dns_name_init(&origin, NULL);
	dns_name_init(&prevname, NULL);
	dns_name_init(&prevorigin, NULL);

	CHECK(manager_get_ldap_instance(pevent->dbname, &inst));
	CHECK(dn_to_dnsname(mctx, pevent->dn, &name, &origin));
	CHECK(zr_get_zone_ptr(inst->zone_register, &origin, &zone_ptr));
	zone_found = ISC_TRUE;

update_restart:
	rbtdb = NULL;
	ldapdb = NULL;
	journal = NULL;
	ldapdb_rdatalist_destroy(mctx, &rdatalist);
	CHECK(zr_get_zone_dbs(inst->zone_register, &name, &ldapdb, &rbtdb));
	CHECK(dns_db_newversion(ldapdb, &version));

	CHECK(dns_db_findnode(rbtdb, &name, ISC_TRUE, &node));
	result = dns_db_allrdatasets(rbtdb, node, version, 0, &rbt_rds_iterator);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND)
		goto cleanup;


	/* This code is disabled because we don't have UUID->DN database yet.
	    || SYNCREPL_MODDN(pevent->chgtype)) { */
	if (SYNCREPL_DEL(pevent->chgtype)) {
		log_debug(5, "syncrepl_update: removing name from rbtdb, dn: '%s'",
			  pevent->dn);
		/* Do nothing. rdatalist is initialized to empty list,
		 * so resulting diff will remove all the data from node. */
	}

	/* TODO: double check correctness before replacing ldap_query() with
	 *       data from *event */
	/* This code is disabled because we don't have UUID->DN database yet.
	if (SYNCREPL_MODDN(pevent->chgtype)) {
		// remove previous name only if it was inside DNS subtree //
		if (dn_to_dnsname(mctx, pevent->prevdn, &prevname, &prevorigin)
				== ISC_R_SUCCESS) {
			log_debug(5, "syncrepl_update: removing name from cache, dn: '%s'",
					  pevent->prevdn);
			cache = NULL;
			zone = NULL;
			rbtdb = NULL;
			CHECK(zr_get_zone_ptr(inst->zone_register, &prevname, &zone));
			result = dns_zone_getdb(zone, &rbtdb);
			REQUIRE(result == ISC_R_SUCCESS);

			result = zr_get_zone_cache(inst->zone_register, &prevname, &cache);
			if (result == ISC_R_SUCCESS)
				CHECK(discard_from_cache(cache, &prevname));
			else if (result != ISC_R_NOTFOUND)
				goto cleanup;

		} else {
			log_debug(5, "syncrepl_update: old name wasn't managed "
					"by plugin, dn '%s'", pevent->prevdn);
		}
	}
	*/

	if (SYNCREPL_ADD(pevent->chgtype) || SYNCREPL_MOD(pevent->chgtype)) {
		/* Parse new data from LDAP. */
		log_debug(5, "syncrepl_update: updating name in rbtdb, dn: '%s'",
		          pevent->dn);
		CHECK(setting_get_str("fake_mname", inst->local_settings,
				      &fake_mname));
		CHECK(ldap_parse_rrentry(mctx, entry, &origin, fake_mname,
					 &rdatalist));
	}

	if (rbt_rds_iterator != NULL) {
		CHECK(diff_ldap_rbtdb(mctx, &name, &rdatalist,
				      rbt_rds_iterator, &diff));
		dns_rdatasetiter_destroy(&rbt_rds_iterator);
	}

	sync_state_get(inst->sctx, &sync_state);
	/* No real change in RR data -> do not increment SOA serial. */
	if (HEAD(diff.tuples) != NULL) {
		if (sync_state == sync_finished) {
			CHECK(dns_db_createsoatuple(ldapdb, version, mctx,
						    DNS_DIFFOP_DEL, &soa_tuple));
			dns_diff_append(&diff, &soa_tuple);
			CHECK(dns_db_createsoatuple(ldapdb, version, mctx,
						    DNS_DIFFOP_ADD, &soa_tuple));
			CHECK(update_soa_serial(dns_updatemethod_unixtime,
						soa_tuple, &serial));
			dns_zone_log(zone_ptr, ISC_LOG_DEBUG(5),
				     "writing new zone serial %u to LDAP", serial);
			result = ldap_replace_serial(inst, &origin, serial);
			if (result != ISC_R_SUCCESS)
				dns_zone_log(zone_ptr, ISC_LOG_ERROR,
					     "serial (%u) write back to LDAP failed",
					     serial);
			dns_diff_append(&diff, &soa_tuple);
		}

#if RBTDB_DEBUG >= 2
		dns_diff_print(&diff, stdout);
#else
		dns_diff_print(&diff, NULL);
#endif
		if (sync_state == sync_finished) {
			/* write the transaction to journal */
			dns_zone_getraw(zone_ptr, &zone_raw);
			if (zone_raw == NULL)
				journal_filename = dns_zone_getjournal(zone_ptr);
			else
				journal_filename = dns_zone_getjournal(zone_raw);
			CHECK(dns_journal_open(mctx, journal_filename,
					       DNS_JOURNAL_CREATE, &journal));
			CHECK(dns_journal_write_transaction(journal, &diff));
		}
		/* commit */
		CHECK(dns_diff_apply(&diff, rbtdb, version));
		dns_db_closeversion(ldapdb, &version, ISC_TRUE);
	}

	/* Check if the zone is loaded or not.
	 * No other function above returns DNS_R_NOTLOADED. */
	if (sync_state == sync_finished)
		result = dns_zone_getserial2(zone_ptr, &serial);

cleanup:
#ifdef RBTDB_DEBUG
	if (++count % 100 == 0)
		log_info("update_record: %u entries processed; inuse: %zd",
			 count, isc_mem_inuse(mctx));
#endif
	dns_diff_clear(&diff);
	if (soa_tuple != NULL)
		dns_difftuple_free(&soa_tuple);
	if (rbt_rds_iterator != NULL)
		dns_rdatasetiter_destroy(&rbt_rds_iterator);
	if (node != NULL)
		dns_db_detachnode(rbtdb, &node);
	/* rollback */
	if (rbtdb != NULL && version != NULL)
		dns_db_closeversion(ldapdb, &version, ISC_FALSE);
	if (rbtdb != NULL)
		dns_db_detach(&rbtdb);
	if (journal != NULL)
		dns_journal_destroy(&journal);
	if (ldapdb != NULL)
		dns_db_detach(&ldapdb);
	if (result != ISC_R_SUCCESS && zone_found && !zone_reloaded &&
	   (result == DNS_R_NOTLOADED || result == DNS_R_BADZONE)) {
		log_debug(1, "reloading invalid zone after a change; "
			     "reload triggered by change in '%s'",
			     pevent->dn);

		if (zone_ptr != NULL)
			result = dns_zone_load(zone_ptr);
		if (result == ISC_R_SUCCESS || result == DNS_R_UPTODATE ||
		    result == DNS_R_DYNAMIC || result == DNS_R_CONTINUE) {
			/* zone reload succeeded, fire current event again */
			log_debug(1, "restarting update_record after zone reload "
				     "caused by change in '%s'", pevent->dn);
			zone_reloaded = ISC_TRUE;
			result = dns_zone_getserial2(zone_ptr, &serial);
			if (result == ISC_R_SUCCESS) {
				dns_zone_log(zone_ptr, ISC_LOG_INFO,
					     "reloaded serial %u", serial);
				goto update_restart;
			} else {
				dns_zone_log(zone_ptr, ISC_LOG_ERROR,
					     "could not get serial after "
					     "reload");
			}
		} else {
			dns_zone_log(zone_ptr, ISC_LOG_ERROR,
				    "unable to reload invalid zone; "
				    "reload triggered by change in '%s':%s",
				    pevent->dn, dns_result_totext(result));
		}

	} else if (result != ISC_R_SUCCESS) {
		/* error other than invalid zone */
		log_error_r("update_record (syncrepl) failed, dn '%s' change type 0x%x. "
			  "Records can be outdated, run `rndc reload`",
			  pevent->dn, pevent->chgtype);
	}

	sync_concurr_limit_signal(inst->sctx);
	if (zone_ptr != NULL)
		dns_zone_detach(&zone_ptr);
	if (dns_name_dynamic(&name))
		dns_name_free(&name, inst->mctx);
	if (dns_name_dynamic(&prevname))
		dns_name_free(&prevname, inst->mctx);
	if (dns_name_dynamic(&origin))
		dns_name_free(&origin, inst->mctx);
	if (dns_name_dynamic(&prevorigin))
		dns_name_free(&prevorigin, inst->mctx);
	ldapdb_rdatalist_destroy(mctx, &rdatalist);
	isc_mem_free(mctx, pevent->dbname);
	if (pevent->prevdn != NULL)
		isc_mem_free(mctx, pevent->prevdn);
	ldap_entry_destroy(mctx, &entry);
	isc_mem_free(mctx, pevent->dn);
	isc_mem_detach(&mctx);
	isc_event_free(&event);
	isc_task_detach(&task);
}

isc_result_t
ldap_dn_compare(const char *dn1_instr, const char *dn2_instr,
		isc_boolean_t *isequal) {
	int ret;
	isc_result_t result;
	LDAPDN dn1_ldap = NULL;
	LDAPDN dn2_ldap = NULL;
	char *dn1_outstr = NULL;
	char *dn2_outstr = NULL;

	ret = ldap_str2dn(dn1_instr, &dn1_ldap, LDAP_DN_FORMAT_LDAPV3);
	if (ret != LDAP_SUCCESS)
		CLEANUP_WITH(ISC_R_FAILURE);

	ret = ldap_str2dn(dn2_instr, &dn2_ldap, LDAP_DN_FORMAT_LDAPV3);
	if (ret != LDAP_SUCCESS)
		CLEANUP_WITH(ISC_R_FAILURE);

	ret = ldap_dn2str(dn1_ldap, &dn1_outstr, LDAP_DN_FORMAT_LDAPV3 | LDAP_DN_PEDANTIC);
	if (ret != LDAP_SUCCESS)
		CLEANUP_WITH(ISC_R_FAILURE);

	ret = ldap_dn2str(dn2_ldap, &dn2_outstr, LDAP_DN_FORMAT_LDAPV3 | LDAP_DN_PEDANTIC);
	if (ret != LDAP_SUCCESS)
		CLEANUP_WITH(ISC_R_FAILURE);

	*isequal = ISC_TF(strcasecmp(dn1_outstr, dn2_outstr) == 0);
	result = ISC_R_SUCCESS;

cleanup:
	if (dn1_ldap != NULL)
		ldap_dnfree(dn1_ldap);
	if (dn2_ldap != NULL)
		ldap_dnfree(dn2_ldap);
	if (dn1_outstr != NULL)
		ldap_memfree(dn1_outstr);
	if (dn1_outstr != NULL)
		ldap_memfree(dn2_outstr);

	return result;
}

static void
syncrepl_update(ldap_instance_t *inst, ldap_entry_t *entry, int chgtype)
{
	ldap_entryclass_t class = LDAP_ENTRYCLASS_NONE;
	isc_result_t result = ISC_R_SUCCESS;
	ldap_syncreplevent_t *pevent = NULL;
	dns_name_t entry_name;
	dns_name_t zone_name;
	dns_zone_t *zone_ptr = NULL;
	char *dn = NULL;
	char *prevdn_ldap = NULL;
	char *prevdn = NULL;
	char *dbname = NULL;
	const char *ldap_base = NULL;
	isc_boolean_t isbase;
	isc_mem_t *mctx = NULL;
	isc_taskaction_t action = NULL;
	isc_task_t *task = NULL;
	sync_state_t sync_state;

	log_debug(20, "syncrepl change type: " /*"none%d,"*/ "add%d, del%d, mod%d", /* moddn%d", */
		  /* !SYNCREPL_ANY(chgtype), */ SYNCREPL_ADD(chgtype),
		  SYNCREPL_DEL(chgtype), SYNCREPL_MOD(chgtype)/*, SYNCREPL_MODDN(chgtype) */ );

	isc_mem_attach(inst->mctx, &mctx);
	dns_name_init(&entry_name, NULL);
	dns_name_init(&zone_name, NULL);

	CHECKED_MEM_STRDUP(mctx, entry->dn, dn);
	CHECKED_MEM_STRDUP(mctx, inst->db_name, dbname);

	/* TODO: handle config objects properly - via UUID database */
	CHECK(setting_get_str("base", inst->local_settings, &ldap_base));
	CHECK(ldap_dn_compare(ldap_base, entry->dn, &isbase));
	if (isbase == ISC_TRUE) {
		class = LDAP_ENTRYCLASS_CONFIG;
	} else {
		CHECK(dn_to_dnsname(inst->mctx, dn, &entry_name, &zone_name));
		switch (chgtype) {
		case LDAP_SYNC_CAPI_ADD:
		case LDAP_SYNC_CAPI_MODIFY:
			CHECK(ldap_entry_getclass(entry, &class));
			break;

		default:
			/* deleted entry doesn't contain objectClass, so
			 * we need to find if the entry is zone or not
			 * in other way */
			result = fwdr_zone_ispresent(inst->fwd_register,
						     &entry_name);
			if (result == ISC_R_SUCCESS) {
				class = LDAP_ENTRYCLASS_FORWARD;
			} else if (dns_name_equal(&zone_name, dns_rootname)
						  == ISC_TRUE)
				class = LDAP_ENTRYCLASS_MASTER;
			else
				class = LDAP_ENTRYCLASS_RR;
			break;
		}
	}
	REQUIRE(class != LDAP_ENTRYCLASS_NONE);

	if (class == LDAP_ENTRYCLASS_MASTER || class == LDAP_ENTRYCLASS_RR) {
		result = zr_get_zone_ptr(inst->zone_register, &zone_name,
					 &zone_ptr);
		if (result == ISC_R_SUCCESS && dns_zone_getmgr(zone_ptr) != NULL)
			dns_zone_gettask(zone_ptr, &task);
		else {
			/* TODO: Fix race condition:
			 * zone is not (yet) present in zone register */
			log_debug(1, "TODO: DN '%s': task fallback", entry->dn);
			isc_task_attach(inst->task, &task);
			result = ISC_R_SUCCESS;
		}
	} else {
		isc_task_attach(inst->task, &task);
	}
	REQUIRE(task != NULL);


	/* This code is disabled because we don't have UUID->DN database yet.
	if (SYNCREPL_MODDN(chgtype)) {
		CHECKED_MEM_STRDUP(mctx, prevdn_ldap, prevdn);
	}
	*/

	if ((class & LDAP_ENTRYCLASS_CONFIG) != 0)
		action = update_config;
	else if ((class & LDAP_ENTRYCLASS_MASTER) != 0)
		action = update_zone;
	else if ((class & LDAP_ENTRYCLASS_FORWARD) != 0)
		action = update_zone;
	else if ((class & LDAP_ENTRYCLASS_RR) != 0)
		action = update_record;
	else {
		log_error("unsupported objectClass: dn '%s'", dn);
		result = ISC_R_NOTIMPLEMENTED;
		goto cleanup;
	}

	/* All events for single zone are handled by one task, so we don't
	 * need to spend time with normal records. */
	if (action == update_zone || action == update_config) {
		sync_state_get(inst->sctx, &sync_state);
		if (sync_state == sync_init)
			CHECK(sync_task_add(inst->sctx, task));
	}

	pevent = (ldap_syncreplevent_t *)isc_event_allocate(inst->mctx,
				inst, LDAPDB_EVENT_SYNCREPL_UPDATE,
				action, NULL,
				sizeof(ldap_syncreplevent_t));

	if (pevent == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup;
	}

	pevent->mctx = mctx;
	pevent->dbname = dbname;
	pevent->dn = dn;
	pevent->prevdn = prevdn;
	pevent->chgtype = chgtype;
	pevent->entry = entry;
	isc_task_send(task, (isc_event_t **)&pevent);

cleanup:
	if (dns_name_dynamic(&entry_name))
		dns_name_free(&entry_name, inst->mctx);
	if (dns_name_dynamic(&zone_name))
		dns_name_free(&zone_name, inst->mctx);
	if (zone_ptr != NULL)
		dns_zone_detach(&zone_ptr);
	if (result != ISC_R_SUCCESS) {
		log_error_r("syncrepl_update failed for object '%s'",
			    entry->dn);
		sync_concurr_limit_signal(inst->sctx);

		if (dbname != NULL)
			isc_mem_free(mctx, dbname);
		if (dn != NULL)
			isc_mem_free(mctx, dn);
		if (prevdn != NULL)
			isc_mem_free(mctx, prevdn);
		if (mctx != NULL)
			isc_mem_detach(&mctx);
		if (prevdn_ldap != NULL)
			ldap_memfree(prevdn);
		ldap_entry_destroy(inst->mctx, &entry);
		if (task != NULL)
			isc_task_detach(&task);
	}
}

#define CHECK_EXIT \
	do { \
		if (inst->exiting) \
			goto cleanup; \
	} while (0)

/*
 * This "sane" sleep allows us to end if signal set the "exiting" variable.
 *
 * Returns ISC_FALSE if we should terminate, ISC_TRUE otherwise.
 */
static inline isc_boolean_t
sane_sleep(const ldap_instance_t *inst, unsigned int timeout)
{
	unsigned int remains = timeout;

	while (remains && !inst->exiting)
		remains = sleep(remains);

	if (remains)
		log_debug(99, "sane_sleep: interrupted");

	return inst->exiting ? ISC_FALSE : ISC_TRUE;
}

/* No-op signal handler for SIGUSR1 */
static void
noop_handler(int signal)
{
	UNUSED(signal);
}

static inline void
install_usr1handler(void)
{
	struct sigaction sa;
	struct sigaction oldsa;
	int ret;
	static isc_boolean_t once = ISC_FALSE;

	if (once)
		return;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &noop_handler;

	ret = sigaction(SIGUSR1, &sa, &oldsa);
	RUNTIME_CHECK(ret == 0); /* If sigaction fails, it's a bug */

	/* Don't attempt to replace already existing handler */
	RUNTIME_CHECK(oldsa.sa_handler == NULL);

	once = ISC_TRUE;
}

/*
 * Called when a reference is returned by ldap_sync_init()/ldap_sync_poll().
 */
static int
ldap_sync_search_reference (
	ldap_sync_t			*ls,
	LDAPMessage			*msg ) {

	UNUSED(ls);
	UNUSED(msg);

	log_error("ldap_sync_search_reference is not yet handled");
	return LDAP_SUCCESS;
}

/*
 * Called when an entry is returned by ldap_sync_init()/ldap_sync_poll().
 * If phase is LDAP_SYNC_CAPI_ADD or LDAP_SYNC_CAPI_MODIFY,
 * the entry has been either added or modified, and thus
 * the complete view of the entry should be in the LDAPMessage.
 * If phase is LDAP_SYNC_CAPI_PRESENT or LDAP_SYNC_CAPI_DELETE,
 * only the DN should be in the LDAPMessage.
 */
int ldap_sync_search_entry (
	ldap_sync_t			*ls,
	LDAPMessage			*msg,
	struct berval			*entryUUID,
	ldap_sync_refresh_t		phase ) {

	ldap_instance_t *inst = ls->ls_private;
	ldap_entry_t *entry = NULL;
	isc_result_t result;
#ifdef RBTDB_DEBUG
	static unsigned int count = 0;
#endif

	/* TODO: Use this for UUID->DN mapping and MODDN detection. */
	UNUSED(entryUUID);

	if (inst->exiting)
		return LDAP_SUCCESS;

	sync_concurr_limit_wait(inst->sctx);
	CHECK(ldap_entry_create(inst->mctx, ls->ls_ld, msg, &entry));
	syncrepl_update(inst, entry, phase);
#ifdef RBTDB_DEBUG
	if (++count % 100 == 0)
		log_info("ldap_sync_search_entry: %u entries read; inuse: %zd",
			 count, isc_mem_inuse(inst->mctx));
#endif

cleanup:
	if (result != ISC_R_SUCCESS) {
		log_error_r("ldap_sync_search_entry failed");
		sync_concurr_limit_signal(inst->sctx);
		/* TODO: Add 'tainted' flag to the LDAP instance. */
	}

	/* Following return code will never reach upper layers.
	 * It is limitation in ldap_sync_init() and ldap_sync_poll()
	 * provided by OpenLDAP libs at the time of writing (2013-07-22). */
	return LDAP_SUCCESS;
}

/**
 * Called when specific intermediate/final messages are returned
 * by ldap_sync_init()/ldap_sync_poll().
 * If phase is LDAP_SYNC_CAPI_PRESENTS or LDAP_SYNC_CAPI_DELETES,
 * a "presents" or "deletes" phase begins.
 * If phase is LDAP_SYNC_CAPI_DONE, a special "presents" phase
 * with refreshDone set to "TRUE" has been returned, to indicate
 * that the refresh phase of a refreshAndPersist is complete.
 * In the above cases, syncUUIDs is NULL.
 *
 * If phase is LDAP_SYNC_CAPI_PRESENTS_IDSET or
 * LDAP_SYNC_CAPI_DELETES_IDSET, syncUUIDs is an array of UUIDs
 * that are either present or have been deleted.
 *
 * @see Section @ref syncrepl-theory in syncrepl.c for the background.
 */
int ldap_sync_intermediate (
	ldap_sync_t			*ls,
	LDAPMessage			*msg,
	BerVarray			syncUUIDs,
	ldap_sync_refresh_t		phase ) {

	isc_result_t	result;
	ldap_instance_t *inst = ls->ls_private;

	UNUSED(msg);
	UNUSED(syncUUIDs);
	UNUSED(phase);

	if (inst->exiting)
		return LDAP_SUCCESS;

	if (phase == LDAP_SYNC_CAPI_DONE) {
		log_debug(1, "ldap_sync_intermediate RECEIVED");
		result = sync_barrier_wait(inst->sctx, inst->db_name);
		if (result != ISC_R_SUCCESS)
			log_error_r("sync_barrier_wait() failed for instance '%s'",
				    inst->db_name);
	}
	return LDAP_SUCCESS;
}

/*
 * Called when a searchResultDone is returned
 * by ldap_sync_init()/ldap_sync_poll().
 * In refreshAndPersist, this can only occur if the search for any reason
 * is being terminated by the server.
 */
int ldap_sync_search_result (
	ldap_sync_t			*ls,
	LDAPMessage			*msg,
	int				refreshDeletes ) {

	UNUSED(ls);
	UNUSED(msg);
	UNUSED(refreshDeletes);

	log_error("ldap_sync_search_result is not yet handled");
	return LDAP_SUCCESS;
}

static void
ldap_sync_cleanup(ldap_sync_t **ldap_syncp) {
	ldap_sync_t *ldap_sync = NULL;

	REQUIRE(ldap_syncp != NULL);

	if (*ldap_syncp == NULL)
		return;

	ldap_sync = *ldap_syncp;
	ldap_sync_destroy(ldap_sync, 1);

	*ldap_syncp = NULL;
}


static isc_result_t
ldap_sync_prepare(ldap_instance_t *inst, settings_set_t *settings,
		  ldap_connection_t *conn, ldap_sync_t **ldap_syncp) {
	isc_result_t result;
	const char *base = NULL;
	isc_uint32_t reconnect_interval;
	ldap_sync_t *ldap_sync = NULL;

	REQUIRE(inst != NULL);
	REQUIRE(ldap_syncp != NULL && *ldap_syncp == NULL);

	sync_state_reset(inst->sctx);

	/* Remove stale zone & journal files. */
	CHECK(cleanup_files(inst));

	/* Try to connect. */
	while (conn->handle == NULL) {
		result = ISC_R_SHUTTINGDOWN;
		CHECK_EXIT;
		CHECK(setting_get_uint("reconnect_interval", settings,
				       &reconnect_interval));

		log_error("ldap_syncrepl will reconnect in %d second%s",
			  reconnect_interval,
			  reconnect_interval == 1 ? "": "s");
		if (!sane_sleep(inst, reconnect_interval))
			CLEANUP_WITH(ISC_R_SHUTTINGDOWN);
		handle_connection_error(inst, conn, ISC_TRUE);
	}

	ldap_sync = ldap_sync_initialize(NULL);
	if (ldap_sync == NULL) {
		log_error("cannot initialize LDAP syncrepl context");
		CLEANUP_WITH(ISC_R_NOMEMORY);
	}
	ZERO_PTR(ldap_sync);

	CHECK(setting_get_str("base", settings, &base));
	ldap_sync->ls_base = ldap_strdup(base);
	if (ldap_sync->ls_base == NULL)
		CLEANUP_WITH(ISC_R_NOMEMORY);
	ldap_sync->ls_scope = LDAP_SCOPE_SUBTREE;
	ldap_sync->ls_filter = ldap_strdup("(|(objectClass=idnsConfigObject)"
					   "  (objectClass=idnsZone)"
					   "  (objectClass=idnsForwardZone)"
					   "  (objectClass=idnsRecord))");
	if (ldap_sync->ls_filter == NULL)
		CLEANUP_WITH(ISC_R_NOMEMORY);
	ldap_sync->ls_timeout = -1; /* sync_poll is blocking */
	ldap_sync->ls_ld = conn->handle;
	/* This is a hack: ldap_sync_destroy() will call ldap_unbind().
	 * We have to ensure that unbind() will not be called twice! */
	conn->handle = NULL;
	ldap_sync->ls_search_entry = ldap_sync_search_entry;
	ldap_sync->ls_search_reference = ldap_sync_search_reference;
	ldap_sync->ls_intermediate = ldap_sync_intermediate;
	ldap_sync->ls_search_result = ldap_sync_search_result;
	ldap_sync->ls_private = inst;

	result = ISC_R_SUCCESS;
	*ldap_syncp = ldap_sync;

cleanup:
	if (result != ISC_R_SUCCESS)
		ldap_sync_cleanup(&ldap_sync);

	return result;
}


/*
 * NOTE:
 * Every blocking call in syncrepl_watcher thread must be preemptible.
 */
static isc_threadresult_t
ldap_syncrepl_watcher(isc_threadarg_t arg)
{
	ldap_instance_t *inst = (ldap_instance_t *)arg;
	ldap_connection_t *conn = NULL;
	int ret;
	isc_result_t result;
	sigset_t sigset;
	ldap_sync_t *ldap_sync = NULL;
	const char *err_hint = "";

	log_debug(1, "Entering ldap_syncrepl_watcher");

	install_usr1handler();

	/*
	 * By default, BIND sets threads to accept signals only via
	 * sigwait(). However we need to use SIGUSR1 to interrupt
	 * watcher from waiting inside ldap_result so enable
	 * asynchronous delivering of SIGUSR1.
	 */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGUSR1);
	ret = pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
	/* pthread_sigmask fails only due invalid args */
	RUNTIME_CHECK(ret == 0);

	/* Pick connection, one is reserved purely for this thread */
	CHECK(ldap_pool_getconnection(inst->pool, &conn));

	while (!inst->exiting) {
		ldap_sync_cleanup(&ldap_sync);
		result = ldap_sync_prepare(inst, inst->global_settings,
					   conn, &ldap_sync);
		if (result != ISC_R_SUCCESS) {
			log_error_r("ldap_sync_prepare() failed, retrying "
				    "in 1 second");
			sane_sleep(inst, 1);
			continue;
		}

		log_debug(1, "Sending initial syncrepl lookup");
		ret = ldap_sync_init(ldap_sync, LDAP_SYNC_REFRESH_AND_PERSIST);
		/* TODO: error handling, set tainted flag & do full reload? */
		if (ret != LDAP_SUCCESS) {
			if (ret == LDAP_UNAVAILABLE_CRITICAL_EXTENSION)
				err_hint = ": is RFC 4533 supported by LDAP server?";
			else
				err_hint = "";

			log_ldap_error(ldap_sync->ls_ld, "unable to start SyncRepl "
					"session%s", err_hint);
			conn->handle = NULL;
			continue;
		}

		while (!inst->exiting && ret == LDAP_SUCCESS) {
			ret = ldap_sync_poll(ldap_sync);
			if (!inst->exiting && ret != LDAP_SUCCESS) {
				log_ldap_error(ldap_sync->ls_ld,
					       "ldap_sync_poll() failed");
				/* force reconnect in sync_prepare */
				conn->handle = NULL;
			}
		}
	}

cleanup:
	log_debug(1, "Ending ldap_syncrepl_watcher");
	ldap_sync_cleanup(&ldap_sync);
	ldap_pool_putconnection(inst->pool, &conn);

	return (isc_threadresult_t)0;
}

settings_set_t *
ldap_instance_getsettings_local(ldap_instance_t *ldap_inst)
{
	return ldap_inst->local_settings;
}

const char *
ldap_instance_getdbname(ldap_instance_t *ldap_inst)
{
	return ldap_inst->db_name;
}

zone_register_t *
ldap_instance_getzr(ldap_instance_t *ldap_inst)
{
	return ldap_inst->zone_register;
}
