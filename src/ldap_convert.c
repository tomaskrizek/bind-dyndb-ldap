/*
 * Authors: Martin Nagy <mnagy@redhat.com>
 *
 * Copyright (C) 2009  Red Hat
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

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/util.h>
#include <isc/string.h>

#include <dns/name.h>
#include <dns/result.h>
#include <dns/types.h>

#define LDAP_DEPRECATED 1
#include <ldap.h>

#include <errno.h>
#include <strings.h>
#include <ctype.h>

#include "str.h"
#include "ldap_convert.h"
#include "log.h"
#include "util.h"
#include "zone_register.h"

/**
 * Convert LDAP DN to absolute DNS names.
 *
 * @param[in]  dn     LDAP DN with one or two idnsName components at the
 *                    beginning.
 * @param[out] target Absolute DNS name derived from the first two idnsNames.
 * @param[out] origin Absolute DNS name derived from the last idnsName
 *                    component of DN, i.e. zone. Can be NULL.
 *
 * @code
 * Examples:
 * dn = "idnsName=foo.bar, idnsName=example.org., cn=dns, dc=example, dc=org"
 * target = "foo.bar.example.org."
 * origin = "example.org."
 *
 * dn = "idnsname=89, idnsname=4.34.10.in-addr.arpa, cn=dns, dc=example, dc=org"
 * target = "89.4.34.10.in-addr.arpa."
 * origin = "4.34.10.in-addr.arpa."
 *
 * dn = "idnsname=third.test., idnsname=test., cn=dns, dc=example, dc=org"
 * target = "third.test."
 * origin = "test."
 * @endcode
 */
isc_result_t
dn_to_dnsname(isc_mem_t *mctx, const char *dn_str, dns_name_t *target,
	      dns_name_t *otarget)
{
	LDAPDN dn = NULL;
	LDAPRDN rdn = NULL;
	LDAPAVA *attr = NULL;
	int idx;
	int ret;

	DECLARE_BUFFERED_NAME(name);
	DECLARE_BUFFERED_NAME(origin);
	isc_buffer_t name_buf;
	isc_buffer_t origin_buf;
	isc_result_t result;

	REQUIRE(dn_str != NULL);
	REQUIRE(target != NULL);

	INIT_BUFFERED_NAME(name);
	INIT_BUFFERED_NAME(origin);
	isc_buffer_initnull(&name_buf);
	isc_buffer_initnull(&origin_buf);

	/* Example DN: cn=a+sn=b, ou=people */

	ret = ldap_str2dn(dn_str, &dn, LDAP_DN_FORMAT_LDAPV3);
	if (ret != LDAP_SUCCESS || dn == NULL) {
		log_bug("ldap_str2dn failed: %u", ret);
		CLEANUP_WITH(ISC_R_UNEXPECTED);
	}

	/* iterate over DN components: e.g. cn=a+sn=b */
	for (idx = 0; dn[idx] != NULL; idx++) {
		rdn = dn[idx];

		/* "iterate" over RDN components: e.g. cn=a */
		INSIST(rdn[0] != NULL); /* RDN without (attr=value)?! */
		if (rdn[1] != NULL) {
			log_bug("multi-valued RDNs are not supported");
			CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
		}

		/* attribute in current RDN component */
		attr = rdn[0];
		if ((attr->la_flags & LDAP_AVA_STRING) == 0) {
			log_error("non-string attribute detected: position %u",
				  idx);
			CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
		}

		if (strncasecmp("idnsName", attr->la_attr.bv_val,
				attr->la_attr.bv_len) == 0) {
			if (idx == 0) {
				isc_buffer_init(&name_buf,
						attr->la_value.bv_val,
						attr->la_value.bv_len);
				isc_buffer_add(&name_buf,
					       attr->la_value.bv_len);
			} else if (idx == 1) {
				isc_buffer_init(&origin_buf,
						attr->la_value.bv_val,
						attr->la_value.bv_len);
				isc_buffer_add(&origin_buf,
					       attr->la_value.bv_len);
			} else { /* more than two idnsNames?! */
				break;
			}
		} else { /* no match - idx holds position */
			break;
		}
	}

	/* filter out unsupported cases */
	if (idx <= 0) {
		log_error("no idnsName component found in DN");
		CLEANUP_WITH(ISC_R_UNEXPECTEDEND);
	} else if (idx == 1) { /* zone only */
		CHECK(dns_name_copy(dns_rootname, &origin, NULL));
		CHECK(dns_name_fromtext(&name, &name_buf, dns_rootname, 0, NULL));
	} else if (idx == 2) { /* owner and zone */
		CHECK(dns_name_fromtext(&origin, &origin_buf, dns_rootname, 0,
					NULL));
		CHECK(dns_name_fromtext(&name, &name_buf, &origin, 0, NULL));
		if (dns_name_issubdomain(&name, &origin) == ISC_FALSE) {
			log_error("out-of-zone data: first idnsName is not a "
				  "subdomain of the other");
			CLEANUP_WITH(DNS_R_BADOWNERNAME);
		} else if (dns_name_equal(&name, &origin) == ISC_TRUE) {
			log_error("attempt to redefine zone apex: first "
				  "idnsName equals to zone name");
			CLEANUP_WITH(DNS_R_BADOWNERNAME);
		}
	} else {
		log_error("unsupported number of idnsName components in DN: "
			  "%u components found", idx);
		CLEANUP_WITH(ISC_R_NOTIMPLEMENTED);
	}

cleanup:
	if (result == ISC_R_SUCCESS)
		result = dns_name_dupwithoffsets(&name, mctx, target);
	else
		log_error_r("failed to convert DN '%s' to DNS name", dn_str);

	if (result == ISC_R_SUCCESS && otarget != NULL)
		result = dns_name_dupwithoffsets(&origin, mctx, otarget);

	if (result != ISC_R_SUCCESS) {
		if (dns_name_dynamic(target))
			dns_name_free(target, mctx);
		if (otarget) {
			if (dns_name_dynamic(otarget))
				dns_name_free(otarget, mctx);
		}
	}

	if (dn != NULL)
		ldap_dnfree(dn);

	return result;
}

