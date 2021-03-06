/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

#include "gnutls_int.h"
#include <common.h>
#include <x509.h>
#include <x509_int.h>
#include <num.h>
#include "errors.h"
#include <extras/randomart.h>
#include <c-ctype.h>
#include <gnutls-idna.h>

#define addf _gnutls_buffer_append_printf
#define adds _gnutls_buffer_append_str

static void print_dn(gnutls_buffer_st * str, const char *prefix,
		     const gnutls_datum_t * raw)
{
	gnutls_x509_dn_t dn = NULL;
	gnutls_datum_t output = { NULL, 0 };
	int ret;

	ret = gnutls_x509_dn_init(&dn);
	if (ret < 0) {
		addf(str, "%s: [error]\n", prefix);
		return;
	}

	ret = gnutls_x509_dn_import(dn, raw);
	if (ret < 0) {
		addf(str, "%s: [error]\n", prefix);
		goto cleanup;
	}

	ret = gnutls_x509_dn_get_str(dn, &output);
	if (ret < 0) {
		addf(str, "%s: [error]\n", prefix);
		goto cleanup;
	}

	addf(str, "%s: %s\n", prefix, output.data);

 cleanup:
	gnutls_x509_dn_deinit(dn);
	gnutls_free(output.data);
}

static void print_raw(gnutls_buffer_st * str, const char *prefix,
		      const gnutls_datum_t * raw)
{
	char data[512];
	size_t data_size;
	int ret;

	if (raw->data == NULL || raw->size == 0)
		return;

	data_size = sizeof(data);
	ret = gnutls_hex_encode(raw, data, &data_size);
	if (ret < 0) {
		addf(str, "%s: [error]\n", prefix);
		return;
	}

	addf(str, "%s: %s\n", prefix, data);
}

static void print_pkcs7_info(gnutls_pkcs7_signature_info_st * info,
			     gnutls_buffer_st * str,
			     gnutls_certificate_print_formats_t format)
{
	unsigned i;
	char *oid;
	gnutls_datum_t data;
	char prefix[128];
	char s[42];
	size_t max;
	int ret;

	print_dn(str, "\tSigner's issuer DN", &info->issuer_dn);
	print_raw(str, "\tSigner's serial", &info->signer_serial);
	print_raw(str, "\tSigner's issuer key ID", &info->issuer_keyid);
	if (info->signing_time != -1) {
		struct tm t;
		if (gmtime_r(&info->signing_time, &t) == NULL) {
			addf(str, "error: gmtime_r (%ld)\n",
			     (unsigned long)info->signing_time);
		} else {
			max = sizeof(s);
			if (strftime(s, max, "%a %b %d %H:%M:%S UTC %Y", &t) ==
			    0) {
				addf(str, "error: strftime (%ld)\n",
				     (unsigned long)info->signing_time);
			} else {
				addf(str, "\tSigning time: %s\n", s);
			}
		}
	}

	addf(str, "\tSignature Algorithm: %s\n",
	     gnutls_sign_get_name(info->algo));

	if (format == GNUTLS_CRT_PRINT_FULL) {
		if (info->signed_attrs) {
			for (i = 0;; i++) {
				ret =
				    gnutls_pkcs7_get_attr(info->signed_attrs, i,
							  &oid, &data, 0);
				if (ret < 0)
					break;
				if (i == 0)
					addf(str, "\tSigned Attributes:\n");

				snprintf(prefix, sizeof(prefix), "\t\t%s", oid);
				print_raw(str, prefix, &data);
				gnutls_free(data.data);
			}
		}
		if (info->unsigned_attrs) {
			for (i = 0;; i++) {
				ret =
				    gnutls_pkcs7_get_attr(info->unsigned_attrs,
							  i, &oid, &data, 0);
				if (ret < 0)
					break;
				if (i == 0)
					addf(str, "\tUnsigned Attributes:\n");

				snprintf(prefix, sizeof(prefix), "\t\t%s", oid);
				print_raw(str, prefix, &data);
				gnutls_free(data.data);
			}
		}
	}
	adds(str, "\n");
}

/**
 * gnutls_pkcs7_crt_print:
 * @pkcs7: The PKCS7 struct to be printed
 * @format: Indicate the format to use
 * @out: Newly allocated datum with null terminated string.
 *
 * This function will pretty print a signed PKCS #7 structure, suitable for
 * display to a human.
 *
 * Currently the supported formats are %GNUTLS_CRT_PRINT_FULL and
 * %GNUTLS_CRT_PRINT_COMPACT.
 *
 * The output @out needs to be deallocated using gnutls_free().
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 **/
int gnutls_pkcs7_print(gnutls_pkcs7_t pkcs7,
		       gnutls_certificate_print_formats_t format,
		       gnutls_datum_t * out)
{
	unsigned i, count;
	int ret;
	gnutls_pkcs7_signature_info_st info;
	gnutls_buffer_st str;

	_gnutls_buffer_init(&str);

	for (i = 0;; i++) {
		if (i == 0)
			addf(&str, "Signers:\n");

		ret = gnutls_pkcs7_get_signature_info(pkcs7, i, &info);
		if (ret < 0)
			break;

		print_pkcs7_info(&info, &str, format);
		gnutls_pkcs7_signature_info_deinit(&info);
	}

	if (format == GNUTLS_CRT_PRINT_FULL) {
		gnutls_datum_t data, b64;

		count = gnutls_pkcs7_get_crt_count(pkcs7);

		if (count > 0) {
			addf(&str, "Number of certificates: %u\n\n",
			     count);

			for (i = 0; i < count; i++) {
				ret =
				    gnutls_pkcs7_get_crt_raw2(pkcs7, i, &data);
				if (ret < 0) {
					addf(&str,
					     "Error: cannot print certificate %d\n",
					     i);
					continue;
				}

				ret =
				    gnutls_pem_base64_encode_alloc
				    ("CERTIFICATE", &data, &b64);
				if (ret < 0) {
					gnutls_free(data.data);
					continue;
				}

				adds(&str, (char*)b64.data);
				adds(&str, "\n");
				gnutls_free(b64.data);
				gnutls_free(data.data);
			}
		}

		count = gnutls_pkcs7_get_crl_count(pkcs7);
		if (count > 0) {
			addf(&str, "Number of CRLs: %u\n\n", count);

			for (i = 0; i < count; i++) {
				ret =
				    gnutls_pkcs7_get_crl_raw2(pkcs7, i, &data);
				if (ret < 0) {
					addf(&str,
					     "Error: cannot print certificate %d\n",
					     i);
					continue;
				}

				ret =
				    gnutls_pem_base64_encode_alloc("X509 CRL",
								   &data, &b64);
				if (ret < 0) {
					gnutls_free(data.data);
					continue;
				}

				adds(&str, (char*)b64.data);
				adds(&str, "\n");
				gnutls_free(b64.data);
				gnutls_free(data.data);
			}
		}
	}

	return _gnutls_buffer_to_datum(&str, out, 1);
}
