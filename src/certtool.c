/*
 * Copyright (C) 2003-2016 Free Software Foundation, Inc.
 * Copyright (C) 2015-2016 Red Hat, Inc.
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
#include <gnutls/crypto.h>

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
#ifndef _WIN32
# include <signal.h>
#endif

/* Gnulib portability files. */
#include <read-file.h>

#include <certtool-cfg.h>
#include <common.h>
#include "certtool-args.h"
#include "certtool-common.h"

static FILE *stdlog = NULL;

static void print_crl_info(gnutls_x509_crl_t crl, FILE * out);
void pkcs7_info(common_info_st *);
void pkcs7_sign(common_info_st *, unsigned embed);
void pkcs7_generate(common_info_st *);
void pkcs8_info(void);
void pkcs8_info_int(gnutls_datum_t *data, unsigned format, 
			unsigned ignore_err, FILE *out, const char *tab);
void crq_info(void);
void smime_to_pkcs7(void);
void pkcs12_info(common_info_st *);
void generate_pkcs12(common_info_st *);
void generate_pkcs8(common_info_st *);
static void verify_chain(void);
void verify_crl(common_info_st * cinfo);
void verify_pkcs7(common_info_st * cinfo, const char *purpose, unsigned display_data);
void pubkey_info(gnutls_x509_crt_t crt, common_info_st *);
void pgp_privkey_info(void);
void pgp_ring_info(void);
void certificate_info(int, common_info_st *);
void pgp_certificate_info(void);
void crl_info(void);
void privkey_info(common_info_st *);
static void cmd_parser(int argc, char **argv);
void generate_self_signed(common_info_st *);
void generate_request(common_info_st *);
static void print_certificate_info(gnutls_x509_crt_t crt, FILE * out,
				   unsigned int all);
static void verify_certificate(common_info_st * cinfo);

static void pubkey_keyid(common_info_st * cinfo);
static void certificate_fpr(common_info_st * cinfo);

FILE *outfile;
FILE *infile;
static gnutls_digest_algorithm_t default_dig;
static unsigned int incert_format, outcert_format;
static unsigned int req_key_type;
gnutls_certificate_print_formats_t full_format = GNUTLS_CRT_PRINT_FULL;

/* non interactive operation if set
 */
int batch;
int ask_pass;


static void tls_log_func(int level, const char *str)
{
	fprintf(stderr, "|<%d>| %s", level, str);
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
	cfg_init();
	cmd_parser(argc, argv);

	return 0;
}