/**
 * WARNING! This function is used to mangle input from network
 *          and it is security sensitive.
 *
 * Convert a string from DNS escaping to LDAP escaping.
 * The Input string dns_str is expected to be the result of dns_name_tostring().
 * The DNS label can contain any binary data as described in
 * http://tools.ietf.org/html/rfc2181#section-11 .
 *
 * DNS escaping uses 2 forms: (see dns_name_totext2() in bind/lib/dns/name.c)
 *     form "\123" = ASCII value 123 (decimal)
 *     form "\$" = character '$' is escaped with '\'
 *     WARNING! Some characters are not escaped at all (e.g. ',').
 *
 * LDAP escaping users form "\7b"  = ASCII value 7b (hexadecimal)
 *
 * Input  (DNS escaped)  example: \$.\255_aaa,bbb\127\000ccc.555.ddd-eee
 * Output (LDAP escaped) example: \24.\ff_aaa\2cbbb\7f\00ccc.555.ddd-eee
 *
 * The DNS to text functions from ISC libraries do not convert certain
 * characters (e.g. ","). This function converts \123 form to \7b form in all
 * cases. Other characters (not escaped by ISC libraries) will be additionally
 * converted to the LDAP escape form.
 * Input characters [a-zA-Z0-9._-] are left in raw ASCII form.
 *
 * If dns_str consists only of the characters in the [a-zA-Z0-9._-] set, it
 * will be checked & copied to the output buffer, without any additional escaping.
 */
isc_result_t
dns_to_ldap_dn_escape(isc_mem_t *mctx, const char * const dns_str, char ** ldap_name) {
	isc_result_t result = ISC_R_FAILURE;
	char * esc_name = NULL;
	int idx_symb_first = -1; /* index of first "nice" printable symbol in dns_str */
	int dns_idx = 0;
	int esc_idx = 0;

	REQUIRE(dns_str != NULL);
	REQUIRE(ldap_name != NULL && *ldap_name == NULL);

	int dns_str_len = strlen(dns_str);

	/**
	 * In worst case each symbol from DNS dns_str will be represented
	 * as "\xy" in ldap_name. (xy are hexadecimal digits)
	 */
	CHECKED_MEM_ALLOCATE(mctx, *ldap_name, 3 * dns_str_len + 1);
	esc_name = *ldap_name;

	for (dns_idx = 0; dns_idx < dns_str_len; dns_idx++) {
		if (isalnum(dns_str[dns_idx]) || dns_str[dns_idx] == '.'
				|| dns_str[dns_idx] == '-' || dns_str[dns_idx] == '_' ) {
			if (idx_symb_first == -1)
				idx_symb_first = dns_idx;
			continue;
		} else { /* some not very nice symbols */
			int ascii_val;
			if (idx_symb_first != -1) { /* copy previous nice part */
				int length_ok = dns_idx - idx_symb_first;
				memcpy(esc_name + esc_idx, dns_str + idx_symb_first, length_ok);
				esc_idx += length_ok;
				idx_symb_first = -1;
			}
			if (dns_str[dns_idx] != '\\') { /* not nice raw value, e.g. ',' */
				ascii_val = dns_str[dns_idx];
			} else { /* DNS escaped value, it starts with '\' */
				if (!(dns_idx + 1 < dns_str_len)) {
					CHECK(DNS_R_BADESCAPE); /* this problem should never happen */
				}
				if (isdigit(dns_str[dns_idx + 1])) { /* \123 decimal format */
					/* check if input length <= expected size */
					if (!(dns_idx + 3 < dns_str_len)) {
						CHECK(DNS_R_BADESCAPE); /* this problem should never happen */
					}
					ascii_val = 100 * (dns_str[dns_idx + 1] - '0')
							+ 10 * (dns_str[dns_idx + 2] - '0')
							+ (dns_str[dns_idx + 3] - '0');
					dns_idx += 3;
				} else { /* \$ single char format */
					ascii_val = dns_str[dns_idx + 1];
					dns_idx += 1;
				}
			}
			/* LDAP uses \xy escaping. "xy" represent two hexadecimal digits.*/
			/* TODO: optimize to bit mask & rotate & dec->hex table? */
			CHECK(isc_string_printf(esc_name + esc_idx, 4, "\\%02x", ascii_val));
			esc_idx += 3; /* isc_string_printf wrote 4 bytes including '\0' */
		}
	}
	if (idx_symb_first != -1) { /* copy last nice part */
		int length_ok = dns_idx - idx_symb_first;
		memcpy(esc_name + esc_idx, dns_str + idx_symb_first, dns_idx - idx_symb_first);
		esc_idx += length_ok;
	}
	esc_name[esc_idx] = '\0';
	return ISC_R_SUCCESS;

cleanup:
	if (result == DNS_R_BADESCAPE)
		log_bug("improperly escaped DNS string: '%s'", dns_str);

	if (*ldap_name) {
		isc_mem_free(mctx, *ldap_name);
		*ldap_name = NULL;
	}
	return result;
}

