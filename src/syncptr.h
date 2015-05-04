/*
 * Copyright (C) 2009-2015  bind-dyndb-ldap authors; see COPYING for license
 */

#ifndef SRC_SYNCPTR_H_
#define SRC_SYNCPTR_H_

#include "util.h"

isc_result_t ATTR_NONNULLS ATTR_CHECKRESULT
ldap_sync_ptr(isc_mem_t *mctx, ldap_instance_t *ldap_inst, dns_zt_t * zonetable,
	      zone_register_t *zone_register, dns_name_t *a_name, const int af,
	      const char *ip_str, const int mod_op);

#endif /* SRC_SYNCPTR_H_ */