static gnutls_x509_privkey_t
generate_private_key_int(common_info_st * cinfo)
{
	gnutls_x509_privkey_t key;
	int ret, key_type, bits;
	unsigned provable = cinfo->provable;
	unsigned flags = 0;

	key_type = req_key_type;

	ret = gnutls_x509_privkey_init(&key);
	if (ret < 0) {
		fprintf(stderr, "privkey_init: %s", gnutls_strerror(ret));
		exit(1);
	}

	bits = get_bits(key_type, cinfo->bits, cinfo->sec_param, 1);

	fprintf(stdlog, "Generating a %d bit %s private key...\n",
		bits, gnutls_pk_algorithm_get_name(key_type));

	if (bits < 256 && key_type == GNUTLS_PK_EC)
		fprintf(stderr,
			"Note that ECDSA keys with size less than 256 are not widely supported.\n\n");

	if (provable && (key_type != GNUTLS_PK_RSA && key_type != GNUTLS_PK_DSA)) {
		fprintf(stderr,
			"The --provable parameter cannot be used with ECDSA keys.\n");
		exit(1);
	}

	if (bits > 1024 && key_type == GNUTLS_PK_DSA)
		fprintf(stderr,
			"Note that DSA keys with size over 1024 may cause incompatibility problems when used with earlier than TLS 1.2 versions.\n\n");

	if ((HAVE_OPT(SEED) || provable) && key_type == GNUTLS_PK_RSA) {
		if (bits != 2048 && bits != 3072) {
			fprintf(stderr, "Note that the FIPS 186-4 key generation restricts keys to 2048 and 3072 bits\n");
		}
	}

	if (HAVE_OPT(SEED)) {
		gnutls_keygen_data_st data;
		unsigned char seed[256];
		size_t seed_size = sizeof(seed);
		ret = gnutls_hex2bin(OPT_ARG(SEED), strlen(OPT_ARG(SEED)), seed, &seed_size);
		if (ret < 0) {
			fprintf(stderr, "Could not hex decode data: %s\n", gnutls_strerror(ret));
			exit(1);
		}

		data.type = GNUTLS_KEYGEN_SEED;
		data.data = seed;
		data.size = seed_size;

		if (key_type == GNUTLS_PK_RSA) {
			if ((bits == 3072 && seed_size != 32) || (bits == 2048 && seed_size != 28)) {
				fprintf(stderr, "The seed size (%d) doesn't match the size of the request security level; use -d 2 for more information.\n", (int)seed_size);
			}
		} else if (key_type == GNUTLS_PK_DSA) {
			if (seed_size != 65) {
				fprintf(stderr, "The seed size (%d) doesn't match the size of the request security level; use -d 2 for more information.\n", (int)seed_size);
			}
		}

		ret = gnutls_x509_privkey_generate2(key, key_type, bits, GNUTLS_PRIVKEY_FLAG_PROVABLE, &data, 1);
	} else {
		if (provable)
			flags |= GNUTLS_PRIVKEY_FLAG_PROVABLE;
		ret = gnutls_x509_privkey_generate(key, key_type, bits, flags);
	}
	if (ret < 0) {
		fprintf(stderr, "privkey_generate: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	ret = gnutls_x509_privkey_verify_params(key);
	if (ret < 0) {
		fprintf(stderr, "privkey_verify_params: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	return key;
}


static void generate_private_key(common_info_st * cinfo)
{
	gnutls_x509_privkey_t key;

	key = generate_private_key_int(cinfo);

	print_private_key(outfile, cinfo, key);

	gnutls_x509_privkey_deinit(key);
}

static void verify_provable_privkey(common_info_st * cinfo)
{
	gnutls_privkey_t pkey;
	int ret;

	pkey = load_private_key(1, cinfo);

	if (HAVE_OPT(SEED)) {
		char seed[256];
		size_t seed_size = sizeof(seed);
		ret = gnutls_hex2bin(OPT_ARG(SEED), strlen(OPT_ARG(SEED)), seed, &seed_size);
		if (ret < 0) {
			fprintf(stderr, "Could not hex decode data: %s\n", gnutls_strerror(ret));
			exit(1);
		}
		ret = gnutls_privkey_verify_seed(pkey, 0, seed, seed_size);
	} else {
		ret = gnutls_privkey_verify_seed(pkey, 0, NULL, 0);
	}

	if (ret < 0) {
		fprintf(stderr, "Error verifying private key: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	printf("Key was verified\n");
	gnutls_privkey_deinit(pkey);

	return;
}


static gnutls_x509_crt_t
generate_certificate(gnutls_privkey_t * ret_key,
		     gnutls_x509_crt_t ca_crt, int proxy,
		     common_info_st * cinfo)
{
	gnutls_x509_crt_t crt;
	gnutls_privkey_t key = NULL;
	gnutls_pubkey_t pubkey;
	size_t size;
	int ret;
	int client;
	int result, ca_status = 0, is_ike = 0, path_len;
	time_t secs;
	int vers;
	unsigned int usage = 0, server, ask;
	gnutls_x509_crq_t crq;	/* request */

	ret = gnutls_x509_crt_init(&crt);
	if (ret < 0) {
		fprintf(stderr, "crt_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	crq = load_request(cinfo);

	if (crq == NULL) {

		key = load_private_key(0, cinfo);

		pubkey = load_public_key_or_import(1, key, cinfo);

		if (!batch)
			fprintf(stderr,
				"Please enter the details of the certificate's distinguished name. "
				"Just press enter to ignore a field.\n");

		/* set the DN.
		 */
		if (proxy) {
			result =
			    gnutls_x509_crt_set_proxy_dn(crt, ca_crt, 0,
							 NULL, 0);
			if (result < 0) {
				fprintf(stderr, "set_proxy_dn: %s\n",
					gnutls_strerror(result));
				exit(1);
			}

			get_dn_crt_set(crt);
			get_cn_crt_set(crt);
		} else {
			get_dn_crt_set(crt);

			get_cn_crt_set(crt);
			get_uid_crt_set(crt);
			get_unit_crt_set(crt);
			get_organization_crt_set(crt);
			get_locality_crt_set(crt);
			get_state_crt_set(crt);
			get_country_crt_set(crt);
			get_dc_set(TYPE_CRT, crt);

			get_oid_crt_set(crt);
			get_key_purpose_set(TYPE_CRT, crt);

			if (!batch)
				fprintf(stderr,
					"This field should not be used in new certificates.\n");

			get_pkcs9_email_crt_set(crt);
		}

		result = gnutls_x509_crt_set_pubkey(crt, pubkey);
		if (result < 0) {
			fprintf(stderr, "set_key: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
		gnutls_pubkey_deinit(pubkey);
	} else {
		result = gnutls_x509_crt_set_crq(crt, crq);
		if (result < 0) {
			fprintf(stderr, "set_crq: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}


	{
		size_t serial_size;
		unsigned char serial[16];

		serial_size = sizeof(serial);

		get_serial(serial, &serial_size);

		result = gnutls_x509_crt_set_serial(crt, serial, serial_size);
		if (result < 0) {
			fprintf(stderr, "serial: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}

	if (!batch)
		fprintf(stderr, "\n\nActivation/Expiration time.\n");

	secs = get_activation_date();

	result = gnutls_x509_crt_set_activation_time(crt, secs);
	if (result < 0) {
		fprintf(stderr, "set_activation: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	do {
		ask = 0;
		secs = get_expiration_date();

		if (ca_crt && (secs > gnutls_x509_crt_get_expiration_time(ca_crt))) {
			time_t exp = gnutls_x509_crt_get_expiration_time(ca_crt);
			fprintf(stderr, "\nExpiration time: %s", ctime(&secs));
			fprintf(stderr, "CA expiration time: %s", ctime(&exp));
			fprintf(stderr, "Warning: The time set exceeds the CA's expiration time\n");
			ask = 1;
		}
	} while(batch == 0 && ask != 0 && read_yesno("Is it ok to proceed? (y/N): ", 0) == 0);


	result = gnutls_x509_crt_set_expiration_time(crt, secs);
	if (result < 0) {
		fprintf(stderr, "set_expiration: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	if (!batch)
		fprintf(stderr, "\n\nExtensions.\n");

	/* do not allow extensions on a v1 certificate */
	if (crq && get_crq_extensions_status() != 0) {
		result = gnutls_x509_crt_set_crq_extensions(crt, crq);
		if (result < 0) {
			fprintf(stderr, "set_crq: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}

	/* append additional extensions */
	if (cinfo->v1_cert == 0) {

		if (proxy) {
			const char *policylanguage;
			char *policy;
			size_t policylen;
			int proxypathlen = get_path_len();

			if (!batch) {
				printf
				    ("1.3.6.1.5.5.7.21.1 ::= id-ppl-inheritALL\n");
				printf
				    ("1.3.6.1.5.5.7.21.2 ::= id-ppl-independent\n");
			}

			policylanguage =
			    get_proxy_policy(&policy, &policylen);

			result =
			    gnutls_x509_crt_set_proxy(crt, proxypathlen,
						      policylanguage,
						      policy, policylen);
			if (result < 0) {
				fprintf(stderr, "set_proxy: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		if (!proxy)
			ca_status = get_ca_status();
		if (ca_status)
			path_len = get_path_len();
		else
			path_len = -1;

		result =
		    gnutls_x509_crt_set_basic_constraints(crt, ca_status,
							  path_len);
		if (result < 0) {
			fprintf(stderr, "basic_constraints: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		client = get_tls_client_status();
		if (client != 0) {
			result = gnutls_x509_crt_set_key_purpose_oid(crt,
								     GNUTLS_KP_TLS_WWW_CLIENT,
								     0);
			if (result < 0) {
				fprintf(stderr, "key_kp: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		crt_unique_ids_set(crt);

		is_ike = get_ipsec_ike_status();
		server = get_tls_server_status();

		get_dns_name_set(TYPE_CRT, crt);
		get_uri_set(TYPE_CRT, crt);
		get_ip_addr_set(TYPE_CRT, crt);
		get_other_name_set(TYPE_CRT, crt);
		get_policy_set(crt);

		if (server != 0) {
			result =
			    gnutls_x509_crt_set_key_purpose_oid(crt,
								GNUTLS_KP_TLS_WWW_SERVER,
								0);
			if (result < 0) {
				fprintf(stderr, "key_kp: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		} else if (!proxy) {
			get_email_set(TYPE_CRT, crt);
		}

		if (!ca_status || server) {
			int pk;


			pk = gnutls_x509_crt_get_pk_algorithm(crt, NULL);

			if (pk == GNUTLS_PK_RSA) {	/* DSA and ECDSA keys can only sign. */
				result = get_sign_status(server);
				if (result)
					usage |=
					    GNUTLS_KEY_DIGITAL_SIGNATURE;

				result = get_encrypt_status(server);
				if (result)
					usage |=
					    GNUTLS_KEY_KEY_ENCIPHERMENT;
			} else {
				if (get_encrypt_status(server))
					fprintf(stderr, "warning: this algorithm does not support encryption; disabling the encryption flag\n");

				usage |= GNUTLS_KEY_DIGITAL_SIGNATURE;
			}

			if (is_ike) {
				result =
				    gnutls_x509_crt_set_key_purpose_oid
				    (crt, GNUTLS_KP_IPSEC_IKE, 0);
				if (result < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(result));
					exit(1);
				}
			}
		}

		result = get_key_agreement_status();
		if (result)
			usage |= GNUTLS_KEY_KEY_AGREEMENT;

		result = get_data_encipherment_status();
		if (result)
			usage |= GNUTLS_KEY_DATA_ENCIPHERMENT;

		result = get_non_repudiation_status();
		if (result)
			usage |= GNUTLS_KEY_NON_REPUDIATION;

		result = get_ocsp_sign_status();
		if (result) {
			result =
			    gnutls_x509_crt_set_key_purpose_oid
			    (crt, GNUTLS_KP_OCSP_SIGNING, 0);
			if (result < 0) {
				fprintf(stderr, "key_kp: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		if (ca_status) {
			result = get_cert_sign_status();
			if (result)
				usage |= GNUTLS_KEY_KEY_CERT_SIGN;

			result = get_crl_sign_status();
			if (result)
				usage |= GNUTLS_KEY_CRL_SIGN;

			result = get_code_sign_status();
			if (result) {
				result =
				    gnutls_x509_crt_set_key_purpose_oid
				    (crt, GNUTLS_KP_CODE_SIGNING, 0);
				if (result < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(result));
					exit(1);
				}
			}

			crt_constraints_set(crt);


			result = get_time_stamp_status();
			if (result) {
				result =
				    gnutls_x509_crt_set_key_purpose_oid
				    (crt, GNUTLS_KP_TIME_STAMPING, 0);
				if (result < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(result));
					exit(1);
				}
			}
		}
		get_ocsp_issuer_set(crt);
		get_ca_issuers_set(crt);

		if (usage != 0) {
			/* http://tools.ietf.org/html/rfc4945#section-5.1.3.2: if any KU is
			   set, then either digitalSignature or the nonRepudiation bits in the
			   KeyUsage extension MUST for all IKE certs */
			if (is_ike && (get_sign_status(server) != 1))
				usage |= GNUTLS_KEY_NON_REPUDIATION;
			result = gnutls_x509_crt_set_key_usage(crt, usage);
			if (result < 0) {
				fprintf(stderr, "key_usage: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		/* Subject Key ID.
		 */
		size = lbuffer_size;
		result = gnutls_x509_crt_get_key_id(crt, GNUTLS_KEYID_USE_SHA1, lbuffer, &size);
		if (result >= 0) {
			result =
			    gnutls_x509_crt_set_subject_key_id(crt, lbuffer,
							       size);
			if (result < 0) {
				fprintf(stderr, "set_subject_key_id: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		/* Authority Key ID.
		 */
		if (ca_crt != NULL) {
			size = lbuffer_size;
			result =
			    gnutls_x509_crt_get_subject_key_id(ca_crt,
							       lbuffer,
							       &size,
							       NULL);
			if (result >= 0) {
				result =
				    gnutls_x509_crt_set_authority_key_id
				    (crt, lbuffer, size);
				if (result < 0) {
					fprintf(stderr,
						"set_authority_key_id: %s\n",
						gnutls_strerror(result));
					exit(1);
				}
			}
		}
	}

	/* Version.
	 */
	if (cinfo->v1_cert != 0)
		vers = 1;
	else
		vers = 3;
	result = gnutls_x509_crt_set_version(crt, vers);
	if (result < 0) {
		fprintf(stderr, "set_version: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	*ret_key = key;
	return crt;

}

static gnutls_x509_crl_t
generate_crl(gnutls_x509_crt_t ca_crt, common_info_st * cinfo)
{
	gnutls_x509_crl_t crl;
	gnutls_x509_crt_t *crts;
	gnutls_x509_crl_t *crls;
	size_t size, crl_size;
	int result;
	unsigned int i;
	time_t secs, this_update, exp;

	crls = load_crl_list(0, &crl_size, cinfo);
	if (crls != NULL) {
		if (crl_size > 1) {
			fprintf(stderr, "load_crl: too many CRLs present\n");
			exit(1);
		}
		crl = crls[0];
		gnutls_free(crls);
	} else {
		result = gnutls_x509_crl_init(&crl);
		if (result < 0) {
			fprintf(stderr, "crl_init: %s\n", gnutls_strerror(result));
			exit(1);
		}
	}

	crts = load_cert_list(0, &size, cinfo);

	exp = get_crl_revocation_date();

	for (i = 0; i < size; i++) {
		result = gnutls_x509_crl_set_crt(crl, crts[i], exp);
		if (result < 0) {
			fprintf(stderr, "crl_set_crt: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
		gnutls_x509_crt_deinit(crts[i]);
	}
	gnutls_free(crts);

	this_update = get_crl_this_update_date();

	result = gnutls_x509_crl_set_this_update(crl, this_update);
	if (result < 0) {
		fprintf(stderr, "this_update: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	fprintf(stderr, "Update times.\n");
	secs = get_crl_next_update();

	result =
	    gnutls_x509_crl_set_next_update(crl, secs);
	if (result < 0) {
		fprintf(stderr, "next_update: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	result = gnutls_x509_crl_set_version(crl, 2);
	if (result < 0) {
		fprintf(stderr, "set_version: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	/* Authority Key ID.
	 */
	if (ca_crt != NULL) {
		size = lbuffer_size;
		result = gnutls_x509_crt_get_subject_key_id(ca_crt, lbuffer,
							    &size, NULL);
		if (result >= 0) {
			result =
			    gnutls_x509_crl_set_authority_key_id(crl,
								 lbuffer,
								 size);
			if (result < 0) {
				fprintf(stderr, "set_authority_key_id: %s\n",
					gnutls_strerror(result));
				exit(1);
			}

		}
	}

	{
		size_t serial_size;
		unsigned char serial[16];

		serial_size = sizeof(serial);

		get_crl_number(serial, &serial_size);

		result = gnutls_x509_crl_set_number(crl, serial, serial_size);
		if (result < 0) {
			fprintf(stderr, "crl set_number: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}

	return crl;
}

static gnutls_digest_algorithm_t get_dig_for_pub(gnutls_pubkey_t pubkey)
{
	gnutls_digest_algorithm_t dig;
	int result;
	unsigned int mand;

	result =
	    gnutls_pubkey_get_preferred_hash_algorithm(pubkey, &dig,
						       &mand);
	if (result < 0) {
		{
			fprintf(stderr,
				"crt_get_preferred_hash_algorithm: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}

	/* if algorithm allows alternatives */
	if (mand == 0 && default_dig != GNUTLS_DIG_UNKNOWN)
		dig = default_dig;

	return dig;
}

static gnutls_digest_algorithm_t get_dig(gnutls_x509_crt_t crt)
{
	gnutls_digest_algorithm_t dig;
	gnutls_pubkey_t pubkey;
	int result;

	gnutls_pubkey_init(&pubkey);

	result = gnutls_pubkey_import_x509(pubkey, crt, 0);
	if (result < 0) {
		{
			fprintf(stderr, "gnutls_pubkey_import_x509: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
	}

	dig = get_dig_for_pub(pubkey);

	gnutls_pubkey_deinit(pubkey);

	return dig;
}

void generate_self_signed(common_info_st * cinfo)
{
	gnutls_x509_crt_t crt;
	gnutls_privkey_t key;
	size_t size;
	int result;

	fprintf(stdlog, "Generating a self signed certificate...\n");

	crt = generate_certificate(&key, NULL, 0, cinfo);

	if (!key)
		key = load_private_key(1, cinfo);

	get_crl_dist_point_set(crt);

	print_certificate_info(crt, stdlog, 0);

	fprintf(stdlog, "\n\nSigning certificate...\n");

	result =
	    gnutls_x509_crt_privkey_sign(crt, crt, key, get_dig(crt), 0);
	if (result < 0) {
		fprintf(stderr, "crt_sign: %s\n", gnutls_strerror(result));
		exit(1);
	}

	size = lbuffer_size;
	result =
	    gnutls_x509_crt_export(crt, outcert_format, lbuffer, &size);
	if (result < 0) {
		fprintf(stderr, "crt_export: %s\n", gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

	gnutls_x509_crt_deinit(crt);
	gnutls_privkey_deinit(key);
}

static void generate_signed_certificate(common_info_st * cinfo)
{
	gnutls_x509_crt_t crt;
	gnutls_privkey_t key;
	size_t size;
	int result;
	gnutls_privkey_t ca_key;
	gnutls_x509_crt_t ca_crt;

	fprintf(stdlog, "Generating a signed certificate...\n");

	ca_key = load_ca_private_key(cinfo);
	ca_crt = load_ca_cert(1, cinfo);

	crt = generate_certificate(&key, ca_crt, 0, cinfo);

	/* Copy the CRL distribution points.
	 */
	gnutls_x509_crt_cpy_crl_dist_points(crt, ca_crt);
	/* it doesn't matter if we couldn't copy the CRL dist points.
	 */

	print_certificate_info(crt, stdlog, 0);

	fprintf(stdlog, "\n\nSigning certificate...\n");

	result =
	    gnutls_x509_crt_privkey_sign(crt, ca_crt, ca_key,
					 get_dig(ca_crt), 0);
	if (result < 0) {
		fprintf(stderr, "crt_sign: %s\n", gnutls_strerror(result));
		exit(1);
	}

	size = lbuffer_size;
	result =
	    gnutls_x509_crt_export(crt, outcert_format, lbuffer, &size);
	if (result < 0) {
		fprintf(stderr, "crt_export: %s\n", gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

	gnutls_x509_crt_deinit(crt);
	gnutls_x509_crt_deinit(ca_crt);
	gnutls_privkey_deinit(key);
	gnutls_privkey_deinit(ca_key);
}

static void generate_proxy_certificate(common_info_st * cinfo)
{
	gnutls_x509_crt_t crt, eecrt;
	gnutls_privkey_t key, eekey;
	size_t size;
	int result;

	fprintf(stdlog, "Generating a proxy certificate...\n");

	eekey = load_ca_private_key(cinfo);
	eecrt = load_cert(1, cinfo);

	crt = generate_certificate(&key, eecrt, 1, cinfo);

	print_certificate_info(crt, stdlog, 0);

	fprintf(stdlog, "\n\nSigning certificate...\n");

	result =
	    gnutls_x509_crt_privkey_sign(crt, eecrt, eekey, get_dig(eecrt),
					 0);
	if (result < 0) {
		fprintf(stderr, "crt_sign: %s\n", gnutls_strerror(result));
		exit(1);
	}

	size = lbuffer_size;
	result =
	    gnutls_x509_crt_export(crt, outcert_format, lbuffer, &size);
	if (result < 0) {
		fprintf(stderr, "crt_export: %s\n", gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

	gnutls_x509_crt_deinit(eecrt);
	gnutls_x509_crt_deinit(crt);
	gnutls_privkey_deinit(key);
	gnutls_privkey_deinit(eekey);
}

static void generate_signed_crl(common_info_st * cinfo)
{
	gnutls_x509_crl_t crl;
	int result;
	gnutls_privkey_t ca_key;
	gnutls_x509_crt_t ca_crt;

	fprintf(stdlog, "Generating a signed CRL...\n");

	ca_key = load_ca_private_key(cinfo);
	ca_crt = load_ca_cert(1, cinfo);
	crl = generate_crl(ca_crt, cinfo);

	fprintf(stdlog, "\n");
	result =
	    gnutls_x509_crl_privkey_sign(crl, ca_crt, ca_key,
					 get_dig(ca_crt), 0);
	if (result < 0) {
		fprintf(stderr, "crl_privkey_sign: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	print_crl_info(crl, stdlog);

	gnutls_privkey_deinit(ca_key);
	gnutls_x509_crl_deinit(crl);
	gnutls_x509_crt_deinit(ca_crt);
}

static void update_signed_certificate(common_info_st * cinfo)
{
	gnutls_x509_crt_t crt;
	size_t size;
	int result;
	gnutls_privkey_t ca_key;
	gnutls_x509_crt_t ca_crt;
	time_t tim;

	fprintf(stdlog, "Generating a signed certificate...\n");

	ca_key = load_ca_private_key(cinfo);
	ca_crt = load_ca_cert(1, cinfo);
	crt = load_cert(1, cinfo);

	fprintf(stderr, "Activation/Expiration time.\n");
	tim = get_activation_date();

	result = gnutls_x509_crt_set_activation_time(crt, tim);
	if (result < 0) {
		fprintf(stderr, "set_activation: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	tim = get_expiration_date();

	result = gnutls_x509_crt_set_expiration_time(crt, tim);
	if (result < 0) {
		fprintf(stderr, "set_expiration: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	fprintf(stderr, "\n\nSigning certificate...\n");

	result =
	    gnutls_x509_crt_privkey_sign(crt, ca_crt, ca_key,
					 get_dig(ca_crt), 0);
	if (result < 0) {
		fprintf(stderr, "crt_sign: %s\n", gnutls_strerror(result));
		exit(1);
	}

	size = lbuffer_size;
	result =
	    gnutls_x509_crt_export(crt, outcert_format, lbuffer, &size);
	if (result < 0) {
		fprintf(stderr, "crt_export: %s\n", gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

	gnutls_x509_crt_deinit(crt);
}

static void cmd_parser(int argc, char **argv)
{
	int ret, privkey_op = 0;
	common_info_st cinfo;

	optionProcess(&certtoolOptions, argc, argv);

	if (HAVE_OPT(STDOUT_INFO)) {
		/* print informational messages on stdout instead of stderr */
		stdlog = stdout;
	} else {
		stdlog = stderr;
	}

	if (HAVE_OPT(GENERATE_PRIVKEY) || HAVE_OPT(GENERATE_REQUEST) ||
	    HAVE_OPT(KEY_INFO) || HAVE_OPT(PGP_KEY_INFO))
		privkey_op = 1;

	if (HAVE_OPT(HEX_NUMBERS))
		full_format = GNUTLS_CRT_PRINT_FULL_NUMBERS;

	if (HAVE_OPT(OUTFILE)) {
		outfile = safe_open_rw(OPT_ARG(OUTFILE), privkey_op);
		if (outfile == NULL) {
			fprintf(stderr, "%s", OPT_ARG(OUTFILE));
			exit(1);
		}
	} else
		outfile = stdout;


	if (HAVE_OPT(INFILE)) {
		struct stat st;
		if (stat(OPT_ARG(INFILE), &st) == 0) {
			fix_lbuffer(2*st.st_size);
		}

		infile = fopen(OPT_ARG(INFILE), "rb");
		if (infile == NULL) {
			fprintf(stderr, "%s", OPT_ARG(INFILE));
			exit(1);
		}
	} else
		infile = stdin;

	fix_lbuffer(0);

	if (HAVE_OPT(INDER) || HAVE_OPT(INRAW))
		incert_format = GNUTLS_X509_FMT_DER;
	else
		incert_format = GNUTLS_X509_FMT_PEM;

	if (HAVE_OPT(OUTDER) || HAVE_OPT(OUTRAW))
		outcert_format = GNUTLS_X509_FMT_DER;
	else
		outcert_format = GNUTLS_X509_FMT_PEM;

	if (HAVE_OPT(DSA))
		req_key_type = GNUTLS_PK_DSA;
	else if (HAVE_OPT(ECC))
		req_key_type = GNUTLS_PK_ECC;
	else
		req_key_type = GNUTLS_PK_RSA;

	default_dig = GNUTLS_DIG_UNKNOWN;
	if (HAVE_OPT(HASH)) {
		if (strcasecmp(OPT_ARG(HASH), "md5") == 0) {
			fprintf(stderr,
				"Warning: MD5 is broken, and should not be used any more for digital signatures.\n");
			default_dig = GNUTLS_DIG_MD5;
		} else if (strcasecmp(OPT_ARG(HASH), "sha1") == 0)
			default_dig = GNUTLS_DIG_SHA1;
		else if (strcasecmp(OPT_ARG(HASH), "sha256") == 0)
			default_dig = GNUTLS_DIG_SHA256;
		else if (strcasecmp(OPT_ARG(HASH), "sha224") == 0)
			default_dig = GNUTLS_DIG_SHA224;
		else if (strcasecmp(OPT_ARG(HASH), "sha384") == 0)
			default_dig = GNUTLS_DIG_SHA384;
		else if (strcasecmp(OPT_ARG(HASH), "sha512") == 0)
			default_dig = GNUTLS_DIG_SHA512;
		else if (strcasecmp(OPT_ARG(HASH), "sha3-256") == 0)
			default_dig = GNUTLS_DIG_SHA3_256;
		else if (strcasecmp(OPT_ARG(HASH), "sha3-224") == 0)
			default_dig = GNUTLS_DIG_SHA3_224;
		else if (strcasecmp(OPT_ARG(HASH), "sha3-384") == 0)
			default_dig = GNUTLS_DIG_SHA3_384;
		else if (strcasecmp(OPT_ARG(HASH), "sha3-512") == 0)
			default_dig = GNUTLS_DIG_SHA3_512;
		else if (strcasecmp(OPT_ARG(HASH), "rmd160") == 0)
			default_dig = GNUTLS_DIG_RMD160;
		else {
			default_dig = gnutls_digest_get_id(OPT_ARG(HASH));
			if (default_dig == GNUTLS_DIG_UNKNOWN) {
				fprintf(stderr, "invalid hash: %s\n", OPT_ARG(HASH));
				exit(1);
			}
		}
	}

	batch = 0;
	if (HAVE_OPT(TEMPLATE)) {
		batch = 1;
		template_parse(OPT_ARG(TEMPLATE));
	}

	ask_pass = ENABLED_OPT(ASK_PASS);

	gnutls_global_set_log_function(tls_log_func);

	if (HAVE_OPT(DEBUG)) {
		gnutls_global_set_log_level(OPT_VALUE_DEBUG);
		printf("Setting log level to %d\n", (int) OPT_VALUE_DEBUG);
	}

	if ((ret = gnutls_global_init()) < 0) {
		fprintf(stderr, "global_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	memset(&cinfo, 0, sizeof(cinfo));
#ifdef ENABLE_PKCS11
	if (HAVE_OPT(PROVIDER)) {
		ret = gnutls_pkcs11_init(GNUTLS_PKCS11_FLAG_MANUAL, NULL);
		if (ret < 0)
			fprintf(stderr, "pkcs11_init: %s",
				gnutls_strerror(ret));
		else {
			ret =
			    gnutls_pkcs11_add_provider(OPT_ARG(PROVIDER),
						       NULL);
			if (ret < 0) {
				fprintf(stderr, "pkcs11_add_provider: %s",
					gnutls_strerror(ret));
				exit(1);
			}
		}
	}

	pkcs11_common(&cinfo);
#endif

	if (HAVE_OPT(VERBOSE))
		cinfo.verbose = 1;

	cinfo.batch = batch;
	cinfo.cprint = HAVE_OPT(CPRINT);

	if (HAVE_OPT(LOAD_PRIVKEY))
		cinfo.privkey = OPT_ARG(LOAD_PRIVKEY);

	if (HAVE_OPT(LOAD_CRL))
		cinfo.crl = OPT_ARG(LOAD_CRL);

	if (HAVE_OPT(LOAD_DATA))
		cinfo.data_file = OPT_ARG(LOAD_DATA);

	cinfo.v1_cert = HAVE_OPT(V1);
	if (HAVE_OPT(NO_CRQ_EXTENSIONS))
		cinfo.crq_extensions = 0;
	else
		cinfo.crq_extensions = 1;

	if (HAVE_OPT(LOAD_PUBKEY))
		cinfo.pubkey = OPT_ARG(LOAD_PUBKEY);

	cinfo.pkcs8 = HAVE_OPT(PKCS8);
	cinfo.incert_format = incert_format;
	cinfo.outcert_format = outcert_format;

	if (HAVE_OPT(LOAD_CERTIFICATE))
		cinfo.cert = OPT_ARG(LOAD_CERTIFICATE);

	if (HAVE_OPT(LOAD_REQUEST))
		cinfo.request = OPT_ARG(LOAD_REQUEST);

	if (HAVE_OPT(LOAD_CA_CERTIFICATE))
		cinfo.ca = OPT_ARG(LOAD_CA_CERTIFICATE);

	if (HAVE_OPT(LOAD_CA_PRIVKEY))
		cinfo.ca_privkey = OPT_ARG(LOAD_CA_PRIVKEY);

	if (HAVE_OPT(BITS))
		cinfo.bits = OPT_VALUE_BITS;

	if (HAVE_OPT(CURVE)) {
		gnutls_ecc_curve_t curve = str_to_curve(OPT_ARG(CURVE));
		cinfo.bits = GNUTLS_CURVE_TO_BITS(curve);
	}

	if (HAVE_OPT(SEC_PARAM))
		cinfo.sec_param = OPT_ARG(SEC_PARAM);

	if (HAVE_OPT(PKCS_CIPHER))
		cinfo.pkcs_cipher = OPT_ARG(PKCS_CIPHER);

	if (HAVE_OPT(PASSWORD)) {
		cinfo.password = OPT_ARG(PASSWORD);
		if (HAVE_OPT(GENERATE_PRIVKEY) && cinfo.pkcs8 == 0) {
			fprintf(stderr, "Assuming PKCS #8 format...\n");
			cinfo.pkcs8 = 1;
		}
	}

	if (HAVE_OPT(NULL_PASSWORD)) {
		cinfo.null_password = 1;
		cinfo.password = "";
	}

	if (HAVE_OPT(PROVABLE))
		cinfo.provable = 1;

	if (HAVE_OPT(EMPTY_PASSWORD)) {
		cinfo.empty_password = 1;
		cinfo.password = "";
	}

	if (HAVE_OPT(GENERATE_SELF_SIGNED))
		generate_self_signed(&cinfo);
	else if (HAVE_OPT(GENERATE_CERTIFICATE))
		generate_signed_certificate(&cinfo);
	else if (HAVE_OPT(GENERATE_PROXY))
		generate_proxy_certificate(&cinfo);
	else if (HAVE_OPT(GENERATE_CRL))
		generate_signed_crl(&cinfo);
	else if (HAVE_OPT(UPDATE_CERTIFICATE))
		update_signed_certificate(&cinfo);
	else if (HAVE_OPT(GENERATE_PRIVKEY))
		generate_private_key(&cinfo);
	else if (HAVE_OPT(GENERATE_REQUEST))
		generate_request(&cinfo);
	else if (HAVE_OPT(VERIFY_PROVABLE_PRIVKEY))
		verify_provable_privkey(&cinfo);
	else if (HAVE_OPT(VERIFY_CHAIN))
		verify_chain();
	else if (HAVE_OPT(VERIFY))
		verify_certificate(&cinfo);
	else if (HAVE_OPT(VERIFY_CRL))
		verify_crl(&cinfo);
	else if (HAVE_OPT(CERTIFICATE_INFO))
		certificate_info(0, &cinfo);
	else if (HAVE_OPT(DH_INFO))
		dh_info(infile, outfile, &cinfo);
	else if (HAVE_OPT(CERTIFICATE_PUBKEY))
		certificate_info(1, &cinfo);
	else if (HAVE_OPT(KEY_INFO))
		privkey_info(&cinfo);
	else if (HAVE_OPT(PUBKEY_INFO))
		pubkey_info(NULL, &cinfo);
	else if (HAVE_OPT(FINGERPRINT))
		certificate_fpr(&cinfo);
	else if (HAVE_OPT(KEY_ID))
		pubkey_keyid(&cinfo);
	else if (HAVE_OPT(TO_P12))
		generate_pkcs12(&cinfo);
	else if (HAVE_OPT(P12_INFO))
		pkcs12_info(&cinfo);
	else if (HAVE_OPT(GENERATE_DH_PARAMS))
		generate_prime(outfile, 1, &cinfo);
	else if (HAVE_OPT(GET_DH_PARAMS))
		generate_prime(outfile, 0, &cinfo);
	else if (HAVE_OPT(CRL_INFO))
		crl_info();
	else if (HAVE_OPT(P7_INFO))
		pkcs7_info(&cinfo);
	else if (HAVE_OPT(P7_GENERATE))
		pkcs7_generate(&cinfo);
	else if (HAVE_OPT(P7_SIGN))
		pkcs7_sign(&cinfo, 1);
	else if (HAVE_OPT(P7_DETACHED_SIGN))
		pkcs7_sign(&cinfo, 0);
	else if (HAVE_OPT(P7_VERIFY))
		verify_pkcs7(&cinfo, OPT_ARG(VERIFY_PURPOSE), ENABLED_OPT(P7_SHOW_DATA));
	else if (HAVE_OPT(P8_INFO))
		pkcs8_info();
	else if (HAVE_OPT(SMIME_TO_P7))
		smime_to_pkcs7();
	else if (HAVE_OPT(TO_P8))
		generate_pkcs8(&cinfo);
#ifdef ENABLE_OPENPGP
	else if (HAVE_OPT(PGP_CERTIFICATE_INFO))
		pgp_certificate_info();
	else if (HAVE_OPT(PGP_KEY_INFO))
		pgp_privkey_info();
	else if (HAVE_OPT(PGP_RING_INFO))
		pgp_ring_info();
#endif
	else if (HAVE_OPT(CRQ_INFO))
		crq_info();
	else
		USAGE(1);

	if (outfile != stdout)
		fclose(outfile);

#ifdef ENABLE_PKCS11
	gnutls_pkcs11_deinit();
#endif
	gnutls_global_deinit();
}

void certificate_info(int pubkey, common_info_st * cinfo)
{
	gnutls_x509_crt_t *crts = NULL;
	size_t size;
	int ret, i, count;
	gnutls_datum_t pem;
	unsigned int crt_num;

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret =
	    gnutls_x509_crt_list_import2(&crts, &crt_num, &pem, incert_format, 0);
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	free(pem.data);

	count = crt_num;

	if (count > 1 && outcert_format == GNUTLS_X509_FMT_DER) {
		fprintf(stderr,
			"cannot output multiple certificates in DER format; "
			"using PEM instead");
		outcert_format = GNUTLS_X509_FMT_PEM;
	}

	for (i = 0; i < count; i++) {
		if (i > 0)
			fprintf(outfile, "\n");

		if (outcert_format == GNUTLS_X509_FMT_PEM)
			print_certificate_info(crts[i], outfile, 1);

		if (pubkey)
			pubkey_info(crts[i], cinfo);
		else {
			size = lbuffer_size;
			ret =
			    gnutls_x509_crt_export(crts[i], outcert_format,
						   lbuffer, &size);
			if (ret < 0) {
				fprintf(stderr, "export error: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}

			fwrite(lbuffer, 1, size, outfile);
		}

		gnutls_x509_crt_deinit(crts[i]);
	}
	gnutls_free(crts);
}

#ifdef ENABLE_OPENPGP

void pgp_certificate_info(void)
{
	gnutls_openpgp_crt_t crt;
	size_t size;
	int ret;
	gnutls_datum_t pem, out_data;
	unsigned int verify_status;

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_openpgp_crt_init(&crt);
	if (ret < 0) {
		fprintf(stderr, "openpgp_crt_init: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	ret = gnutls_openpgp_crt_import(crt, &pem, incert_format);

	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	free(pem.data);

	if (outcert_format == GNUTLS_OPENPGP_FMT_BASE64) {
		ret = gnutls_openpgp_crt_print(crt, 0, &out_data);

		if (ret == 0) {
			fprintf(outfile, "%s\n", out_data.data);
			gnutls_free(out_data.data);
		}
	}


	ret = gnutls_openpgp_crt_verify_self(crt, 0, &verify_status);
	if (ret < 0) {
		{
			fprintf(stderr, "verify signature error: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	}

	if (verify_status & GNUTLS_CERT_INVALID) {
		fprintf(outcert_format == GNUTLS_OPENPGP_FMT_RAW ? stderr : outfile,
			"Self Signature verification: failed\n\n");
	} else {
		fprintf(outcert_format == GNUTLS_OPENPGP_FMT_RAW ? stderr : outfile,
			"Self Signature verification: ok (%x)\n\n",
			verify_status);
	}

	size = lbuffer_size;
	ret =
	    gnutls_openpgp_crt_export(crt, outcert_format, lbuffer, &size);
	if (ret < 0) {
		fprintf(stderr, "export error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "%s\n", lbuffer);
	gnutls_openpgp_crt_deinit(crt);
}

void pgp_privkey_info(void)
{
	gnutls_openpgp_privkey_t key;
	unsigned char keyid[GNUTLS_OPENPGP_KEYID_SIZE];
	size_t size;
	int ret, i, subkeys, bits = 0;
	gnutls_datum_t pem;
	const char *cprint;

	size = fread(lbuffer, 1, lbuffer_size - 1, infile);
	lbuffer[size] = 0;

	gnutls_openpgp_privkey_init(&key);

	pem.data = lbuffer;
	pem.size = size;

	ret = gnutls_openpgp_privkey_import(key, &pem, incert_format,
					    NULL, 0);

	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	/* Public key algorithm
	 */
	subkeys = gnutls_openpgp_privkey_get_subkey_count(key);
	if (subkeys < 0) {
		fprintf(stderr, "privkey_get_subkey_count: %s\n",
			gnutls_strerror(subkeys));
		exit(1);
	}

	for (i = -1; i < subkeys; i++) {

		if (i != -1)
			fprintf(outfile, "Subkey[%d]:\n", i);

		fprintf(outfile, "Public Key Info:\n");

		if (i == -1)
			ret =
			    gnutls_openpgp_privkey_get_pk_algorithm(key,
								    NULL);
		else
			ret =
			    gnutls_openpgp_privkey_get_subkey_pk_algorithm
			    (key, i, NULL);

		fprintf(outfile, "\tPublic Key Algorithm: ");
		cprint = gnutls_pk_algorithm_get_name(ret);
		fprintf(outfile, "%s\n", cprint ? cprint : "Unknown");
		fprintf(outfile, "\tKey Security Level: %s\n",
			gnutls_sec_param_get_name
			(gnutls_openpgp_privkey_sec_param(key)));

		/* Print the raw public and private keys
		 */

		if (ret == GNUTLS_PK_RSA) {
			gnutls_datum_t m, e, d, p, q, u;

			if (i == -1)
				ret =
				    gnutls_openpgp_privkey_export_rsa_raw
				    (key, &m, &e, &d, &p, &q, &u);
			else
				ret =
				    gnutls_openpgp_privkey_export_subkey_rsa_raw
				    (key, i, &m, &e, &d, &p, &q, &u);
			if (ret < 0)
				fprintf(stderr,
					"Error in key RSA data export: %s\n",
					gnutls_strerror(ret));
			else
				print_rsa_pkey(outfile, &m, &e, &d, &p, &q,
					       &u, NULL, NULL,
					       HAVE_OPT(CPRINT));

			bits = m.size * 8;
		} else if (ret == GNUTLS_PK_DSA) {
			gnutls_datum_t p, q, g, y, x;

			if (i == -1)
				ret =
				    gnutls_openpgp_privkey_export_dsa_raw
				    (key, &p, &q, &g, &y, &x);
			else
				ret =
				    gnutls_openpgp_privkey_export_subkey_dsa_raw
				    (key, i, &p, &q, &g, &y, &x);
			if (ret < 0)
				fprintf(stderr,
					"Error in key DSA data export: %s\n",
					gnutls_strerror(ret));
			else
				print_dsa_pkey(outfile, &x, &y, &p, &q, &g,
					       HAVE_OPT(CPRINT));

			bits = y.size * 8;
		}

		fprintf(outfile, "\n");

		size = lbuffer_size;
		if (i == -1)
			ret =
			    gnutls_openpgp_privkey_get_key_id(key, keyid);
		else
			ret =
			    gnutls_openpgp_privkey_get_subkey_id(key, i,
								 keyid);

		if (ret < 0) {
			fprintf(stderr,
				"Error in key id calculation: %s\n",
				gnutls_strerror(ret));
		} else {
			fprintf(outfile, "Public key ID: %s\n",
				raw_to_string(keyid, 8));
		}

		size = lbuffer_size;
		if (i == -1)
			ret =
			    gnutls_openpgp_privkey_get_fingerprint(key,
								   lbuffer,
								   &size);
		else
			ret =
			    gnutls_openpgp_privkey_get_subkey_fingerprint
			    (key, i, lbuffer, &size);

		if (ret < 0) {
			fprintf(stderr,
				"Error in fingerprint calculation: %s\n",
				gnutls_strerror(ret));
		} else {
			gnutls_datum_t art;

			fprintf(outfile, "Fingerprint: %s\n",
				raw_to_string(lbuffer, size));

			ret =
			    gnutls_random_art(GNUTLS_RANDOM_ART_OPENSSH,
					      cprint, bits, lbuffer, size,
					      &art);
			if (ret >= 0) {
				fprintf(outfile,
					"Fingerprint's random art:\n%s\n\n",
					art.data);
				gnutls_free(art.data);
			}
		}
	}

	size = lbuffer_size;
	ret = gnutls_openpgp_privkey_export(key, GNUTLS_OPENPGP_FMT_BASE64,
					    NULL, 0, lbuffer, &size);
	if (ret < 0) {
		fprintf(stderr, "export error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "\n%s\n", lbuffer);

	gnutls_openpgp_privkey_deinit(key);
}

void pgp_ring_info(void)
{
	gnutls_openpgp_keyring_t ring;
	gnutls_openpgp_crt_t crt;
	size_t size;
	int ret, i, count;
	gnutls_datum_t pem;

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_openpgp_keyring_init(&ring);
	if (ret < 0) {
		fprintf(stderr, "openpgp_keyring_init: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	ret = gnutls_openpgp_keyring_import(ring, &pem, incert_format);

	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	free(pem.data);

	count = gnutls_openpgp_keyring_get_crt_count(ring);
	if (count >= 0)
		fprintf(outfile,
			"Keyring contains %d OpenPGP certificates\n\n",
			count);
	else {
		fprintf(stderr, "keyring error: %s\n",
			gnutls_strerror(count));
		exit(1);
	}

	for (i = 0; i < count; i++) {
		ret = gnutls_openpgp_keyring_get_crt(ring, i, &crt);
		if (ret < 0) {
			fprintf(stderr, "export error: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		size = lbuffer_size;
		ret = gnutls_openpgp_crt_export(crt, outcert_format,
						lbuffer, &size);
		if (ret < 0) {
			fprintf(stderr, "export error: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		fwrite(lbuffer, 1, size, outfile);
		fprintf(outfile, "\n\n");

		gnutls_openpgp_crt_deinit(crt);


	}

	gnutls_openpgp_keyring_deinit(ring);
}


#endif



static void
print_certificate_info(gnutls_x509_crt_t crt, FILE * out, unsigned int all)
{
	gnutls_datum_t data;
	int ret;

	if (all)
		ret = gnutls_x509_crt_print(crt, full_format, &data);
	else
		ret =
		    gnutls_x509_crt_print(crt,
					  GNUTLS_CRT_PRINT_UNSIGNED_FULL,
					  &data);
	if (ret == 0) {
		fprintf(out, "%s\n", data.data);
		gnutls_free(data.data);
	}

	if (out == stderr && batch == 0)	/* interactive */
		if (read_yesno("Is the above information ok? (y/N): ", 0)
		    == 0) {
			exit(1);
		}
}

static void print_crl_info(gnutls_x509_crl_t crl, FILE * out)
{
	gnutls_datum_t data;
	gnutls_datum_t cout;
	int ret;

	if (outcert_format == GNUTLS_X509_FMT_PEM) {
		ret = gnutls_x509_crl_print(crl, full_format, &data);
		if (ret < 0) {
			fprintf(stderr, "crl_print: %s\n", gnutls_strerror(ret));
			exit(1);
		}
		fprintf(out, "%s\n", data.data);

		gnutls_free(data.data);
	}

	ret =
	    gnutls_x509_crl_export2(crl, outcert_format, &cout);
	if (ret < 0) {
		fprintf(stderr, "crl_export: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fwrite(cout.data, 1, cout.size, outfile);
	gnutls_free(cout.data);
}

void crl_info(void)
{
	gnutls_x509_crl_t crl;
	int ret;
	size_t size;
	gnutls_datum_t pem;

	ret = gnutls_x509_crl_init(&crl);
	if (ret < 0) {
		fprintf(stderr, "crl_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_x509_crl_import(crl, &pem, incert_format);

	free(pem.data);
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	print_crl_info(crl, outfile);

	gnutls_x509_crl_deinit(crl);
}

static void print_crq_info(gnutls_x509_crq_t crq, FILE * out)
{
	gnutls_datum_t data;
	int ret;
	size_t size;

	if (outcert_format == GNUTLS_X509_FMT_PEM) {
		ret = gnutls_x509_crq_print(crq, full_format, &data);
		if (ret < 0) {
			fprintf(stderr, "crq_print: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		fprintf(out, "%s\n", data.data);

		gnutls_free(data.data);
	}

	ret = gnutls_x509_crq_verify(crq, 0);
	if (ret < 0) {
		fprintf(outcert_format == GNUTLS_X509_FMT_DER ? out : stderr,
			"Self signature: FAILED\n\n");
	} else {
		fprintf(outcert_format == GNUTLS_X509_FMT_DER ? out : stderr,
			"Self signature: verified\n\n");
	}

	size = lbuffer_size;
	ret = gnutls_x509_crq_export(crq, outcert_format, lbuffer, &size);
	if (ret < 0) {
		fprintf(stderr, "crq_export: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);
}

void crq_info(void)
{
	gnutls_x509_crq_t crq;
	int ret;
	size_t size;
	gnutls_datum_t pem;

	ret = gnutls_x509_crq_init(&crq);
	if (ret < 0) {
		fprintf(stderr, "crq_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_x509_crq_import(crq, &pem, incert_format);

	free(pem.data);
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	print_crq_info(crq, outfile);

	gnutls_x509_crq_deinit(crq);
}

void privkey_info(common_info_st * cinfo)
{
	gnutls_x509_privkey_t key;
	size_t size;
	int ret;
	gnutls_datum_t pem;
	const char *pass;
	unsigned int flags = 0;

	size = fread(lbuffer, 1, lbuffer_size - 1, infile);
	lbuffer[size] = 0;

	gnutls_x509_privkey_init(&key);

	pem.data = lbuffer;
	pem.size = size;

	ret =
	    gnutls_x509_privkey_import2(key, &pem, incert_format, NULL, GNUTLS_PKCS_PLAIN);

	/* If we failed to import the certificate previously try PKCS #8 */
	if (ret == GNUTLS_E_DECRYPTION_FAILED) {
		fprintf(stderr, "Encrypted structure detected...\n");

		pkcs8_info_int(&pem, incert_format, 1, outfile, "");

		pass = get_password(cinfo, &flags, 0);

		ret = gnutls_x509_privkey_import2(key, &pem,
						  incert_format, pass,
						  flags);
	}
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}
	/* On this option we may import from PKCS #8 but we are always exporting
	 * to our format. */
	cinfo->pkcs8 = 0;

	print_private_key(outfile, cinfo, key);

	ret = gnutls_x509_privkey_verify_params(key);
	if (ret < 0)
		fprintf(outfile,
			"\n** Private key parameters validation failed **\n\n");

	gnutls_x509_privkey_deinit(key);
}


/* Generate a PKCS #10 certificate request.
 */
void generate_request(common_info_st * cinfo)
{
	gnutls_x509_crq_t crq;
	gnutls_x509_privkey_t xkey;
	gnutls_pubkey_t pubkey;
	gnutls_privkey_t pkey;
	int ret, ca_status, path_len, pk;
	const char *pass;
	unsigned int usage = 0;

	fprintf(stderr, "Generating a PKCS #10 certificate request...\n");

	ret = gnutls_x509_crq_init(&crq);
	if (ret < 0) {
		fprintf(stderr, "crq_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}


	/* Load the private key.
	 */
	pkey = load_private_key(0, cinfo);
	if (!pkey) {
		ret = gnutls_privkey_init(&pkey);
		if (ret < 0) {
			fprintf(stderr, "privkey_init: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		xkey = generate_private_key_int(cinfo);

		print_private_key(outfile, cinfo, xkey);

		ret =
		    gnutls_privkey_import_x509(pkey, xkey,
					       GNUTLS_PRIVKEY_IMPORT_AUTO_RELEASE);
		if (ret < 0) {
			fprintf(stderr, "privkey_import_x509: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	}

	pubkey = load_public_key_or_import(1, pkey, cinfo);

	pk = gnutls_pubkey_get_pk_algorithm(pubkey, NULL);

	/* Set the DN.
	 */
	get_dn_crq_set(crq);

	get_cn_crq_set(crq);
	get_unit_crq_set(crq);
	get_organization_crq_set(crq);
	get_locality_crq_set(crq);
	get_state_crq_set(crq);
	get_country_crq_set(crq);

	get_dc_set(TYPE_CRQ, crq);
	get_uid_crq_set(crq);
	get_oid_crq_set(crq);

	get_dns_name_set(TYPE_CRQ, crq);
	get_uri_set(TYPE_CRQ, crq);
	get_ip_addr_set(TYPE_CRQ, crq);
	get_email_set(TYPE_CRQ, crq);
	get_other_name_set(TYPE_CRQ, crq);

	pass = get_challenge_pass();

	if (pass != NULL && pass[0] != 0) {
		ret = gnutls_x509_crq_set_challenge_password(crq, pass);
		if (ret < 0) {
			fprintf(stderr, "set_pass: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
	}

	if (cinfo->crq_extensions != 0) {
		ca_status = get_ca_status();
		if (ca_status)
			path_len = get_path_len();
		else
			path_len = -1;

		ret =
		    gnutls_x509_crq_set_basic_constraints(crq, ca_status,
							  path_len);
		if (ret < 0) {
			fprintf(stderr, "set_basic_constraints: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		if (pk == GNUTLS_PK_RSA) {
			ret = get_sign_status(1);
			if (ret)
				usage |= GNUTLS_KEY_DIGITAL_SIGNATURE;

			/* Only ask for an encryption certificate
			 * if it is an RSA one */
			ret = get_encrypt_status(1);
			if (ret)
				usage |= GNUTLS_KEY_KEY_ENCIPHERMENT;
			else
				usage |= GNUTLS_KEY_DIGITAL_SIGNATURE;
		} else {	/* DSA and ECDSA are always signing */
			if (get_encrypt_status(1))
				fprintf(stderr, "warning: this algorithm does not support encryption; disabling the encryption flag\n");

			usage |= GNUTLS_KEY_DIGITAL_SIGNATURE;
		}

		if (ca_status) {
			ret = get_cert_sign_status();
			if (ret)
				usage |= GNUTLS_KEY_KEY_CERT_SIGN;

			ret = get_crl_sign_status();
			if (ret)
				usage |= GNUTLS_KEY_CRL_SIGN;

			ret = get_code_sign_status();
			if (ret) {
				ret = gnutls_x509_crq_set_key_purpose_oid
				    (crq, GNUTLS_KP_CODE_SIGNING, 0);
				if (ret < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(ret));
					exit(1);
				}
			}

			ret = get_ocsp_sign_status();
			if (ret) {
				ret = gnutls_x509_crq_set_key_purpose_oid
				    (crq, GNUTLS_KP_OCSP_SIGNING, 0);
				if (ret < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(ret));
					exit(1);
				}
			}

			ret = get_time_stamp_status();
			if (ret) {
				ret = gnutls_x509_crq_set_key_purpose_oid
				    (crq, GNUTLS_KP_TIME_STAMPING, 0);
				if (ret < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(ret));
					exit(1);
				}
			}

			ret = get_ipsec_ike_status();
			if (ret) {
				ret = gnutls_x509_crq_set_key_purpose_oid
				    (crq, GNUTLS_KP_IPSEC_IKE, 0);
				if (ret < 0) {
					fprintf(stderr, "key_kp: %s\n",
						gnutls_strerror(ret));
					exit(1);
				}
			}
		}

		ret = gnutls_x509_crq_set_key_usage(crq, usage);
		if (ret < 0) {
			fprintf(stderr, "key_usage: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		ret = get_tls_client_status();
		if (ret != 0) {
			ret = gnutls_x509_crq_set_key_purpose_oid
			    (crq, GNUTLS_KP_TLS_WWW_CLIENT, 0);
			if (ret < 0) {
				fprintf(stderr, "key_kp: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}

		ret = get_tls_server_status();
		if (ret != 0) {
			ret = gnutls_x509_crq_set_key_purpose_oid
			    (crq, GNUTLS_KP_TLS_WWW_SERVER, 0);
			if (ret < 0) {
				fprintf(stderr, "key_kp: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}

		get_key_purpose_set(TYPE_CRQ, crq);
	}

	ret = gnutls_x509_crq_set_pubkey(crq, pubkey);
	if (ret < 0) {
		fprintf(stderr, "set_key: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	ret =
	    gnutls_x509_crq_privkey_sign(crq, pkey,
					 get_dig_for_pub(pubkey), 0);
	if (ret < 0) {
		fprintf(stderr, "sign: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	print_crq_info(crq, outfile);

	gnutls_x509_crq_deinit(crq);
	gnutls_privkey_deinit(pkey);
	gnutls_pubkey_deinit(pubkey);

}

static void print_verification_res(FILE * outfile, unsigned int output);

static int detailed_verification(gnutls_x509_crt_t cert,
				 gnutls_x509_crt_t issuer,
				 gnutls_x509_crl_t crl,
				 unsigned int verification_output)
{
	char name[512];
	char tmp[255];
	char issuer_name[512];
	size_t name_size;
	size_t issuer_name_size;
	int ret;

	issuer_name_size = sizeof(issuer_name);
	ret =
	    gnutls_x509_crt_get_issuer_dn(cert, issuer_name,
					  &issuer_name_size);
	if (ret < 0) {
		fprintf(stderr, "gnutls_x509_crt_get_issuer_dn: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	name_size = sizeof(name);
	ret = gnutls_x509_crt_get_dn(cert, name, &name_size);
	if (ret < 0) {
		fprintf(stderr, "gnutls_x509_crt_get_dn: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "\tSubject: %s\n", name);
	fprintf(outfile, "\tIssuer: %s\n", issuer_name);

	if (issuer != NULL) {
		issuer_name_size = sizeof(issuer_name);
		ret =
		    gnutls_x509_crt_get_dn(issuer, issuer_name,
					   &issuer_name_size);
		if (ret < 0) {
			fprintf(stderr,
				"gnutls_x509_crt_get_issuer_dn: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		fprintf(outfile, "\tChecked against: %s\n", issuer_name);
	}

	if (crl != NULL) {
		gnutls_datum_t data;

		issuer_name_size = sizeof(issuer_name);
		ret =
		    gnutls_x509_crl_get_issuer_dn(crl, issuer_name,
						  &issuer_name_size);
		if (ret < 0) {
			fprintf(stderr,
				"gnutls_x509_crl_get_issuer_dn: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		name_size = sizeof(tmp);
		ret =
		    gnutls_x509_crl_get_number(crl, tmp, &name_size, NULL);
		if (ret < 0)
			strcpy(name, "unnumbered");
		else {
			data.data = (void *) tmp;
			data.size = name_size;

			name_size = sizeof(name);
			ret = gnutls_hex_encode(&data, name, &name_size);
			if (ret < 0) {
				fprintf(stderr, "gnutls_hex_encode: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}
		fprintf(outfile, "\tChecked against CRL[%s] of: %s\n",
			name, issuer_name);
	}

	fprintf(outfile, "\tOutput: ");
	print_verification_res(outfile, verification_output);

	fputs("\n\n", outfile);

	return 0;
}

static void load_data(common_info_st *cinfo, gnutls_datum_t *data)
{
	FILE *fp;
	size_t size;

	fp = fopen(cinfo->data_file, "r");
	if (fp == NULL) {
		fprintf(stderr, "Could not open %s\n", cinfo->data_file);
		exit(1);
	}

	data->data = (void *) fread_file(fp, &size);
	if (data->data == NULL) {
		fprintf(stderr, "Error reading data file");
		exit(1);
	}

	data->size = size;
	fclose(fp);
}

static gnutls_x509_trust_list_t load_tl(common_info_st * cinfo)
{
	gnutls_x509_trust_list_t list;
	int ret;
	FILE *fp;
	gnutls_datum_t tmp = {NULL, 0}, tmp2 = {NULL, 0};
	char *cas, *crls;
	size_t ca_size, crl_size;

	ret = gnutls_x509_trust_list_init(&list, 0);
	if (ret < 0) {
		fprintf(stderr, "gnutls_x509_trust_list_init: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (cinfo->ca == NULL) { /* system */
		ret = gnutls_x509_trust_list_add_system_trust(list, 0, 0);
		if (ret < 0) {
			fprintf(stderr, "Error loading system trust: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
		fprintf(stderr, "Loaded system trust (%d CAs available)\n", ret);
	} else if (cinfo->ca != NULL) {
		fp = fopen(cinfo->ca, "r");
		if (fp == NULL) {
			fprintf(stderr, "Could not open %s\n", cinfo->ca);
			exit(1);
		}

		cas = (void *) fread_file(fp, &ca_size);
		if (cas == NULL) {
			fprintf(stderr, "Error reading CA list");
			exit(1);
		}

		tmp.data = (void *) cas;
		tmp.size = ca_size;
		fclose(fp);

		if (cinfo->crl) {
			fp = fopen(cinfo->crl, "r");
			if (fp == NULL) {
				fprintf(stderr, "Could not open %s\n", cinfo->crl);
				exit(1);
			}

			crls = (void *) fread_file(fp, &crl_size);
			if (crls == NULL) {
				fprintf(stderr, "Error reading CRL list");
				exit(1);
			}

			fclose(fp);

			tmp2.data = (void *) crls;
			tmp2.size = crl_size;
		}

		ret =
		    gnutls_x509_trust_list_add_trust_mem(list, &tmp, 
		    				tmp2.data?&tmp2:NULL,
						cinfo->incert_format,
						0, 0);
		if (ret < 0) {
			int ret2 =
			    gnutls_x509_trust_list_add_trust_mem(list, &tmp, 
			    				tmp2.data?&tmp2:NULL,
							GNUTLS_X509_FMT_PEM,
							0, 0);
			if (ret2 >= 0)
				ret = ret2;
		}

		if (ret < 0) {
			fprintf(stderr, "gnutls_x509_trust_add_trust_mem: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		free(tmp.data);
		free(tmp2.data);

		fprintf(stderr, "Loaded CAs (%d available)\n", ret);
	}

	return list;
}

/* Will verify a certificate chain. If no CA certificates
 * are provided, then the last certificate in the certificate
 * chain is used as a CA.
 *
 * If @system is non-zero then the system's CA will be used.
 */
static int
_verify_x509_mem(const void *cert, int cert_size, const void *ca,
		 int ca_size, unsigned system, const char *purpose,
		 const char *hostname, const char *email)
{
	int ret;
	unsigned i;
	gnutls_datum_t tmp;
	gnutls_x509_crt_t *x509_cert_list = NULL;
	gnutls_x509_crt_t *x509_ca_list = NULL;
	gnutls_x509_crt_t *pca_list = NULL;
	gnutls_x509_crl_t *x509_crl_list = NULL;
	unsigned int x509_ncerts, x509_ncrls = 0, x509_ncas = 0;
	gnutls_x509_trust_list_t list;
	unsigned int output;
	unsigned vflags;

	ret = gnutls_x509_trust_list_init(&list, 0);
	if (ret < 0) {
		fprintf(stderr, "gnutls_x509_trust_list_init: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (system != 0) {
		ret = gnutls_x509_trust_list_add_system_trust(list, 0, 0);
		if (ret < 0) {
			fprintf(stderr, "Error loading system trust: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
		fprintf(stderr, "Loaded system trust (%d CAs available)\n", ret);

		x509_ncas = ret;

		tmp.data = (void *) cert;
		tmp.size = cert_size;

		/* ignore errors. CRLs might not be given */
		ret =
		    gnutls_x509_crt_list_import2(&x509_cert_list,
						 &x509_ncerts, &tmp,
						 GNUTLS_X509_FMT_PEM, 0);
		if (ret < 0 || x509_ncerts < 1) {
			fprintf(stderr, "error parsing CRTs: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

	} else {
		if (ca == NULL) {
			tmp.data = (void *) cert;
			tmp.size = cert_size;
		} else {
			tmp.data = (void *) ca;
			tmp.size = ca_size;

			/* Load CAs */
			ret =
			    gnutls_x509_crt_list_import2(&x509_ca_list,
							 &x509_ncas, &tmp,
							 GNUTLS_X509_FMT_PEM,
							 0);
			if (ret < 0 || x509_ncas < 1) {
				fprintf(stderr, "error parsing CAs: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
		}
		pca_list = x509_ca_list;

		ret =
		    gnutls_x509_crl_list_import2(&x509_crl_list,
						 &x509_ncrls, &tmp,
						 GNUTLS_X509_FMT_PEM, 0);
		if (ret < 0) {
			x509_crl_list = NULL;
			x509_ncrls = 0;
		}


		tmp.data = (void *) cert;
		tmp.size = cert_size;

		/* ignore errors. CRLs might not be given */
		ret =
		    gnutls_x509_crt_list_import2(&x509_cert_list,
						 &x509_ncerts, &tmp,
						 GNUTLS_X509_FMT_PEM, 0);
		if (ret < 0 || x509_ncerts < 1) {
			fprintf(stderr, "error parsing CRTs: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		if (ca == NULL) {
			pca_list = &x509_cert_list[x509_ncerts - 1];
			x509_ncas = 1;
		}

		ret =
		    gnutls_x509_trust_list_add_cas(list, pca_list,
						   x509_ncas, 0);
		if (ret < 0) {
			fprintf(stderr, "gnutls_x509_trust_add_cas: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		ret =
		    gnutls_x509_trust_list_add_crls(list, x509_crl_list,
						    x509_ncrls, 0, 0);
		if (ret < 0) {
			fprintf(stderr, "gnutls_x509_trust_add_crls: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

	}

	fprintf(stdout, "Loaded %d certificates, %d CAs and %d CRLs\n\n",
		x509_ncerts, x509_ncas, x509_ncrls);

	vflags = GNUTLS_VERIFY_DO_NOT_ALLOW_SAME;

	if (HAVE_OPT(VERIFY_ALLOW_BROKEN))
		vflags |= GNUTLS_VERIFY_ALLOW_BROKEN;


	if (purpose || hostname || email) {
		gnutls_typed_vdata_st vdata[2];
		unsigned vdata_size = 0;

		if (purpose) {
			vdata[vdata_size].type = GNUTLS_DT_KEY_PURPOSE_OID;
			vdata[vdata_size].data = (void*)purpose;
			vdata[vdata_size].size = strlen(purpose);
			vdata_size++;
		}

		if (hostname) {
			vdata[vdata_size].type = GNUTLS_DT_DNS_HOSTNAME;
			vdata[vdata_size].data = (void*)hostname;
			vdata[vdata_size].size = strlen(hostname);
			vdata_size++;
		} else if (email) {
			vdata[vdata_size].type = GNUTLS_DT_RFC822NAME;
			vdata[vdata_size].data = (void*)email;
			vdata[vdata_size].size = strlen(email);
			vdata_size++;
		}

		ret =
		    gnutls_x509_trust_list_verify_crt2(list, x509_cert_list,
						       x509_ncerts,
						       vdata,
						       vdata_size,
						       vflags,
						       &output,
						       detailed_verification);
	} else { 
		ret =
		    gnutls_x509_trust_list_verify_crt(list, x509_cert_list,
						      x509_ncerts,
						      vflags,
						      &output,
						      detailed_verification);
	}
	if (ret < 0) {
		fprintf(stderr, "gnutls_x509_trusted_list_verify_crt: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "Chain verification output: ");
	print_verification_res(outfile, output);

	fprintf(outfile, "\n\n");

	gnutls_x509_trust_list_deinit(list, 0);
	for (i=0;i<x509_ncerts;i++)
		gnutls_x509_crt_deinit(x509_cert_list[i]);
	gnutls_free(x509_cert_list);
	if (x509_ca_list != NULL) {
		for (i=0;i<x509_ncas;i++)
			gnutls_x509_crt_deinit(x509_ca_list[i]);
		gnutls_free(x509_ca_list);
	}
	for (i=0;i<x509_ncrls;i++)
		gnutls_x509_crl_deinit(x509_crl_list[i]);
	gnutls_free(x509_crl_list);

	if (output != 0)
		exit(EXIT_FAILURE);

	return 0;
}

static void print_verification_res(FILE * out, unsigned int output)
{
	gnutls_datum_t pout;
	int ret;

	if (output) {
		fprintf(out, "Not verified.");
	} else {
		fprintf(out, "Verified.");
	}

	ret =
	    gnutls_certificate_verification_status_print(output,
							 GNUTLS_CRT_X509,
							 &pout, 0);
	if (ret < 0) {
		fprintf(stderr, "error: %s\n", gnutls_strerror(ret));
		exit(EXIT_FAILURE);
	}

	fprintf(out, " %s", pout.data);
	gnutls_free(pout.data);
}

static void verify_chain(void)
{
	char *buf;
	size_t size;

	buf = (void *) fread_file(infile, &size);
	if (buf == NULL) {
		fprintf(stderr, "Error reading chain");
		exit(1);
	}

	buf[size] = 0;

	_verify_x509_mem(buf, size, NULL, 0, 0, OPT_ARG(VERIFY_PURPOSE),
	                 OPT_ARG(VERIFY_HOSTNAME), OPT_ARG(VERIFY_EMAIL));
	free(buf);
}

static void verify_certificate(common_info_st * cinfo)
{
	char *cert;
	char *cas = NULL;
	size_t cert_size, ca_size = 0;
	FILE *ca_file;

	cert = (void *) fread_file(infile, &cert_size);
	if (cert == NULL) {
		fprintf(stderr, "Error reading certificate chain");
		exit(1);
	}

	if (cinfo->ca != NULL) {
		ca_file = fopen(cinfo->ca, "r");
		if (ca_file == NULL) {
			fprintf(stderr, "Could not open %s\n", cinfo->ca);
			exit(1);
		}

		cert[cert_size] = 0;

		cas = (void *) fread_file(ca_file, &ca_size);
		if (cas == NULL) {
			fprintf(stderr, "Error reading CA list");
			exit(1);
		}

		cas[ca_size] = 0;
		fclose(ca_file);
	}

	_verify_x509_mem(cert, cert_size, cas, ca_size,
			 (cinfo->ca != NULL) ? 0 : 1, OPT_ARG(VERIFY_PURPOSE),
			 OPT_ARG(VERIFY_HOSTNAME), OPT_ARG(VERIFY_EMAIL));
	free(cert);
	free(cas);


}

void verify_crl(common_info_st * cinfo)
{
	size_t size, dn_size;
	char dn[128];
	unsigned int output;
	int ret;
	gnutls_datum_t pem, pout;
	gnutls_x509_crl_t crl;
	gnutls_x509_crt_t issuer;

	issuer = load_ca_cert(1, cinfo);

	fprintf(outfile, "\nCA certificate:\n");

	dn_size = sizeof(dn);
	ret = gnutls_x509_crt_get_dn(issuer, dn, &dn_size);
	if (ret < 0) {
		fprintf(stderr, "crt_get_dn: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "\tSubject: %s\n\n", dn);

	ret = gnutls_x509_crl_init(&crl);
	if (ret < 0) {
		fprintf(stderr, "crl_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	pem.data = (void *) fread_file(infile, &size);
	pem.size = size;

	if (!pem.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_x509_crl_import(crl, &pem, incert_format);
	free(pem.data);
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	print_crl_info(crl, outfile);

	fprintf(outfile, "Verification output: ");
	ret = gnutls_x509_crl_verify(crl, &issuer, 1, 0, &output);
	if (ret < 0) {
		fprintf(stderr, "verification error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (output) {
		fprintf(outfile, "Not verified. ");
	} else {
		fprintf(outfile, "Verified.");
	}

	ret =
	    gnutls_certificate_verification_status_print(output,
							 GNUTLS_CRT_X509,
							 &pout, 0);
	if (ret < 0) {
		fprintf(stderr, "error: %s\n", gnutls_strerror(ret));
		exit(EXIT_FAILURE);
	}

	fprintf(outfile, " %s", pout.data);
	gnutls_free(pout.data);

	fprintf(outfile, "\n");
}

static void print_dn(const char *prefix, const gnutls_datum_t *raw)
{
	gnutls_x509_dn_t dn = NULL;
	gnutls_datum_t str = {NULL, 0};
	int ret;

	ret = gnutls_x509_dn_init(&dn);
	if (ret < 0)
		return;

	ret = gnutls_x509_dn_import(dn, raw);
	if (ret < 0)
		goto cleanup;

	ret = gnutls_x509_dn_get_str(dn, &str);
	if (ret < 0)
		goto cleanup;

	fprintf(outfile, "%s: %s\n", prefix, str.data);

 cleanup:
 	gnutls_x509_dn_deinit(dn);
 	gnutls_free(str.data);
}

static void print_raw(const char *prefix, const gnutls_datum_t *raw)
{
	char data[512];
	size_t data_size;
	int ret;

	if (raw->data == NULL || raw->size == 0)
		return;

	data_size = sizeof(data);
	ret = gnutls_hex_encode(raw, data, &data_size);
	if (ret < 0) {
		fprintf(stderr, "gnutls_hex_encode: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(outfile, "%s: %s\n", prefix, data);
}

static void print_pkcs7_sig_info(gnutls_pkcs7_signature_info_st *info, common_info_st *cinfo)
{
	unsigned i;
	char *oid;
	gnutls_datum_t data;
	char prefix[128];
	int ret;

	print_dn("\tSigner's issuer DN", &info->issuer_dn);
	print_raw("\tSigner's serial", &info->signer_serial);
	print_raw("\tSigner's issuer key ID", &info->issuer_keyid);
	if (info->signing_time != -1)
		fprintf(outfile, "\tSigning time: %s", ctime(&info->signing_time));

	fprintf(outfile, "\tSignature Algorithm: %s\n", gnutls_sign_get_name(info->algo));

	if (info->signed_attrs) {
		for (i=0;;i++) {
			ret = gnutls_pkcs7_get_attr(info->signed_attrs, i, &oid, &data, 0);
			if (ret < 0)
				break;
			if (i==0)
				fprintf(outfile, "\tSigned Attributes:\n");

			snprintf(prefix, sizeof(prefix), "\t\t%s", oid);
			print_raw(prefix, &data);
			gnutls_free(data.data);
		}
	}
	if (info->unsigned_attrs) {
		for (i=0;;i++) {
			ret = gnutls_pkcs7_get_attr(info->unsigned_attrs, i, &oid, &data, 0);
			if (ret < 0)
				break;
			if (i==0)
				fprintf(outfile, "\tUnsigned Attributes:\n");

			snprintf(prefix, sizeof(prefix), "\t\t%s", oid);
			print_raw(prefix, &data);
			gnutls_free(data.data);
		}
	}
	fprintf(outfile, "\n");
}

void verify_pkcs7(common_info_st * cinfo, const char *purpose, unsigned display_data)
{
	gnutls_pkcs7_t pkcs7;
	int ret, ecode;
	size_t size;
	gnutls_datum_t data, detached = {NULL,0};
	gnutls_datum_t tmp = {NULL,0};
	gnutls_datum_t embdata = {NULL,0};
	int i;
	gnutls_pkcs7_signature_info_st info;
	gnutls_x509_trust_list_t tl = NULL;
	gnutls_typed_vdata_st vdata[2];
	unsigned vdata_size = 0;
	gnutls_x509_crt_t signer = NULL;
	unsigned flags = 0;

	ret = gnutls_pkcs7_init(&pkcs7);
	if (ret < 0) {
		fprintf(stderr, "p7_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	data.data = (void *) fread_file(infile, &size);
	data.size = size;

	if (!data.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	ret = gnutls_pkcs7_import(pkcs7, &data, cinfo->incert_format);
	free(data.data);
	if (ret < 0) {
		fprintf(stderr, "import error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	if (cinfo->cert != NULL) {
		signer = load_cert(1, cinfo);
	} else { /* trust list */
		tl = load_tl(cinfo);
		if (tl == NULL) {
			fprintf(stderr, "error loading trust list\n");
		}
	}

	if (cinfo->data_file)
		load_data(cinfo, &detached);

	if (purpose) {
		if (purpose) {
			vdata[vdata_size].type = GNUTLS_DT_KEY_PURPOSE_OID;
			vdata[vdata_size].data = (void*)purpose;
			vdata[vdata_size].size = strlen(purpose);
			vdata_size++;
		}
	}

	ecode = 1;
	for (i=0;;i++) {
		ret = gnutls_pkcs7_get_signature_info(pkcs7, i, &info);
		if (ret < 0)
			break;

		if (!display_data) {
			if (i==0)
				fprintf(outfile, "Signers:\n");
			print_pkcs7_sig_info(&info, cinfo);
		} else {
			if (!detached.data) {
				ret = gnutls_pkcs7_get_embedded_data(pkcs7, i, &tmp);
				if (ret != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE || i == 0) {
					if (ret < 0) {
						fprintf(stderr, "error getting embedded data: %s\n", gnutls_strerror(ret));
						exit(1);
					}

					/* check if the embedded data in subsequent calls remain the same */
					if (i != 0) {
						if (tmp.size != embdata.size || memcmp(embdata.data, tmp.data, tmp.size) != 0) {
							fprintf(stderr, "error: the embedded data differ in signed data with index %d\n", i);
							exit(1);
						}
					}

					if (i == 0) {
						fwrite(tmp.data, 1, tmp.size, outfile);
						embdata.data = tmp.data;
						embdata.size = tmp.size;
						tmp.data = NULL;
					} else {
						gnutls_free(tmp.data);
					}
				}
			} else {
				if (i==0)
					fwrite(detached.data, 1, detached.size, outfile);
			}
		}

		gnutls_pkcs7_signature_info_deinit(&info);

		if (HAVE_OPT(VERIFY_ALLOW_BROKEN))
			flags |= GNUTLS_VERIFY_ALLOW_BROKEN;

		if (signer)
			ret = gnutls_pkcs7_verify_direct(pkcs7, signer, i, detached.data!=NULL?&detached:NULL, flags);
		else
			ret = gnutls_pkcs7_verify(pkcs7, tl, vdata, vdata_size, i, detached.data!=NULL?&detached:NULL, flags);
		if (ret < 0) {
			fprintf(stderr, "\tSignature status: verification failed: %s\n", gnutls_strerror(ret));
			ecode = 1;
		} else {
			fprintf(stderr, "\tSignature status: ok\n");
			ecode = 0;
		}
	}


	gnutls_pkcs7_deinit(pkcs7);
	if (signer)
		gnutls_x509_crt_deinit(signer);
	else
		gnutls_x509_trust_list_deinit(tl, 1);
	free(detached.data);
	gnutls_free(embdata.data);
	exit(ecode);
}

void pkcs7_sign(common_info_st * cinfo, unsigned embed)
{
	gnutls_pkcs7_t pkcs7;
	gnutls_privkey_t key;
	int ret;
	size_t size;
	gnutls_datum_t data;
	unsigned flags = 0;
	gnutls_x509_crt_t signer;

	if (ENABLED_OPT(P7_TIME))
		flags |= GNUTLS_PKCS7_INCLUDE_TIME;

	if (ENABLED_OPT(P7_INCLUDE_CERT))
		flags |= GNUTLS_PKCS7_INCLUDE_CERT;

	ret = gnutls_pkcs7_init(&pkcs7);
	if (ret < 0) {
		fprintf(stderr, "p7_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	data.data = (void *) fread_file(infile, &size);
	data.size = size;

	if (!data.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	signer = load_cert(1, cinfo);
	key = load_private_key(1, cinfo);

	if (embed)
		flags |= GNUTLS_PKCS7_EMBED_DATA;

	ret = gnutls_pkcs7_sign(pkcs7, signer, key, &data, NULL, NULL, get_dig(signer), flags);
	if (ret < 0) {
		fprintf(stderr, "Error signing: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	size = lbuffer_size;
	ret =
	    gnutls_pkcs7_export(pkcs7, outcert_format, lbuffer, &size);
	if (ret < 0) {
		fprintf(stderr, "pkcs7_export: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

	gnutls_privkey_deinit(key);
	gnutls_x509_crt_deinit(signer);
	gnutls_pkcs7_deinit(pkcs7);
	exit(0);
}

void pkcs7_generate(common_info_st * cinfo)
{
	gnutls_pkcs7_t pkcs7;
	int ret;
	size_t crl_size = 0, crt_size = 0;
	gnutls_x509_crt_t *crts;
	gnutls_x509_crl_t *crls;
	gnutls_datum_t tmp;
	unsigned i;

	crts = load_cert_list(1, &crt_size, cinfo);
	crls = load_crl_list(0, &crl_size, cinfo);

	ret = gnutls_pkcs7_init(&pkcs7);
	if (ret < 0) {
		fprintf(stderr, "p7_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	for (i=0;i<crt_size;i++) {
		ret = gnutls_pkcs7_set_crt(pkcs7, crts[i]);
		if (ret < 0) {
			fprintf(stderr, "Error adding cert: %s\n", gnutls_strerror(ret));
			exit(1);
		}
		gnutls_x509_crt_deinit(crts[i]);
	}
	gnutls_free(crts);

	for (i=0;i<crl_size;i++) {
		ret = gnutls_pkcs7_set_crl(pkcs7, crls[i]);
		if (ret < 0) {
			fprintf(stderr, "Error adding CRL: %s\n", gnutls_strerror(ret));
			exit(1);
		}
		gnutls_x509_crl_deinit(crls[i]);
	}
	gnutls_free(crls);

	ret =
	    gnutls_pkcs7_export2(pkcs7, outcert_format, &tmp);
	if (ret < 0) {
		fprintf(stderr, "pkcs7_export: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fwrite(tmp.data, 1, tmp.size, outfile);
	gnutls_free(tmp.data);

	gnutls_pkcs7_deinit(pkcs7);
	exit(0);
}


void generate_pkcs8(common_info_st * cinfo)
{
	gnutls_x509_privkey_t key;
	int result;
	size_t size;
	unsigned int flags = 0;
	const char *password;

	fprintf(stderr, "Generating a PKCS #8 key structure...\n");

	key = load_x509_private_key(1, cinfo);

	password = get_password(cinfo, &flags, 1);

	flags |= cipher_to_flags(cinfo->pkcs_cipher);

	size = lbuffer_size;
	result =
	    gnutls_x509_privkey_export_pkcs8(key, outcert_format,
					     password, flags, lbuffer,
					     &size);

	if (result < 0) {
		fprintf(stderr, "key_export: %s\n", gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);

}


#include <gnutls/pkcs12.h>
#include <unistd.h>

void generate_pkcs12(common_info_st * cinfo)
{
	gnutls_pkcs12_t pkcs12;
	gnutls_x509_crt_t *crts, ca_crt;
	gnutls_x509_privkey_t *keys;
	int result;
	size_t size;
	gnutls_datum_t data;
	const char *pass;
	const char *name;
	unsigned int flags = 0, i;
	gnutls_datum_t key_id;
	unsigned char _key_id[64];
	int indx;
	size_t ncrts;
	size_t nkeys;

	fprintf(stderr, "Generating a PKCS #12 structure...\n");

	keys = load_privkey_list(0, &nkeys, cinfo);
	crts = load_cert_list(0, &ncrts, cinfo);
	ca_crt = load_ca_cert(0, cinfo);

	if (HAVE_OPT(P12_NAME)) {
		name = OPT_ARG(P12_NAME);
	} else {
		name = get_pkcs12_key_name();
	}

	result = gnutls_pkcs12_init(&pkcs12);
	if (result < 0) {
		fprintf(stderr, "pkcs12_init: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	pass = get_password(cinfo, &flags, 1);
	flags |= cipher_to_flags(cinfo->pkcs_cipher);

	for (i = 0; i < ncrts; i++) {
		gnutls_pkcs12_bag_t bag;

		result = gnutls_pkcs12_bag_init(&bag);
		if (result < 0) {
			fprintf(stderr, "bag_init: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_bag_set_crt(bag, crts[i]);
		if (result < 0) {
			fprintf(stderr, "set_crt[%d]: %s\n", i,
				gnutls_strerror(result));
			exit(1);
		}

		indx = result;

		if (i == 0) {	/* only the first certificate gets the friendly name */
			result =
			    gnutls_pkcs12_bag_set_friendly_name(bag, indx,
								name);
			if (result < 0) {
				fprintf(stderr,
					"bag_set_friendly_name: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		size = sizeof(_key_id);
		result =
		    gnutls_x509_crt_get_key_id(crts[i], GNUTLS_KEYID_USE_SHA1, _key_id, &size);
		if (result < 0) {
			fprintf(stderr, "key_id[%d]: %s\n", i,
				gnutls_strerror(result));
			exit(1);
		}

		key_id.data = _key_id;
		key_id.size = size;

		result = gnutls_pkcs12_bag_set_key_id(bag, indx, &key_id);
		if (result < 0) {
			fprintf(stderr, "bag_set_key_id: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_bag_encrypt(bag, pass, flags);
		if (result < 0) {
			fprintf(stderr, "bag_encrypt: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_set_bag(pkcs12, bag);
		if (result < 0) {
			fprintf(stderr, "set_bag: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
		gnutls_pkcs12_bag_deinit(bag);
	}

	/* Add the ca cert, if any */
	if (ca_crt) {
		gnutls_pkcs12_bag_t bag;

		result = gnutls_pkcs12_bag_init(&bag);
		if (result < 0) {
			fprintf(stderr, "bag_init: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_bag_set_crt(bag, ca_crt);
		if (result < 0) {
			fprintf(stderr, "set_crt[%d]: %s\n", i,
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_bag_encrypt(bag, pass, flags);
		if (result < 0) {
			fprintf(stderr, "bag_encrypt: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_set_bag(pkcs12, bag);
		if (result < 0) {
			fprintf(stderr, "set_bag: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
		gnutls_pkcs12_bag_deinit(bag);
	}

	for (i = 0; i < nkeys; i++) {
		gnutls_pkcs12_bag_t kbag;

		result = gnutls_pkcs12_bag_init(&kbag);
		if (result < 0) {
			fprintf(stderr, "bag_init: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		size = lbuffer_size;
		result =
		    gnutls_x509_privkey_export_pkcs8(keys[i],
						     GNUTLS_X509_FMT_DER,
						     pass, flags, lbuffer,
						     &size);
		if (result < 0) {
			fprintf(stderr, "key_export[%d]: %s\n", i,
				gnutls_strerror(result));
			exit(1);
		}

		data.data = lbuffer;
		data.size = size;
		result =
		    gnutls_pkcs12_bag_set_data(kbag,
					       GNUTLS_BAG_PKCS8_ENCRYPTED_KEY,
					       &data);
		if (result < 0) {
			fprintf(stderr, "bag_set_data: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		indx = result;

		result =
		    gnutls_pkcs12_bag_set_friendly_name(kbag, indx, name);
		if (result < 0) {
			fprintf(stderr, "bag_set_friendly_name: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		size = sizeof(_key_id);
		result =
		    gnutls_x509_privkey_get_key_id(keys[i], GNUTLS_KEYID_USE_SHA1, _key_id,
						   &size);
		if (result < 0) {
			fprintf(stderr, "key_id[%d]: %s\n", i,
				gnutls_strerror(result));
			exit(1);
		}

		key_id.data = _key_id;
		key_id.size = size;

		result = gnutls_pkcs12_bag_set_key_id(kbag, indx, &key_id);
		if (result < 0) {
			fprintf(stderr, "bag_set_key_id: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_set_bag(pkcs12, kbag);
		if (result < 0) {
			fprintf(stderr, "set_bag: %s\n",
				gnutls_strerror(result));
			exit(1);
		}
		gnutls_pkcs12_bag_deinit(kbag);
	}

	result = gnutls_pkcs12_generate_mac(pkcs12, pass);
	if (result < 0) {
		fprintf(stderr, "generate_mac: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	size = lbuffer_size;
	result =
	    gnutls_pkcs12_export(pkcs12, outcert_format, lbuffer, &size);
	if (result < 0) {
		fprintf(stderr, "pkcs12_export: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	fwrite(lbuffer, 1, size, outfile);
	for (i=0;i<ncrts;i++)
		gnutls_x509_crt_deinit(crts[i]);
	gnutls_free(crts);
	gnutls_x509_crt_deinit(ca_crt);
	gnutls_pkcs12_deinit(pkcs12);
}

static const char *BAGTYPE(gnutls_pkcs12_bag_type_t x)
{
	switch (x) {
	case GNUTLS_BAG_PKCS8_ENCRYPTED_KEY:
		return "PKCS #8 Encrypted key";
	case GNUTLS_BAG_EMPTY:
		return "Empty";
	case GNUTLS_BAG_PKCS8_KEY:
		return "PKCS #8 Key";
	case GNUTLS_BAG_CERTIFICATE:
		return "Certificate";
	case GNUTLS_BAG_ENCRYPTED:
		return "Encrypted";
	case GNUTLS_BAG_CRL:
		return "CRL";
	case GNUTLS_BAG_SECRET:
		return "Secret";
	default:
		return "Unknown";
	}
}

static void print_bag_data(gnutls_pkcs12_bag_t bag)
{
	int result;
	int count, i, type;
	gnutls_datum_t cdata, id;
	const char *str, *name;
	gnutls_datum_t out;

	count = gnutls_pkcs12_bag_get_count(bag);
	if (count < 0) {
		fprintf(stderr, "get_count: %s\n", gnutls_strerror(count));
		exit(1);
	}

	fprintf(outfile, "\tElements: %d\n", count);

	for (i = 0; i < count; i++) {
		type = gnutls_pkcs12_bag_get_type(bag, i);
		if (type < 0) {
			fprintf(stderr, "get_type: %s\n",
				gnutls_strerror(type));
			exit(1);
		}

		fprintf(stderr, "\tType: %s\n", BAGTYPE(type));

		result = gnutls_pkcs12_bag_get_data(bag, i, &cdata);
		if (result < 0) {
			fprintf(stderr, "get_data: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		if (type == GNUTLS_BAG_PKCS8_ENCRYPTED_KEY) {
			pkcs8_info_int(&cdata, GNUTLS_X509_FMT_DER, 1, outfile, "\t");
		}

		name = NULL;
		result =
		    gnutls_pkcs12_bag_get_friendly_name(bag, i,
							(char **) &name);
		if (result < 0) {
			fprintf(stderr, "get_friendly_name: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		if (name)
			fprintf(outfile, "\tFriendly name: %s\n", name);

		id.data = NULL;
		id.size = 0;
		result = gnutls_pkcs12_bag_get_key_id(bag, i, &id);
		if (result < 0) {
			fprintf(stderr, "get_key_id: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		if (id.size > 0)
			fprintf(outfile, "\tKey ID: %s\n",
				raw_to_string(id.data, id.size));


		switch (type) {
		case GNUTLS_BAG_PKCS8_ENCRYPTED_KEY:
			str = "ENCRYPTED PRIVATE KEY";
			break;
		case GNUTLS_BAG_PKCS8_KEY:
			str = "PRIVATE KEY";
			break;
		case GNUTLS_BAG_CERTIFICATE:
			str = "CERTIFICATE";
			break;
		case GNUTLS_BAG_CRL:
			str = "CRL";
			break;
		case GNUTLS_BAG_ENCRYPTED:
		case GNUTLS_BAG_EMPTY:
		default:
			str = NULL;
		}

		if (str != NULL) {
			gnutls_pem_base64_encode_alloc(str, &cdata, &out);
			fprintf(outfile, "%s\n", out.data);

			gnutls_free(out.data);
		}

	}
}

static
void pkcs12_bag_enc_info(gnutls_pkcs12_bag_t bag, FILE *out)
{
	int ret;
	unsigned schema;
	unsigned cipher;
	unsigned char salt[32];
	char hex[64+1];
	unsigned salt_size = sizeof(salt);
	unsigned iter_count;
	gnutls_datum_t bin;
	size_t hex_size = sizeof(hex);
	const char *str;
	char *oid = NULL;

	ret = gnutls_pkcs12_bag_enc_info(bag, 
		&schema, &cipher, salt, &salt_size, &iter_count, &oid);
	if (ret == GNUTLS_E_UNKNOWN_CIPHER_TYPE) {
		fprintf(out, "\tSchema: unsupported (%s)\n", oid);
		gnutls_free(oid);
		return;
	}

	if (ret < 0) {
		fprintf(stderr, "PKCS #12 bag read error: %s\n",
			gnutls_strerror(ret));
		return;
	}
	gnutls_free(oid);

	fprintf(out, "\tCipher: %s\n", gnutls_cipher_get_name(cipher));

	str = gnutls_pkcs_schema_get_name(schema);
	if (str != NULL) {
		fprintf(out, "\tSchema: %s (%s)\n", str, gnutls_pkcs_schema_get_oid(schema));
	}

	bin.data = salt;
	bin.size = salt_size;
	ret = gnutls_hex_encode(&bin, hex, &hex_size);
	if (ret < 0) {
		fprintf(stderr, "hex encode error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(out, "\tSalt: %s\n", hex);
	fprintf(out, "\tSalt size: %u\n", salt_size);
	fprintf(out, "\tIteration count: %u\n", iter_count);
}

void pkcs12_info(common_info_st * cinfo)
{
	gnutls_pkcs12_t pkcs12;
	gnutls_pkcs12_bag_t bag;
	gnutls_mac_algorithm_t mac_algo;
	char *mac_oid = NULL;
	char hex[64+1];
	size_t hex_size = sizeof(hex);
	char salt[32];
	unsigned int salt_size;
	unsigned int mac_iter;
	int result;
	size_t size;
	gnutls_datum_t data;
	const char *pass;
	int indx, fail = 0;

	result = gnutls_pkcs12_init(&pkcs12);
	if (result < 0) {
		fprintf(stderr, "p12_init: %s\n", gnutls_strerror(result));
		exit(1);
	}

	data.data = (void *) fread_file(infile, &size);
	data.size = size;

	if (!data.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	result = gnutls_pkcs12_import(pkcs12, &data, incert_format, 0);
	free(data.data);
	if (result < 0) {
		fprintf(stderr, "p12_import: %s\n", gnutls_strerror(result));
		exit(1);
	}

	salt_size = sizeof(salt);
	result = gnutls_pkcs12_mac_info(pkcs12, &mac_algo, salt, &salt_size, &mac_iter, &mac_oid);
	if (result == GNUTLS_E_UNKNOWN_HASH_ALGORITHM) {
		fprintf(outfile, "MAC info:\n");
		if (mac_oid != NULL)
			fprintf(outfile, "\tMAC: unknown (%s)\n", mac_oid);
	} else if (result >= 0) {
		gnutls_datum_t bin;

		fprintf(outfile, "MAC info:\n");
		fprintf(outfile, "\tMAC: %s (%s)\n", gnutls_mac_get_name(mac_algo), mac_oid);

		bin.data = (void*)salt;
		bin.size = salt_size;
		result = gnutls_hex_encode(&bin, hex, &hex_size);
		if (result < 0) {
			fprintf(stderr, "hex encode error: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		fprintf(outfile, "\tSalt: %s\n", hex);
		fprintf(outfile, "\tSalt size: %u\n", salt_size);
		fprintf(outfile, "\tIteration count: %u\n\n", mac_iter);
	}
	gnutls_free(mac_oid);

	pass = get_password(cinfo, NULL, 0);

	result = gnutls_pkcs12_verify_mac(pkcs12, pass);
	if (result < 0) {
		fail = 1;
		fprintf(stderr, "verify_mac: %s\n", gnutls_strerror(result));
	}

	for (indx = 0;; indx++) {
		result = gnutls_pkcs12_bag_init(&bag);
		if (result < 0) {
			fprintf(stderr, "bag_init: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		result = gnutls_pkcs12_get_bag(pkcs12, indx, bag);
		if (result < 0) {
			gnutls_pkcs12_bag_deinit(bag);
			break;
		}

		result = gnutls_pkcs12_bag_get_count(bag);
		if (result < 0) {
			fprintf(stderr, "bag_count: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		fprintf(outfile, "BAG #%d\n", indx);

		result = gnutls_pkcs12_bag_get_type(bag, 0);
		if (result < 0) {
			fprintf(stderr, "bag_init: %s\n",
				gnutls_strerror(result));
			exit(1);
		}

		if (result == GNUTLS_BAG_ENCRYPTED) {
			fprintf(stderr, "\tType: %s\n", BAGTYPE(result));
			pkcs12_bag_enc_info(bag, stderr);
			fprintf(stderr, "\n\tDecrypting...\n");

			result = gnutls_pkcs12_bag_decrypt(bag, pass);

			if (result < 0) {
				fail = 1;
				fprintf(stderr, "bag_decrypt: %s\n",
					gnutls_strerror(result));
				continue;
			}

			result = gnutls_pkcs12_bag_get_count(bag);
			if (result < 0) {
				fprintf(stderr, "encrypted bag_count: %s\n",
					gnutls_strerror(result));
				exit(1);
			}
		}

		print_bag_data(bag);

		gnutls_pkcs12_bag_deinit(bag);
	}

	gnutls_pkcs12_deinit(pkcs12);

	if (fail) {
		fprintf(stderr,
			"There were errors parsing the structure\n");
		exit(1);
	}
}

void pkcs8_info_int(gnutls_datum_t *data, unsigned format, 
		    unsigned ignore_err, FILE *out, const char *tab)
{
	int ret;
	unsigned schema;
	unsigned cipher;
	unsigned char salt[32];
	char hex[64+1];
	unsigned salt_size = sizeof(salt);
	unsigned iter_count;
	gnutls_datum_t bin;
	size_t hex_size = sizeof(hex);
	const char *str;
	char *oid = NULL;

	ret = gnutls_pkcs8_info(data, format,
		&schema, &cipher, salt, &salt_size, &iter_count, &oid);
	if (ret == GNUTLS_E_UNKNOWN_CIPHER_TYPE) {
		fprintf(out, "PKCS #8 information:\n");
		fprintf(out, "\tSchema: unsupported (%s)\n", oid);
		gnutls_free(oid);
		return;
	}

	if (ret < 0) {
		if (ignore_err)
			return;
		fprintf(stderr, "PKCS #8 read error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}
	gnutls_free(oid);

	fprintf(out, "%sPKCS #8 information:\n", tab);
	fprintf(out, "%s\tCipher: %s\n", tab, gnutls_cipher_get_name(cipher));

	str = gnutls_pkcs_schema_get_name(schema);
	if (str != NULL) {
		fprintf(out, "%s\tSchema: %s (%s)\n", tab, str, gnutls_pkcs_schema_get_oid(schema));
	}

	bin.data = salt;
	bin.size = salt_size;
	ret = gnutls_hex_encode(&bin, hex, &hex_size);
	if (ret < 0) {
		fprintf(stderr, "hex encode error: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fprintf(out, "%s\tSalt: %s\n", tab, hex);
	fprintf(out, "%s\tSalt size: %u\n", tab, salt_size);
	fprintf(out, "%s\tIteration count: %u\n\n", tab, iter_count);
}

void pkcs8_info(void)
{
	size_t size;
	gnutls_datum_t data;

	data.data = (void *) fread_file(infile, &size);
	data.size = size;

	if (!data.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	pkcs8_info_int(&data, incert_format, 0, outfile, "");
	free(data.data);
}

void pkcs7_info(common_info_st *cinfo)
{
	gnutls_pkcs7_t pkcs7;
	int result;
	size_t size;
	gnutls_datum_t data, str;

	result = gnutls_pkcs7_init(&pkcs7);
	if (result < 0) {
		fprintf(stderr, "p7_init: %s\n", gnutls_strerror(result));
		exit(1);
	}

	data.data = (void *) fread_file(infile, &size);
	data.size = size;

	if (!data.data) {
		fprintf(stderr, "%s", infile ? "file" : "standard input");
		exit(1);
	}

	result = gnutls_pkcs7_import(pkcs7, &data, incert_format);
	free(data.data);
	if (result < 0) {
		fprintf(stderr, "import error: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	result = gnutls_pkcs7_print(pkcs7, GNUTLS_CRT_PRINT_FULL, &str);
	if (result < 0) {
		fprintf(stderr, "printing error: %s\n",
			gnutls_strerror(result));
		exit(1);
	}

	fprintf(outfile, "%s", str.data);
	gnutls_free(str.data);

	gnutls_pkcs7_deinit(pkcs7);
}

void smime_to_pkcs7(void)
{
	size_t linesize = 0;
	char *lineptr = NULL;
	ssize_t len;

	/* Find body.  FIXME: Handle non-b64 Content-Transfer-Encoding.
	   Reject non-S/MIME tagged Content-Type's? */
	do {
		len = getline(&lineptr, &linesize, infile);
		if (len == -1) {
			fprintf(stderr,
				"cannot find RFC 2822 header/body separator");
			exit(1);
		}
	}
	while (strcmp(lineptr, "\r\n") != 0 && strcmp(lineptr, "\n") != 0);

	do {
		len = getline(&lineptr, &linesize, infile);
		if (len == -1) {
			fprintf(stderr,
				"message has RFC 2822 header but no body");
			exit(1);
		}
	}
	while (strcmp(lineptr, "\r\n") == 0 && strcmp(lineptr, "\n") == 0);

	fprintf(outfile, "%s", "-----BEGIN PKCS7-----\n");

	do {
		while (len > 0
		       && (lineptr[len - 1] == '\r'
			   || lineptr[len - 1] == '\n'))
			lineptr[--len] = '\0';
		if (strcmp(lineptr, "") != 0)
			fprintf(outfile, "%s\n", lineptr);
		len = getline(&lineptr, &linesize, infile);
	}
	while (len != -1);

	fprintf(outfile, "%s", "-----END PKCS7-----\n");

	free(lineptr);
}

/* Tries to find a public key in the provided options or stdin */
static
gnutls_pubkey_t find_pubkey(gnutls_x509_crt_t crt, common_info_st * cinfo)
{
	gnutls_pubkey_t pubkey = NULL;
	gnutls_privkey_t privkey = NULL;
	gnutls_x509_crq_t crq = NULL;
	int ret;
	size_t size;
	gnutls_datum_t pem;

	ret = gnutls_pubkey_init(&pubkey);
	if (ret < 0) {
		fprintf(stderr, "pubkey_init: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	if (crt == NULL) {
		crt = load_cert(0, cinfo);
	}

	if (crq == NULL) {
		crq = load_request(cinfo);
	}

	if (crt != NULL) {
		ret = gnutls_pubkey_import_x509(pubkey, crt, 0);
		if (ret < 0) {
			fprintf(stderr, "pubkey_import_x509: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
		gnutls_x509_crt_deinit(crt);
	} else if (crq != NULL) {
		ret = gnutls_pubkey_import_x509_crq(pubkey, crq, 0);
		if (ret < 0) {
			fprintf(stderr, "pubkey_import_x509_crq: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}
		gnutls_x509_crq_deinit(crq);
	} else {
		privkey = load_private_key(0, cinfo);

		if (privkey != NULL) {
			ret =
			    gnutls_pubkey_import_privkey(pubkey, privkey,
							 0, 0);
			if (ret < 0) {
				fprintf(stderr,
					"pubkey_import_privkey: %s\n",
					gnutls_strerror(ret));
				exit(1);
			}
			gnutls_privkey_deinit(privkey);
		} else {
			gnutls_pubkey_deinit(pubkey);
			pubkey = load_pubkey(0, cinfo);

			if (pubkey == NULL) { /* load from stdin */
				pem.data = (void *) fread_file(infile, &size);
				pem.size = size;

				if (!pem.data) {
					fprintf(stderr, "%s", infile ? "file" : "standard input");
					exit(1);
				}

				ret = gnutls_pubkey_init(&pubkey);
				if (ret < 0) {
					fprintf(stderr,
						"pubkey_init: %s\n",
						gnutls_strerror(ret));
					exit(1);
				}

				if (memmem(pem.data, pem.size, "BEGIN CERTIFICATE", 16) != 0 ||
				    memmem(pem.data, pem.size, "BEGIN X509", 10) != 0) {
					ret = gnutls_x509_crt_init(&crt);
					if (ret < 0) {
						fprintf(stderr,
							"crt_init: %s\n",
							gnutls_strerror(ret));
						exit(1);
					}

					ret = gnutls_x509_crt_import(crt, &pem, GNUTLS_X509_FMT_PEM);
					if (ret < 0) {
						fprintf(stderr,
							"crt_import: %s\n",
							gnutls_strerror(ret));
						exit(1);
					}

					ret = gnutls_pubkey_import_x509(pubkey, crt, 0);
					if (ret < 0) {
						fprintf(stderr, "pubkey_import_x509: %s\n",
						gnutls_strerror(ret));
						exit(1);
					}
					gnutls_x509_crt_deinit(crt);
				} else {
					ret = gnutls_pubkey_import(pubkey, &pem, incert_format);
					if (ret < 0) {
						fprintf(stderr,
							"pubkey_import: %s\n",
							gnutls_strerror(ret));
						exit(1);
					}
				}
				free(pem.data);
			}

		}
	}

	return pubkey;
}

void pubkey_info(gnutls_x509_crt_t crt, common_info_st * cinfo)
{
	gnutls_pubkey_t pubkey;
	int ret;
	size_t size;

	pubkey = find_pubkey(crt, cinfo);
	if (pubkey == 0) {
		fprintf(stderr, "find public key error\n");
		exit(1);
	}

	if (outcert_format == GNUTLS_X509_FMT_DER) {
		size = lbuffer_size;
		ret =
		    gnutls_pubkey_export(pubkey, outcert_format, lbuffer,
					 &size);
		if (ret < 0) {
			fprintf(stderr, "export error: %s\n",
				gnutls_strerror(ret));
			exit(1);
		}

		fwrite(lbuffer, 1, size, outfile);

		gnutls_pubkey_deinit(pubkey);

		return;
	}

	/* PEM */

	_pubkey_info(outfile, full_format, pubkey);
	gnutls_pubkey_deinit(pubkey);
}

static
void pubkey_keyid(common_info_st * cinfo)
{
	gnutls_pubkey_t pubkey;
	uint8_t fpr[64];
	char txt[256];
	int ret;
	size_t size, fpr_size;
	gnutls_datum_t tmp;
	unsigned flags;

	pubkey = find_pubkey(NULL, cinfo);
	if (pubkey == 0) {
		fprintf(stderr, "find public key error\n");
		exit(1);
	}

	if (default_dig == GNUTLS_DIG_SHA1 || default_dig == GNUTLS_DIG_UNKNOWN)
		flags = GNUTLS_KEYID_USE_SHA1; /* be backwards compatible */
	else if (default_dig == GNUTLS_DIG_SHA256)
		flags = GNUTLS_KEYID_USE_SHA256;
	else {
		fprintf(stderr, "Cannot calculate key ID with the provided hash\n");
		exit(1);
	}

	fpr_size = sizeof(fpr);
	ret = gnutls_pubkey_get_key_id(pubkey, flags, fpr, &fpr_size);
	if (ret < 0) {
		fprintf(stderr,
			"get_key_id: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	tmp.data = fpr;
	tmp.size = fpr_size;

	size = sizeof(txt);
	ret = gnutls_hex_encode(&tmp, txt, &size);
	if (ret < 0) {
		fprintf(stderr,
			"hex_encode: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fputs(txt, outfile);
	fputs("\n", outfile);

	gnutls_pubkey_deinit(pubkey);
	return;
}

static
void certificate_fpr(common_info_st * cinfo)
{
	gnutls_x509_crt_t crt;
	size_t size;
	int ret = 0;
	gnutls_datum_t pem, tmp;
	unsigned int crt_num;
	uint8_t fpr[32];
	char txt[128];
	size_t fpr_size;

	crt = load_cert(0, cinfo);

	if (crt == NULL) {
		pem.data = (void *) fread_file(infile, &size);
		pem.size = size;

		if (!pem.data) {
			fprintf(stderr, "%s", infile ? "file" : "standard input");
			exit(1);
		}

		crt_num = 1;
		ret =
		    gnutls_x509_crt_list_import(&crt, &crt_num, &pem, incert_format,
						GNUTLS_X509_CRT_LIST_IMPORT_FAIL_IF_EXCEED);
		if (ret == GNUTLS_E_SHORT_MEMORY_BUFFER) {
			fprintf(stderr, "too many certificates (%d).",
				crt_num);
		} else if (ret >= 0 && crt_num == 0) {
			fprintf(stderr, "no certificates were found.\n");
		}

		free(pem.data);
	}

	if (ret < 0) {
		fprintf(stderr, "import error: %s\n", gnutls_strerror(ret));
		exit(1);
	}

	fpr_size = sizeof(fpr);

	if (default_dig == GNUTLS_DIG_UNKNOWN)
		default_dig = GNUTLS_DIG_SHA1;

	ret = gnutls_x509_crt_get_fingerprint(crt, default_dig, fpr, &fpr_size);
	if (ret < 0) {
		fprintf(stderr,
			"get_key_id: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	tmp.data = fpr;
	tmp.size = fpr_size;

	size = sizeof(txt);
	ret = gnutls_hex_encode(&tmp, txt, &size);
	if (ret < 0) {
		fprintf(stderr,
			"hex_encode: %s\n",
			gnutls_strerror(ret));
		exit(1);
	}

	fputs(txt, outfile);
	fputs("\n", outfile);

	gnutls_x509_crt_deinit(crt);
	return;
}
