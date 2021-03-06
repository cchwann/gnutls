/*
 * Copyright (C) 2003-2012 Free Software Foundation, Inc.
 *
 * This file is part of GnuTLS.
 *
 * GnuTLS is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/openpgp.h>
#include <gnutls/pkcs12.h>
#include <gnutls/pkcs11.h>
#include <gnutls/abstract.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <common.h>
#include "certtool-common.h"
#include "certtool-args.h"
#include "certtool-cfg.h"
#include "common.h"
#include <minmax.h>

/* Gnulib portability files. */
#include <read-file.h>

unsigned char *lbuffer = NULL;
unsigned long lbuffer_size = 0;

static unsigned long file_size(FILE *fp)
{
	unsigned long size;
	long cur = ftell(fp);

	if (cur == -1)
		return 0;

	if (fseek(fp, 0, SEEK_END) == -1)
		return 0;

	size = ftell(fp);
	fseek(fp, cur, SEEK_SET);
	return size;
}

void fix_lbuffer(unsigned long size)
{
	if (lbuffer_size == 0 || lbuffer == NULL) {
		if (size == 0)
			lbuffer_size = 64*1024;
		else
			lbuffer_size = MAX(64*1024,size+1);
		lbuffer = malloc(lbuffer_size);
	} else if (size > lbuffer_size) {
		lbuffer_size = MAX(64*1024,size+1);
		lbuffer = realloc(lbuffer, lbuffer_size);
	}

	if (lbuffer == NULL) {
		fprintf(stderr, "memory error");
		exit(1);
	}
}

FILE *safe_open_rw(const char *file, int privkey_op)
{
	mode_t omask = 0;
	FILE *fh;

	if (privkey_op != 0) {
		omask = umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	}

	fh = fopen(file, "wb");

	if (privkey_op != 0) {
		umask(omask);
	}

	return fh;
}