isc_result_t
dnsname_to_dn(zone_register_t *zr, dns_name_t *name, ld_string_t *target)
{
	isc_result_t result;
	int label_count;
	const char *zone_dn = NULL;
	char *dns_str = NULL;
	char *escaped_name = NULL;

	REQUIRE(zr != NULL);
	REQUIRE(name != NULL);
	REQUIRE(target != NULL);

	isc_mem_t * mctx = zr_get_mctx(zr);

	/* Find the DN of the zone we belong to. */
	{
		DECLARE_BUFFERED_NAME(zone);
		int dummy;
		unsigned int common_labels;

		INIT_BUFFERED_NAME(zone);

		CHECK(zr_get_zone_dn(zr, name, &zone_dn, &zone));

		dns_name_fullcompare(name, &zone, &dummy, &common_labels);
		label_count = dns_name_countlabels(name) - common_labels;
	}

	str_clear(target);
	if (label_count > 0) {
		dns_name_t labels;

		dns_name_init(&labels, NULL);
		dns_name_getlabelsequence(name, 0, label_count, &labels);
		CHECK(dns_name_tostring(&labels, &dns_str, mctx));

		CHECK(dns_to_ldap_dn_escape(mctx, dns_str, &escaped_name));
		CHECK(str_cat_char(target, "idnsName="));
		CHECK(str_cat_char(target, escaped_name));
		/* 
		 * Modification of following line can affect modify_ldap_common().
		 * See line with: char *zone_dn = strstr(str_buf(owner_dn),", ") + 1;  
		 */
		CHECK(str_cat_char(target, ", "));
	}
	CHECK(str_cat_char(target, zone_dn));

cleanup:
	if (dns_str)
		isc_mem_free(mctx, dns_str);
	if (escaped_name)
		isc_mem_free(mctx, escaped_name);
	return result;
}

/**
 * Convert attribute name to dns_rdatatype.
 *
 * @param[in]  ldap_attribute String with attribute name terminated by \0.
 * @param[out] rdtype
 */
isc_result_t
ldap_attribute_to_rdatatype(const char *ldap_attribute, dns_rdatatype_t *rdtype)
{
	isc_result_t result;
	unsigned len;
	isc_consttextregion_t region;

	len = strlen(ldap_attribute);
	if (len <= LDAP_RDATATYPE_SUFFIX_LEN)
		return ISC_R_UNEXPECTEDEND;

	/* Does attribute name end with RECORD_SUFFIX? */
	if (strcasecmp(ldap_attribute + len - LDAP_RDATATYPE_SUFFIX_LEN,
		       LDAP_RDATATYPE_SUFFIX))
		return ISC_R_UNEXPECTED;

	region.base = ldap_attribute;
	region.length = len - LDAP_RDATATYPE_SUFFIX_LEN;
	result = dns_rdatatype_fromtext(rdtype, (isc_textregion_t *)&region);
	if (result != ISC_R_SUCCESS)
		log_error_r("dns_rdatatype_fromtext() failed for attribute '%s'",
			    ldap_attribute);

	return result;
}

isc_result_t
rdatatype_to_ldap_attribute(dns_rdatatype_t rdtype, char *target,
			    unsigned int size)
{
	isc_result_t result;
	char rdtype_str[DNS_RDATATYPE_FORMATSIZE];

	dns_rdatatype_format(rdtype, rdtype_str, DNS_RDATATYPE_FORMATSIZE);
	CHECK(isc_string_copy(target, size, rdtype_str));
	CHECK(isc_string_append(target, size, LDAP_RDATATYPE_SUFFIX));

	return ISC_R_SUCCESS;

cleanup:
	return result;
}