gnutls_datum_t *load_secret_key(int mand, common_info_st * info)
{
	static char raw_key[64];
	size_t raw_key_size = sizeof(raw_key);
	static gnutls_datum_t key;
	gnutls_datum_t hex_key;
	int ret;

	if (info->verbose)
		fprintf(stderr, "Loading secret key...\n");

	if (info->secret_key == NULL) {
		if (mand) {
			fprintf(stderr, "missing --secret-key\n");
			exit(1);
		} else
			return NULL;
	}

	hex_key.data = (void *) info->secret_key;
	hex_key.size = strlen(info->secret_key);

	ret = gnutls_hex_decode(&hex_key, raw_key, &raw_key_size);
	if (ret < 0) {
		fprintf(stderr, "hex_decode: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	key.data = (void *) raw_key;
	key.size = raw_key_size;

	return &key;
}

const char *get_password(common_info_st * cinfo, unsigned int *flags,
			 int confirm)
{
	const char *p;

	if (cinfo->null_password) {
		if (flags)
			*flags |= GNUTLS_PKCS_NULL_PASSWORD;
		return NULL;
	} else if (cinfo->password) {
		p = cinfo->password;
	} else {
		if (confirm)
			p = get_confirmed_pass(true);
		else
			p = get_pass();
	}

	if ((p == NULL || p[0] == 0) && flags && !cinfo->empty_password)
		*flags |= GNUTLS_PKCS_PLAIN;

	return p;
}

static gnutls_privkey_t _load_privkey(gnutls_datum_t * dat,
				      common_info_st * info)
{
	int ret;
	gnutls_privkey_t key;
	unsigned int flags = 0;
	const char *pass;

	ret = gnutls_privkey_init(&key);
	if (ret < 0) {
		fprintf(stderr, "privkey_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	ret =
	    gnutls_privkey_import_x509_raw(key, dat, info->incert_format,
					   NULL, 0);
	if (ret == GNUTLS_E_DECRYPTION_FAILED) {
		pass = get_password(info, &flags, 0);
		ret =
		    gnutls_privkey_import_x509_raw(key, dat,
						   info->incert_format,
						   pass, flags);
	}

	if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR) {
		fprintf(stderr,
			"import error: could not find a valid PEM header; "
			"check if your key is PKCS #12 encoded\n");
		exit(1);
	}

	if (ret < 0) {
		fprintf(stderr, "importing --load-privkey: %s: %s\n",
			info->privkey, gnutls_strerror(ret));
		exit(1);
	}

	return key;
}

static gnutls_privkey_t _load_url_privkey(const char *url)
{
	int ret;
	gnutls_privkey_t key;

	ret = gnutls_privkey_init(&key);
	if (ret < 0) {
		fprintf(stderr, "privkey_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	ret = gnutls_privkey_import_url(key, url, 0);
	if (ret < 0) {
		fprintf(stderr, "importing key: %s: %s\n",
			url, gnutls_strerror(ret));
		exit(1);
	}

	return key;
}

static gnutls_pubkey_t _load_url_pubkey(const char *url)
{
	int ret;
	gnutls_pubkey_t pubkey;
	unsigned int obj_flags = 0;

	ret = gnutls_pubkey_init(&pubkey);
	if (ret < 0) {
		fprintf(stderr, "Error in %s:%d: %s\n", __func__, __LINE__,
			gnutls_strerror(ret));
		exit(1);
	}

	ret = gnutls_pubkey_import_url(pubkey, url, obj_flags);
	if (ret < 0) {
		fprintf(stderr, "Error in %s:%d: %s: %s\n", __func__,
			__LINE__, gnutls_strerror(ret), url);
		exit(1);
	}

	return pubkey;
}

/* Load the private key.
 * @mand should be non zero if it is required to read a private key.
 */
gnutls_privkey_t load_private_key(int mand, common_info_st * info)
{
	gnutls_privkey_t key;
	gnutls_datum_t dat;
	size_t size;

	if (!info->privkey && !mand)
		return NULL;

	if (info->privkey == NULL) {
		fprintf(stderr, "missing --load-privkey\n");
		exit(1);
	}

	if (gnutls_url_is_supported(info->privkey) != 0)
		return _load_url_privkey(info->privkey);

	dat.data = (void *) read_binary_file(info->privkey, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "reading --load-privkey: %s\n",
			info->privkey);
		exit(1);
	}

	key = _load_privkey(&dat, info);

	free(dat.data);

	return key;
}

/* Load the private key.
 * @mand should be non zero if it is required to read a private key.
 */
gnutls_x509_privkey_t
load_x509_private_key(int mand, common_info_st * info)
{
	gnutls_x509_privkey_t key;
	int ret;
	gnutls_datum_t dat;
	size_t size;
	unsigned int flags = 0;
	const char *pass;

	if (!info->privkey && !mand)
		return NULL;

	if (info->privkey == NULL) {
		fprintf(stderr, "missing --load-privkey\n");
		exit(1);
	}

	ret = gnutls_x509_privkey_init(&key);
	if (ret < 0) {
		fprintf(stderr, "privkey_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	dat.data = (void *) read_binary_file(info->privkey, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "reading --load-privkey: %s\n",
			info->privkey);
		exit(1);
	}

	if (info->pkcs8) {
		pass = get_password(info, &flags, 0);
		ret =
		    gnutls_x509_privkey_import_pkcs8(key, &dat,
						     info->incert_format,
						     pass, flags);
	} else {
		ret =
		    gnutls_x509_privkey_import2(key, &dat,
						info->incert_format, NULL,
						0);
		if (ret == GNUTLS_E_DECRYPTION_FAILED) {
			pass = get_password(info, &flags, 0);
			ret =
			    gnutls_x509_privkey_import2(key, &dat,
							info->
							incert_format,
							pass, flags);
		}
	}

	free(dat.data);

	if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR) {
		fprintf(stderr,
			"import error: could not find a valid PEM header; "
			"check if your key is PEM encoded\n");
		exit(1);
	}

	if (ret < 0) {
		fprintf(stderr, "importing --load-privkey: %s: %s\n",
			info->privkey, gnutls_strerror(ret));
		exit(1);
	}

	return key;
}


/* Loads the certificate
 * If mand is non zero then a certificate is mandatory. Otherwise
 * null will be returned if the certificate loading fails.
 */
gnutls_x509_crt_t load_cert(int mand, common_info_st * info)
{
	gnutls_x509_crt_t *crt;
	gnutls_x509_crt_t ret_crt;
	size_t size, i;

	crt = load_cert_list(mand, &size, info);
	if (crt) {
		ret_crt = crt[0];
		for (i=1;i<size;i++)
			gnutls_x509_crt_deinit(crt[i]);
		gnutls_free(crt);
		return ret_crt;
	}

	return NULL;
}

/* Loads a certificate list
 */
gnutls_x509_crt_t *load_cert_list(int mand, size_t * crt_size,
				  common_info_st * info)
{
	FILE *fd;
	static gnutls_x509_crt_t *crt;
	int ret;
	gnutls_datum_t dat;
	unsigned size;
	unsigned int crt_max;

	*crt_size = 0;
	if (info->verbose)
		fprintf(stderr, "Loading certificate list...\n");

	if (info->cert == NULL) {
		if (mand) {
			fprintf(stderr, "missing --load-certificate\n");
			exit(1);
		} else
			return NULL;
	}

	fd = fopen(info->cert, "r");
	if (fd == NULL) {
		fprintf(stderr, "Could not open %s\n", info->cert);
		exit(1);
	}

	fix_lbuffer(file_size(fd));

	size = fread(lbuffer, 1, lbuffer_size - 1, fd);
	lbuffer[size] = 0;

	fclose(fd);

	dat.data = (void *) lbuffer;
	dat.size = size;

	ret = gnutls_x509_crt_list_import2(&crt, &crt_max, &dat, GNUTLS_X509_FMT_PEM, 0);
	if (ret < 0) {
		fprintf(stderr, "Error loading certificates: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	*crt_size = crt_max;

	if (info->verbose)
		fprintf(stderr, "Loaded %d certificates.\n",
			(int) crt_max);

	return crt;
}

/* Loads a CRL list
 */
gnutls_x509_crl_t *load_crl_list(int mand, size_t * crl_size,
				  common_info_st * info)
{
	FILE *fd;
	static gnutls_x509_crl_t *crl;
	unsigned int crl_max;
	int ret;
	gnutls_datum_t dat;
	size_t size;

	*crl_size = 0;
	if (info->verbose)
		fprintf(stderr, "Loading CRL list...\n");

	if (info->crl == NULL) {
		if (mand) {
			fprintf(stderr, "missing --load-crl\n");
			exit(1);
		} else
			return NULL;
	}

	fd = fopen(info->crl, "r");
	if (fd == NULL) {
		fprintf(stderr, "Could not open %s\n", info->crl);
		exit(1);
	}

	fix_lbuffer(file_size(fd));

	size = fread(lbuffer, 1, lbuffer_size - 1, fd);
	lbuffer[size] = 0;

	fclose(fd);

	dat.data = (void *) lbuffer;
	dat.size = size;

	ret = gnutls_x509_crl_list_import2(&crl, &crl_max, &dat, GNUTLS_X509_FMT_PEM, 0);
	if (ret < 0) {
		fprintf(stderr, "Error loading CRLs: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	*crl_size = crl_max;

	if (info->verbose)
		fprintf(stderr, "Loaded %d CRLs.\n",
			(int) *crl_size);

	return crl;
}

/* Load the Certificate Request.
 */
gnutls_x509_crq_t load_request(common_info_st * info)
{
	gnutls_x509_crq_t crq;
	int ret;
	gnutls_datum_t dat;
	size_t size;

	if (!info->request)
		return NULL;

	ret = gnutls_x509_crq_init(&crq);
	if (ret < 0) {
		fprintf(stderr, "crq_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	dat.data = (void *) read_binary_file(info->request, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "reading --load-request: %s\n",
			info->request);
		exit(1);
	}

	ret = gnutls_x509_crq_import(crq, &dat, info->incert_format);
	if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR) {
		fprintf(stderr,
			"import error: could not find a valid PEM header\n");
		exit(1);
	}

	free(dat.data);
	if (ret < 0) {
		fprintf(stderr, "importing --load-request: %s: %s\n",
			info->request, gnutls_strerror(ret));
		exit(1);
	}
	return crq;
}

/* Load the CA's private key.
 */
gnutls_privkey_t load_ca_private_key(common_info_st * info)
{
	gnutls_privkey_t key;
	gnutls_datum_t dat;
	size_t size;

	if (info->ca_privkey == NULL) {
		fprintf(stderr, "missing --load-ca-privkey\n");
		exit(1);
	}

	if (gnutls_url_is_supported(info->ca_privkey) != 0)
		return _load_url_privkey(info->ca_privkey);

	dat.data = (void *) read_binary_file(info->ca_privkey, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "reading --load-ca-privkey: %s\n",
			info->ca_privkey);
		exit(1);
	}

	key = _load_privkey(&dat, info);

	free(dat.data);

	return key;
}

/* Loads the CA's certificate
 */
gnutls_x509_crt_t load_ca_cert(unsigned mand, common_info_st * info)
{
	gnutls_x509_crt_t crt;
	int ret;
	gnutls_datum_t dat;
	size_t size;

	if (mand == 0 && info->ca == NULL) {
		return NULL;
	}

	if (info->ca == NULL) {
		fprintf(stderr, "missing --load-ca-certificate\n");
		exit(1);
	}

	ret = gnutls_x509_crt_init(&crt);
	if (ret < 0) {
		fprintf(stderr, "crt_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	dat.data = (void *) read_binary_file(info->ca, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "error reading --load-ca-certificate: %s\n",
			info->ca);
		exit(1);
	}

	ret = gnutls_x509_crt_import(crt, &dat, info->incert_format);
	free(dat.data);
	if (ret < 0) {
		fprintf(stderr, "importing --load-ca-certificate: %s: %s\n",
			info->ca, gnutls_strerror(ret));
		exit(1);
	}

	return crt;
}

/* Load a public key.
 * @mand should be non zero if it is required to read a public key.
 */
gnutls_pubkey_t load_pubkey(int mand, common_info_st * info)
{
	gnutls_pubkey_t key;
	int ret;
	gnutls_datum_t dat;
	size_t size;

	if (!info->pubkey && !mand)
		return NULL;

	if (info->pubkey == NULL) {
		fprintf(stderr, "missing --load-pubkey\n");
		exit(1);
	}

	if (gnutls_url_is_supported(info->pubkey) != 0)
		return _load_url_pubkey(info->pubkey);

	ret = gnutls_pubkey_init(&key);
	if (ret < 0) {
		fprintf(stderr, "privkey_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	dat.data = (void *) read_binary_file(info->pubkey, &size);
	dat.size = size;

	if (!dat.data) {
		fprintf(stderr, "reading --load-pubkey: %s\n", info->pubkey);
		exit(1);
	}

	ret = gnutls_pubkey_import(key, &dat, info->incert_format);
	if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR) {
		ret = gnutls_pubkey_import_x509_raw(key, &dat, info->incert_format, 0);
		if (ret < 0) {
			fprintf(stderr,
				"import error: could not find a valid PEM header; "
				"check if your key has the PUBLIC KEY header\n");
			exit(1);
		}
	} else if (ret < 0) {
		fprintf(stderr, "importing --load-pubkey: %s: %s\n",
			info->pubkey, gnutls_strerror(ret));
		exit(1);
	}

	free(dat.data);
	return key;
}

gnutls_pubkey_t load_public_key_or_import(int mand,
					  gnutls_privkey_t privkey,
					  common_info_st * info)
{
	gnutls_pubkey_t pubkey;
	int ret;

	ret = gnutls_pubkey_init(&pubkey);
	if (ret < 0) {
		fprintf(stderr, "gnutls_pubkey_init: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (!privkey || (ret = gnutls_pubkey_import_privkey(pubkey, privkey, 0, 0)) < 0) {	/* could not get (e.g. on PKCS #11 */
		gnutls_pubkey_deinit(pubkey);
		return load_pubkey(mand, info);
	}

	return pubkey;
}

static const char *bits_to_sp(gnutls_pk_algorithm_t pk, unsigned int bits)
{
	gnutls_sec_param_t s = gnutls_pk_bits_to_sec_param(pk, bits);
	if (s == GNUTLS_SEC_PARAM_UNKNOWN) {
		return gnutls_sec_param_get_name(GNUTLS_SEC_PARAM_MEDIUM);
	}

	return gnutls_sec_param_get_name(s);
}

int
get_bits(gnutls_pk_algorithm_t key_type, int info_bits,
	 const char *info_sec_param, int warn)
{
	int bits;

	if (info_bits != 0) {
		static int warned = 0;

		if (warned == 0 && warn != 0 && GNUTLS_BITS_ARE_CURVE(info_bits)==0) {
			warned = 1;
			fprintf(stderr,
				"** Note: You may use '--sec-param %s' instead of '--bits %d'\n",
				bits_to_sp(key_type, info_bits), info_bits);
		}
		bits = info_bits;
	} else {
		if (info_sec_param == 0) {
			/* For ECDSA keys use 256 bits or better, as they are widely supported */
			info_sec_param = "HIGH";
		}
		bits =
		    gnutls_sec_param_to_pk_bits(key_type,
						str_to_sec_param
						(info_sec_param));
	}

	return bits;
}

gnutls_sec_param_t str_to_sec_param(const char *str)
{
	if (strcasecmp(str, "low") == 0) {
		return GNUTLS_SEC_PARAM_LOW;
	} else if (strcasecmp(str, "legacy") == 0) {
		return GNUTLS_SEC_PARAM_LEGACY;
	} else if (strcasecmp(str, "normal") == 0 || strcasecmp(str, "medium") == 0) {
		return GNUTLS_SEC_PARAM_MEDIUM;
	} else if (strcasecmp(str, "high") == 0) {
		return GNUTLS_SEC_PARAM_HIGH;
	} else if (strcasecmp(str, "ultra") == 0) {
		return GNUTLS_SEC_PARAM_ULTRA;
	} else if (strcasecmp(str, "future") == 0) {
		return GNUTLS_SEC_PARAM_FUTURE;
	} else {
		fprintf(stderr, "Unknown security parameter string: %s\n",
			str);
		exit(1);
	}

}

#define SPACE "\t"
static void
print_hex_datum(FILE * outfile, gnutls_datum_t * dat, int cprint)
{
	unsigned int j;

	if (cprint != 0) {
		fprintf(outfile, "\n" SPACE "\"");
		for (j = 0; j < dat->size; j++) {
			fprintf(outfile, "\\x%.2x",
				(unsigned char) dat->data[j]);
			if ((j + 1) % 16 == 0) {
				fprintf(outfile, "\"\n" SPACE "\"");
			}
		}
		fprintf(outfile, "\";\n\n");

		return;
	}

	fprintf(outfile, "\n" SPACE);
	for (j = 0; j < dat->size; j++) {
		if ((j + 1) % 16 == 0) {
			fprintf(outfile, "%.2x", (unsigned char) dat->data[j]);
			fprintf(outfile, "\n" SPACE);
		} else {
			fprintf(outfile, "%.2x:", (unsigned char) dat->data[j]);
		}
	}
	fprintf(outfile, "\n\n");
}

static void print_head(FILE * out, const char *txt, unsigned int size,
		       int cprint)
{
	unsigned i;
	char *p, *ntxt;

	if (cprint != 0) {
		if (size > 0)
			asprintf(&ntxt, "const unsigned char %s[%u] =",
				 txt, size);
		else
			asprintf(&ntxt, "const unsigned char %s[] =\n",
				 txt);

		p = strstr(ntxt, "char");
		p += 5;

		for (i = 0; i < strlen(txt); i++)
			if (p[i] == ' ')
				p[i] = '_';

		fprintf(out, "%s", ntxt);
		free(ntxt);

		return;
	}
	fprintf(out, "%s:", txt);
}

void
print_dsa_pkey(FILE * outfile, gnutls_datum_t * x, gnutls_datum_t * y,
	       gnutls_datum_t * p, gnutls_datum_t * q, gnutls_datum_t * g,
	       int cprint)
{
	if (x) {
		print_head(outfile, "private key", x->size, cprint);
		print_hex_datum(outfile, x, cprint);
	}
	print_head(outfile, "public key", y->size, cprint);
	print_hex_datum(outfile, y, cprint);
	print_head(outfile, "p", p->size, cprint);
	print_hex_datum(outfile, p, cprint);
	print_head(outfile, "q", q->size, cprint);
	print_hex_datum(outfile, q, cprint);
	print_head(outfile, "g", g->size, cprint);
	print_hex_datum(outfile, g, cprint);
}

gnutls_ecc_curve_t str_to_curve(const char *str)
{
unsigned num = 0;
const gnutls_ecc_curve_t *list, *p;

	list = gnutls_ecc_curve_list();

	p = list;
	while(*p != 0) {
		if (strcasecmp(str, gnutls_ecc_curve_get_name(*p)) == 0)
			return *p;
		p++;
		num++;
	}

	fprintf(stderr, "Unsupported curve: %s\nAvailable curves:\n", str);
	if (num == 0)
		printf("none\n");
	p = list;
	while(*p != 0) {
		fprintf(stderr, "\t- %s\n",
		       gnutls_ecc_curve_get_name(*p));
		p++;
	}
	exit(1);
}

void
print_ecc_pkey(FILE * outfile, gnutls_ecc_curve_t curve,
	       gnutls_datum_t * k, gnutls_datum_t * x, gnutls_datum_t * y,
	       int cprint)
{
	if (cprint != 0)
		fprintf(outfile, "/* curve: %s */\n",
			gnutls_ecc_curve_get_name(curve));
	else
		fprintf(outfile, "curve:\t%s\n",
			gnutls_ecc_curve_get_name(curve));

	if (k) {
		print_head(outfile, "private key", k->size, cprint);
		print_hex_datum(outfile, k, cprint);
	}
	print_head(outfile, "x", x->size, cprint);
	print_hex_datum(outfile, x, cprint);
	print_head(outfile, "y", y->size, cprint);
	print_hex_datum(outfile, y, cprint);
}


void
print_rsa_pkey(FILE * outfile, gnutls_datum_t * m, gnutls_datum_t * e,
	       gnutls_datum_t * d, gnutls_datum_t * p, gnutls_datum_t * q,
	       gnutls_datum_t * u, gnutls_datum_t * exp1,
	       gnutls_datum_t * exp2, int cprint)
{
	print_head(outfile, "modulus", m->size, cprint);
	print_hex_datum(outfile, m, cprint);
	print_head(outfile, "public exponent", e->size, cprint);
	print_hex_datum(outfile, e, cprint);
	if (d) {
		print_head(outfile, "private exponent", d->size, cprint);
		print_hex_datum(outfile, d, cprint);
		print_head(outfile, "prime1", p->size, cprint);
		print_hex_datum(outfile, p, cprint);
		print_head(outfile, "prime2", q->size, cprint);
		print_hex_datum(outfile, q, cprint);
		print_head(outfile, "coefficient", u->size, cprint);
		print_hex_datum(outfile, u, cprint);
		if (exp1 && exp2) {
			print_head(outfile, "exp1", exp1->size, cprint);
			print_hex_datum(outfile, exp1, cprint);
			print_head(outfile, "exp2", exp2->size, cprint);
			print_hex_datum(outfile, exp2, cprint);
		}
	}
}

void _pubkey_info(FILE * outfile,
		  gnutls_certificate_print_formats_t format,
		  gnutls_pubkey_t pubkey)
{
	gnutls_datum_t data;
	int ret;
	size_t size;

	fix_lbuffer(0);

	ret = gnutls_pubkey_print(pubkey, format, &data);
	if (ret < 0) {
		fprintf(stderr, "pubkey_print error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "%s\n", data.data);
	gnutls_free(data.data);

	size = lbuffer_size;
	ret =
	    gnutls_pubkey_export(pubkey, GNUTLS_X509_FMT_PEM, lbuffer,
				 &size);
	if (ret < 0) {
		fprintf(stderr, "export error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "\n%s\n", lbuffer);
}

static void
print_dh_info(FILE * outfile, gnutls_datum_t * p, gnutls_datum_t * g,
	      unsigned int q_bits, int cprint)
{
	if (q_bits > 0) {
		if (cprint != 0)
			fprintf(outfile,
				"\n /* recommended key length: %d bytes */\n\n",
				(7 + q_bits) / 8);
		else
			fprintf(outfile,
				"\nRecommended key length: %d bits\n\n",
				q_bits);
	}

	print_head(outfile, "generator", g->size, cprint);
	print_hex_datum(outfile, g, cprint);

	print_head(outfile, "prime", p->size, cprint);
	print_hex_datum(outfile, p, cprint);


}

static
int import_dsa_dh(gnutls_dh_params_t dh_params, gnutls_datum_t *params, gnutls_x509_crt_fmt_t format)
{
	gnutls_x509_privkey_t pkey;
	int ret;

	ret = gnutls_x509_privkey_init(&pkey);
	if (ret < 0)
		return ret;

	ret = gnutls_x509_privkey_import(pkey, params, format);
	if (ret < 0)
		return ret;

	ret = gnutls_dh_params_import_dsa(dh_params, pkey);

	gnutls_x509_privkey_deinit(pkey);

	return ret;
}

void dh_info(FILE * infile, FILE * outfile, common_info_st * ci)
{
	gnutls_datum_t params;
	size_t size;
	int ret, ret2;
	gnutls_dh_params_t dh_params;
	gnutls_datum_t p, g;
	unsigned int q_bits = 0;

	fix_lbuffer(0);

	if (gnutls_dh_params_init(&dh_params) < 0) {
		fprintf(stderr, "Error in dh parameter initialization\n");
		exit(1);
	}

	params.data = (void *) fread_file(infile, &size);
	params.size = size;

	ret =
	    gnutls_dh_params_import_pkcs3(dh_params, &params,
					  ci->incert_format);
	if (ret < 0) {
		/* Try DSA */
		ret2 = import_dsa_dh(dh_params, &params, ci->incert_format);
		if (ret2 < 0) {
			fprintf(stderr, "Error parsing dh params: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	}

	ret = gnutls_dh_params_export_raw(dh_params, &p, &g, &q_bits);
	if (ret < 0) {
		fprintf(stderr, "Error exporting parameters: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (ci->outcert_format == GNUTLS_X509_FMT_PEM)
		print_dh_info(outfile, &p, &g, q_bits, ci->cprint);

	if (!ci->cprint) {	/* generate a PKCS#3 structure */
		size_t len = lbuffer_size;

		ret =
		    gnutls_dh_params_export_pkcs3(dh_params,
						  ci->outcert_format,
						  lbuffer, &len);

		if (ret == 0) {
			if (ci->outcert_format == GNUTLS_X509_FMT_PEM) {
				fprintf(outfile, "\n%s", lbuffer);
			} else {
				fwrite(lbuffer, 1, len, outfile);
			}
		} else {
			fprintf(stderr, "Error: %s\n",
				gnutls_strerror(ret));
		}
	}

	gnutls_dh_params_deinit(dh_params);
}

int cipher_to_flags(const char *cipher)
{
	if (cipher == NULL) {
#ifdef ENABLE_FIPS140
		return GNUTLS_PKCS_USE_PBES2_AES_128;
#else /* compatibility mode - most implementations don't support PBES2 with AES */
		return GNUTLS_PKCS_USE_PKCS12_3DES;
#endif
	} else if (strcasecmp(cipher, "3des") == 0) {
		return GNUTLS_PKCS_USE_PBES2_3DES;
	} else if (strcasecmp(cipher, "3des-pkcs12") == 0) {
		return GNUTLS_PKCS_USE_PKCS12_3DES;
	} else if (strcasecmp(cipher, "arcfour") == 0) {
		return GNUTLS_PKCS_USE_PKCS12_ARCFOUR;
	} else if (strcasecmp(cipher, "aes-128") == 0) {
		return GNUTLS_PKCS_USE_PBES2_AES_128;
	} else if (strcasecmp(cipher, "aes-192") == 0) {
		return GNUTLS_PKCS_USE_PBES2_AES_192;
	} else if (strcasecmp(cipher, "aes-256") == 0) {
		return GNUTLS_PKCS_USE_PBES2_AES_256;
	} else if (strcasecmp(cipher, "rc2-40") == 0) {
		return GNUTLS_PKCS_USE_PKCS12_RC2_40;
	}

	fprintf(stderr, "unknown cipher %s\n", cipher);
	exit(1);
}

static void privkey_info_int(FILE *outfile, common_info_st * cinfo,
			     gnutls_x509_privkey_t key)
{
	int ret, key_type;
	unsigned int bits = 0;
	size_t size;
	const char *cprint;

	/* Public key algorithm
	 */
	fprintf(outfile, "Public Key Info:\n");
	ret = gnutls_x509_privkey_get_pk_algorithm2(key, &bits);
	fprintf(outfile, "\tPublic Key Algorithm: ");

	key_type = ret;

	cprint = gnutls_pk_algorithm_get_name(key_type);
	fprintf(outfile, "%s\n", cprint ? cprint : "Unknown");
	fprintf(outfile, "\tKey Security Level: %s (%u bits)\n\n",
		gnutls_sec_param_get_name(gnutls_x509_privkey_sec_param
					  (key)), bits);

	/* Print the raw public and private keys
	 */
	if (key_type == GNUTLS_PK_RSA) {
		gnutls_datum_t m, e, d, p, q, u, exp1, exp2;

		ret =
		    gnutls_x509_privkey_export_rsa_raw2(key, &m, &e, &d,
							&p, &q, &u, &exp1,
							&exp2);
		if (ret < 0)
			fprintf(stderr,
				"Error in key RSA data export: %s\n",
				gnutls_strerror(ret));
		else {
			print_rsa_pkey(outfile, &m, &e, &d, &p, &q, &u,
				       &exp1, &exp2, cinfo->cprint);

			gnutls_free(m.data);
			gnutls_free(e.data);
			gnutls_free(d.data);
			gnutls_free(p.data);
			gnutls_free(q.data);
			gnutls_free(u.data);
			gnutls_free(exp1.data);
			gnutls_free(exp2.data);
		}
	} else if (key_type == GNUTLS_PK_DSA) {
		gnutls_datum_t p, q, g, y, x;

		ret =
		    gnutls_x509_privkey_export_dsa_raw(key, &p, &q, &g, &y,
						       &x);
		if (ret < 0)
			fprintf(stderr,
				"Error in key DSA data export: %s\n",
				gnutls_strerror(ret));
		else {
			print_dsa_pkey(outfile, &x, &y, &p, &q, &g,
				       cinfo->cprint);

			gnutls_free(x.data);
			gnutls_free(y.data);
			gnutls_free(p.data);
			gnutls_free(q.data);
			gnutls_free(g.data);
		}
	} else if (key_type == GNUTLS_PK_EC) {
		gnutls_datum_t y, x, k;
		gnutls_ecc_curve_t curve;

		ret =
		    gnutls_x509_privkey_export_ecc_raw(key, &curve, &x, &y,
						       &k);
		if (ret < 0)
			fprintf(stderr,
				"Error in key ECC data export: %s\n",
				gnutls_strerror(ret));
		else {
			cprint = gnutls_ecc_curve_get_name(curve);
			bits = 0;

			print_ecc_pkey(outfile, curve, &k, &x, &y,
				       cinfo->cprint);

			gnutls_free(x.data);
			gnutls_free(y.data);
			gnutls_free(k.data);
		}
	}

	fprintf(outfile, "\n");

	size = lbuffer_size;
	ret = gnutls_x509_privkey_get_seed(key, NULL, lbuffer, &size);
	if (ret >= 0) {
		fprintf(outfile, "Seed: %s\n",
			raw_to_string(lbuffer, size));
	}

	size = lbuffer_size;
	if ((ret =
	     gnutls_x509_privkey_get_key_id(key, GNUTLS_KEYID_USE_SHA1, lbuffer, &size)) < 0) {
		fprintf(stderr, "Error in key id calculation: %s\n",
			gnutls_strerror(ret));
	} else {
		gnutls_datum_t art;

		fprintf(outfile, "Public Key ID: %s\n",
			raw_to_string(lbuffer, size));

		ret =
		    gnutls_random_art(GNUTLS_RANDOM_ART_OPENSSH, cprint,
				      bits, lbuffer, size, &art);
		if (ret >= 0) {
			fprintf(outfile, "Public key's random art:\n%s\n",
				art.data);
			gnutls_free(art.data);
		}

	}
	fprintf(outfile, "\n");

}

void
print_private_key(FILE *outfile, common_info_st * cinfo, gnutls_x509_privkey_t key)
{
	int ret;
	size_t size;

	if (!key)
		return;

	if (!cinfo->pkcs8) {
		/* Only print private key parameters when an unencrypted
		 * format is used */
		if (cinfo->outcert_format == GNUTLS_X509_FMT_PEM)
			privkey_info_int(outfile, cinfo, key);

		size = lbuffer_size;
		ret = gnutls_x509_privkey_export(key, cinfo->outcert_format,
						 lbuffer, &size);
		if (ret < 0) {
			fprintf(stderr, "privkey_export: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		if (cinfo->no_compat == 0 && gnutls_x509_privkey_get_seed(key, NULL, NULL, 0) != GNUTLS_E_INVALID_REQUEST) {
			gnutls_x509_privkey_set_flags(key, GNUTLS_PRIVKEY_FLAG_EXPORT_COMPAT);

			fwrite(lbuffer, 1, size, outfile);

			size = lbuffer_size;
			ret = gnutls_x509_privkey_export(key, cinfo->outcert_format,
						 lbuffer, &size);
			if (ret < 0) {
				fprintf(stderr, "privkey_export: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}

	} else {
		unsigned int flags = 0;
		const char *pass;

		pass = get_password(cinfo, &flags, 0);
		flags |= cipher_to_flags(cinfo->pkcs_cipher);

		size = lbuffer_size;
		ret =
		    gnutls_x509_privkey_export_pkcs8(key, cinfo->outcert_format,
						     pass, flags, lbuffer,
						     &size);
		if (ret < 0) {
			fprintf(stderr, "privkey_export_pkcs8: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	}

	fwrite(lbuffer, 1, size, outfile);
}

/* If how is zero then the included parameters are used.
 */
int generate_prime(FILE * outfile, int how, common_info_st * info)
{
	int ret;
	gnutls_dh_params_t dh_params;
	gnutls_datum_t p, g;
	int bits = get_bits(GNUTLS_PK_DH, info->bits, info->sec_param, 1);
	unsigned int q_bits = 0;

	fix_lbuffer(0);

	gnutls_dh_params_init(&dh_params);

	if (how != 0) {
		fprintf(stderr, "Generating DH parameters (%d bits)...\n",
			bits);
		fprintf(stderr, "(might take long time)\n");
	} else
		fprintf(stderr, "Retrieving DH parameters...\n");

	if (how != 0) {
		if (info->provable != 0) {
			gnutls_x509_privkey_t pkey;
			unsigned save;

			ret = gnutls_x509_privkey_init(&pkey);
			if (ret < 0) {
				fprintf(stderr,
					"Error initializing key: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}

			ret = gnutls_x509_privkey_generate(pkey, GNUTLS_PK_DSA, bits, GNUTLS_PRIVKEY_FLAG_PROVABLE);
			if (ret < 0) {
				fprintf(stderr,
					"Error generating DSA parameters: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}

			if (info->outcert_format == GNUTLS_X509_FMT_PEM) {
				save = info->no_compat;
				info->no_compat = 1;
				print_private_key(outfile, info, pkey);
				info->no_compat = save;
			}

			ret = gnutls_dh_params_import_dsa(dh_params, pkey);
			if (ret < 0) {
				fprintf(stderr,
					"Error importing DSA parameters: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}

			gnutls_x509_privkey_deinit(pkey);
		} else {
			ret = gnutls_dh_params_generate2(dh_params, bits);
			if (ret < 0) {
				fprintf(stderr,
					"Error generating parameters: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}

		ret =
		    gnutls_dh_params_export_raw(dh_params, &p, &g,
						&q_bits);
		if (ret < 0) {
			fprintf(stderr, "Error exporting parameters: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	} else {
#ifdef ENABLE_SRP
		if (info->provable != 0) {
			fprintf(stderr, "The DH parameters obtained via this option are not provable\n");
			exit(1);
		}

		if (bits <= 1024) {
			p = gnutls_srp_1024_group_prime;
			g = gnutls_srp_1024_group_generator;
			bits = 1024;
		} else if (bits <= 1536) {
			p = gnutls_srp_1536_group_prime;
			g = gnutls_srp_1536_group_generator;
			bits = 1536;
		} else if (bits <= 2048) {
			p = gnutls_srp_2048_group_prime;
			g = gnutls_srp_2048_group_generator;
			bits = 2048;
		} else if (bits <= 3072) {
			p = gnutls_srp_3072_group_prime;
			g = gnutls_srp_3072_group_generator;
			bits = 3072;
		} else {
			p = gnutls_srp_4096_group_prime;
			g = gnutls_srp_4096_group_generator;
			bits = 4096;
		}

		ret = gnutls_dh_params_import_raw(dh_params, &p, &g);
		if (ret < 0) {
			fprintf(stderr, "Error exporting parameters: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
#else
		fprintf(stderr,
			"Parameters unavailable as SRP is disabled.\n");
		exit(1);
#endif
	}

	if (info->outcert_format == GNUTLS_X509_FMT_PEM)
		print_dh_info(outfile, &p, &g, q_bits, info->cprint);

	if (!info->cprint) {	/* generate a PKCS#3 structure */
		size_t len = lbuffer_size;

		ret =
		    gnutls_dh_params_export_pkcs3(dh_params,
						  info->outcert_format,
						  lbuffer, &len);

		if (ret == 0) {
			if (info->outcert_format == GNUTLS_X509_FMT_PEM)
				fprintf(outfile, "\n%s", lbuffer);
			else
				fwrite(lbuffer, 1, len, outfile);

		} else {
			fprintf(stderr, "Error: %s\n",
				gnutls_strerror(ret));
		}

	}

	gnutls_dh_params_deinit(dh_params);

	return 0;
}
