/*
 *  Copyright (C) 2003 Nikos Mavroyanopoulos
 *
 *  This file is part of GNUTLS.
 *
 *  The GNUTLS library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public   
 *  License as published by the Free Software Foundation; either 
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of 
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */

/* Functions that relate on PKCS12 packet parsing.
 */

#include <libtasn1.h>
#include <gnutls_int.h>

#ifdef ENABLE_PKI

#include <gnutls_datum.h>
#include <gnutls_global.h>
#include <gnutls_errors.h>
#include <gnutls_num.h>
#include <gnutls_random.h>
#include <common.h>
#include <x509_b64.h>
#include <pkcs12.h>
#include <dn.h>

#define DATA_OID "1.2.840.113549.1.7.1"
#define ENC_DATA_OID "1.2.840.113549.1.7.6"

/* Decodes the PKCS #12 auth_safe, and returns the allocated raw data,
 * which holds them. Returns an ASN1_TYPE of authenticatedSafe.
 */
static
int _decode_pkcs12_auth_safe( ASN1_TYPE pkcs12, ASN1_TYPE * authen_safe, gnutls_datum* raw) 
{
char oid[128];
ASN1_TYPE c2 = ASN1_TYPE_EMPTY;
gnutls_datum auth_safe = { NULL, 0 };
int tmp_size, len, result;

	len = sizeof(oid) - 1;
	result = asn1_read_value(pkcs12, "authSafe.contentType", oid, &len);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		return _gnutls_asn2err(result);
	}

	if ( strcmp( oid, DATA_OID) != 0) {
		gnutls_assert();
		_gnutls_x509_log( "Unknown PKCS12 Content OID '%s'\n", oid);
		return GNUTLS_E_UNKNOWN_PKCS_CONTENT_TYPE;
	}

	/* Step 1. Read the content data
	 */

	tmp_size = 0;
	result = _gnutls_x509_read_value(pkcs12, "authSafe.content", &auth_safe, 1);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}

	/* Step 2. Extract the authenticatedSafe.
	 */

	if ((result=asn1_create_element
	    (_gnutls_get_pkix(), "PKIX1.pkcs-12-AuthenticatedSafe", &c2)) != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	result = asn1_der_decoding(&c2, auth_safe.data, auth_safe.size, NULL);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	if (raw == NULL) {
		_gnutls_free_datum(&auth_safe);
	} else {
		raw->data = auth_safe.data;
		raw->size = auth_safe.size;
	}

	if (authen_safe)
		*authen_safe = c2;

	return 0;

	cleanup:
		if (c2) asn1_delete_structure(&c2);
		_gnutls_free_datum( &auth_safe);
		return result;
}

/**
  * gnutls_pkcs12_init - This function initializes a gnutls_pkcs12 structure
  * @pkcs12: The structure to be initialized
  *
  * This function will initialize a PKCS12 structure. PKCS12 structures
  * usually contain lists of X.509 Certificates and X.509 Certificate
  * revocation lists.
  *
  * Returns 0 on success.
  *
  **/
int gnutls_pkcs12_init(gnutls_pkcs12 * pkcs12)
{
	*pkcs12 = gnutls_calloc( 1, sizeof(gnutls_pkcs12_int));

	if (*pkcs12) {
		int result = asn1_create_element(_gnutls_get_pkix(),
				     "PKIX1.pkcs-12-PFX",
				     &(*pkcs12)->pkcs12);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			return _gnutls_asn2err(result);
		}
		return 0;		/* success */
	}
	return GNUTLS_E_MEMORY_ERROR;
}

/**
  * gnutls_pkcs12_deinit - This function deinitializes memory used by a gnutls_pkcs12 structure
  * @pkcs12: The structure to be initialized
  *
  * This function will deinitialize a PKCS12 structure. 
  *
  **/
void gnutls_pkcs12_deinit(gnutls_pkcs12 pkcs12)
{
	if (pkcs12->pkcs12)
		asn1_delete_structure(&pkcs12->pkcs12);

	gnutls_free(pkcs12);
}

/**
  * gnutls_pkcs12_import - This function will import a DER or PEM encoded PKCS12 structure
  * @pkcs12: The structure to store the parsed PKCS12.
  * @data: The DER or PEM encoded PKCS12.
  * @format: One of DER or PEM
  * @flags: an ORed sequence of gnutls_privkey_pkcs8_flags
  *
  * This function will convert the given DER or PEM encoded PKCS12
  * to the native gnutls_pkcs12 format. The output will be stored in 'pkcs12'.
  *
  * If the PKCS12 is PEM encoded it should have a header of "PKCS12".
  *
  * Returns 0 on success.
  *
  **/
int gnutls_pkcs12_import(gnutls_pkcs12 pkcs12, const gnutls_datum * data,
	gnutls_x509_crt_fmt format, unsigned int flags)
{
	int result = 0, need_free = 0;
	gnutls_datum _data = { data->data, data->size };

	/* If the PKCS12 is in PEM format then decode it
	 */
	if (format == GNUTLS_X509_FMT_PEM) {
		opaque *out;
		
		result = _gnutls_fbase64_decode(PEM_PKCS12, data->data, data->size,
			&out);

		if (result <= 0) {
			if (result==0) result = GNUTLS_E_INTERNAL_ERROR;
			gnutls_assert();
			return result;
		}
		
		_data.data = out;
		_data.size = result;
		
		need_free = 1;
	}

	result = asn1_der_decoding(&pkcs12->pkcs12, _data.data, _data.size, NULL);
	if (result != ASN1_SUCCESS) {
		result = _gnutls_asn2err(result);
		gnutls_assert();
		goto cleanup;
	}

	if (need_free) _gnutls_free_datum( &_data);

	return 0;

      cleanup:
	if (need_free) _gnutls_free_datum( &_data);
	return result;
}


/**
  * gnutls_pkcs12_export - This function will export the pkcs12 structure
  * @pkcs12: Holds the pkcs12 structure
  * @format: the format of output params. One of PEM or DER.
  * @output_data: will contain a structure PEM or DER encoded
  * @output_data_size: holds the size of output_data (and will be replaced by the actual size of parameters)
  *
  * This function will export the pkcs12 structure to DER or PEM format.
  *
  * If the buffer provided is not long enough to hold the output, then
  * GNUTLS_E_SHORT_MEMORY_BUFFER will be returned.
  *
  * If the structure is PEM encoded, it will have a header
  * of "BEGIN PKCS12".
  *
  * In case of failure a negative value will be returned, and
  * 0 on success.
  *
  **/
int gnutls_pkcs12_export( gnutls_pkcs12 pkcs12,
	gnutls_x509_crt_fmt format, unsigned char* output_data, int* output_data_size)
{
	return _gnutls_x509_export_int( pkcs12->pkcs12, format, PEM_PKCS12, *output_data_size,
		output_data, output_data_size);
}

static int _oid2bag( const char* oid)
{
	if (strcmp(oid, BAG_PKCS8_KEY)==0) 
		return GNUTLS_BAG_PKCS8_KEY;
	if (strcmp(oid, BAG_PKCS8_ENCRYPTED_KEY)==0) 
		return GNUTLS_BAG_PKCS8_ENCRYPTED_KEY;
	if (strcmp(oid, BAG_CERTIFICATE)==0) 
		return GNUTLS_BAG_CERTIFICATE;
	if (strcmp(oid, BAG_CRL)==0) 
		return GNUTLS_BAG_CRL;
	
	return GNUTLS_BAG_UNKNOWN;
}

static const char* _bag2oid( int bag)
{
	switch (bag) {
		case GNUTLS_BAG_PKCS8_KEY:
			return BAG_PKCS8_KEY;
		case GNUTLS_BAG_PKCS8_ENCRYPTED_KEY:
			return BAG_PKCS8_ENCRYPTED_KEY;
		case GNUTLS_BAG_CERTIFICATE:
			return BAG_CERTIFICATE;
		case GNUTLS_BAG_CRL:
			return BAG_CRL;
	}
	return NULL;
}

/* Decodes the SafeContents, and puts the output in
 * the given bag. 
 */
int
_pkcs12_decode_safe_contents( const gnutls_datum* content, gnutls_pkcs12_bag bag)
{
char oid[128], root[128];
ASN1_TYPE c2 = ASN1_TYPE_EMPTY;
int len, result;
int bag_type;
int count = 0, i;
char counter[MAX_INT_DIGITS];

	/* Step 1. Extract the SEQUENCE.
	 */

	if ((result=asn1_create_element
	    (_gnutls_get_pkix(), "PKIX1.pkcs-12-SafeContents", &c2)) != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	result = asn1_der_decoding(&c2, content->data, content->size, NULL);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	/* Count the number of bags
	 */
	result = asn1_number_of_elements( c2, "", &count);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	bag->bag_elements = GMIN(MAX_BAG_ELEMENTS, count);

	for (i=0;i<bag->bag_elements;i++) {

		_gnutls_str_cpy( root, sizeof(root), "?"); 
		_gnutls_int2str( i+1, counter);
		_gnutls_str_cat( root, sizeof(root), counter); 
		_gnutls_str_cat( root, sizeof(root), ".bagId"); 

		len = sizeof(oid);
		result = asn1_read_value(c2, root, oid, &len);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			result = _gnutls_asn2err(result);
			goto cleanup;
		}

		/* Read the Bag type
		 */
		bag_type = _oid2bag( oid);
	
		if (bag_type < 0) {
			gnutls_assert();
			goto cleanup;
		}

		/* Read the Bag Value
		 */

		_gnutls_str_cpy( root, sizeof(root), "?"); 
		_gnutls_int2str( i+1, counter);
		_gnutls_str_cat( root, sizeof(root), counter); 
		_gnutls_str_cat( root, sizeof(root), ".bagValue"); 

		result = _gnutls_x509_read_value( c2, root, &bag->data[i], 0);
		if (result < 0) {
			gnutls_assert();
			goto cleanup;
		}

		bag->type[i] = bag_type;
		
	}

	asn1_delete_structure(&c2);


	return 0;

	cleanup:
		if (c2) asn1_delete_structure(&c2);
		return result;

}


static
int _parse_safe_contents( ASN1_TYPE sc, const char* sc_name, gnutls_pkcs12_bag bag) 
{
gnutls_datum content = { NULL, 0 };
int result;

	/* Step 1. Extract the content.
	 */

	result = _gnutls_x509_read_value(sc, sc_name, &content, 1);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}

	result = _pkcs12_decode_safe_contents( &content, bag);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}

	_gnutls_free_datum( &content);

	return 0;
	
	cleanup:
		_gnutls_free_datum( &content);
		return result;
}


/**
  * gnutls_pkcs12_get_bag - This function returns a Bag from a PKCS12 structure
  * @pkcs12_struct: should contain a gnutls_pkcs12 structure
  * @indx: contains the index of the bag to extract
  * @bag: An initialized bag, where the contents of the bag will be copied
  *
  * This function will return a Bag from the PKCS12 structure.
  * Returns 0 on success.
  *
  * After the last Bag has been read GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE
  * will be returned.
  *
  **/
int gnutls_pkcs12_get_bag(gnutls_pkcs12 pkcs12, 
	int indx, gnutls_pkcs12_bag bag)
{
	ASN1_TYPE c2 = ASN1_TYPE_EMPTY;
	int result, len;
	char root2[64];
	char oid[128];
	char counter[MAX_INT_DIGITS];
	gnutls_datum tmp = {NULL, 0};

	/* Step 1. decode the data.
	 */
	result = _decode_pkcs12_auth_safe( pkcs12->pkcs12, &c2, NULL);
	if (result < 0) {
		gnutls_assert();
		return result;
	}
	
	/* Step 2. Parse the AuthenticatedSafe
	 */
	
	_gnutls_str_cpy( root2, sizeof(root2), "?"); 
	_gnutls_int2str( indx+1, counter);
	_gnutls_str_cat( root2, sizeof(root2), counter); 
	_gnutls_str_cat( root2, sizeof(root2), ".contentType"); 

	len = sizeof(oid) - 1;

	result = asn1_read_value(c2, root2, oid, &len);

	if (result == ASN1_ELEMENT_NOT_FOUND) {
		result = GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE;
		goto cleanup;	
	}

	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	/* Not encrypted Bag
	 */

	_gnutls_str_cpy( root2, sizeof(root2), "?"); 
	_gnutls_int2str( indx+1, counter);
	_gnutls_str_cat( root2, sizeof(root2), counter); 
	_gnutls_str_cat( root2, sizeof(root2), ".content"); 

	if (strcmp( oid, DATA_OID) == 0) {
		result = _parse_safe_contents( c2, root2, bag);
		goto cleanup;
	}
	
	/* ENC_DATA_OID needs decryption */

	bag->type[0] = GNUTLS_BAG_ENCRYPTED;
	bag->bag_elements = 1;

	result = _gnutls_x509_read_value( c2, root2, &bag->data[0], 0);
	if (result < 0) {
		gnutls_assert();
		return result;
	}

	return 0;

	cleanup:
		_gnutls_free_datum( &tmp);
		if (c2) asn1_delete_structure(&c2);
		return result;
}

/* Creates an empty PFX structure for the PKCS12 structure.
 */
static int create_empty_pfx(ASN1_TYPE pkcs12)
{
	uint8 three = 3;
	int result;
	ASN1_TYPE c2 = ASN1_TYPE_EMPTY;

	/* Use version 3
	 */
	result = asn1_write_value( pkcs12, "version", &three, 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	/* Write the content type of the data
	 */
	result = asn1_write_value(pkcs12, "authSafe.contentType", DATA_OID, 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	/* Check if the authenticatedSafe content is empty, and encode a
	 * null one in that case.
	 */

	if ((result=asn1_create_element
	    (_gnutls_get_pkix(), "PKIX1.pkcs-12-AuthenticatedSafe", &c2)) != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}
	
	result = _gnutls_x509_der_encode_and_copy( c2, "", pkcs12, "authSafe.content", 1);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}
	asn1_delete_structure( &c2);

	return 0;

	cleanup:
		asn1_delete_structure( &c2);
		return result;	

}

static int
_pkcs12_encode_safe_contents( gnutls_pkcs12_bag bag, ASN1_TYPE* content, int *enc);

/**
  * gnutls_pkcs12_set_bag - This function inserts a Bag into a PKCS12 structure
  * @pkcs12_struct: should contain a gnutls_pkcs12 structure
  * @bag: An initialized bag
  *
  * This function will insert a Bag into the PKCS12 structure.
  * Returns 0 on success.
  *
  **/
int gnutls_pkcs12_set_bag(gnutls_pkcs12 pkcs12, gnutls_pkcs12_bag bag)
{
	ASN1_TYPE c2 = ASN1_TYPE_EMPTY;
	ASN1_TYPE safe_cont = ASN1_TYPE_EMPTY;
	int result;
	int enc = 0, dum = 1;
	char null;

	/* Step 1. Check if the pkcs12 structure is empty. In that
	 * case generate an empty PFX.
	 */
	result = asn1_read_value(pkcs12->pkcs12, "authSafe.content", &null, &dum);
	if (result == ASN1_VALUE_NOT_FOUND) {
		result = create_empty_pfx( pkcs12->pkcs12);
		if (result < 0) {
			gnutls_assert();
			return result;
		}
	}

	/* Step 2. decode the authenticatedSafe.
	 */
	result = _decode_pkcs12_auth_safe( pkcs12->pkcs12, &c2, NULL);
	if (result < 0) {
		gnutls_assert();
		return result;
	}

	/* Step 3. Encode the bag elements into a SafeContents 
	 * structure.
	 */
	result = _pkcs12_encode_safe_contents( bag, &safe_cont, &enc);
	if (result < 0) {
		gnutls_assert();
		return result;
	}

	/* Step 4. Insert the encoded SafeContents into the AuthenticatedSafe
	 * structure.
	 */
	result = asn1_write_value(c2, "", "NEW", 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	if (enc)
		result = asn1_write_value(c2, "?LAST.contentType", ENC_DATA_OID, 1);
	else
		result = asn1_write_value(c2, "?LAST.contentType", DATA_OID, 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	result = _gnutls_x509_der_encode_and_copy( safe_cont, "", c2, "?LAST.content", 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	asn1_delete_structure(&safe_cont);

	
	/* Step 5. Reencode and copy the AuthenticatedSafe into the pkcs12
	 * structure.
	 */
	result = _gnutls_x509_der_encode_and_copy( c2, "", pkcs12->pkcs12, "authSafe.content", 1);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}

	asn1_delete_structure(&c2);

	return 0;

	cleanup:
		asn1_delete_structure(&c2);
		asn1_delete_structure(&safe_cont);
		return result;
}

/**
  * gnutls_pkcs12_generate_mac - This function generates the MAC of the PKCS12 structure
  * @pkcs12_struct: should contain a gnutls_pkcs12 structure
  * @pass: The password for the MAC
  *
  * This function will generate a MAC for the PKCS12 structure.
  * Returns 0 on success.
  *
  **/
int gnutls_pkcs12_generate_mac(gnutls_pkcs12 pkcs12, const char* pass)
{
	opaque salt[8], key[20];
	int result;
	const int iter = 1;
	GNUTLS_MAC_HANDLE td1 = NULL;
	gnutls_datum tmp = {NULL, 0};
	opaque sha_mac[20];

	/* Generate the salt.
	 */
	_gnutls_get_random(salt, sizeof(salt), GNUTLS_WEAK_RANDOM);

	/* Write the salt into the structure.
	 */
	result = asn1_write_value(pkcs12->pkcs12, "macData.macSalt", salt, sizeof(salt));
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	/* write the iterations
	 */
	
	if (iter > 1) {
		result = _gnutls_x509_write_uint32( pkcs12->pkcs12, "macData.iterations", iter);
		if (result < 0) {
			gnutls_assert();
			goto cleanup;
		}
	}
	
	/* Generate the key.
	 */
	result = _pkcs12_string_to_key( 3/*MAC*/, salt, sizeof(salt),
        	iter, pass, sizeof(key), key);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}
                                
	/* Get the data to be MACed
	 */
	result = _decode_pkcs12_auth_safe( pkcs12->pkcs12, NULL, &tmp);
	if (result < 0) {
		gnutls_assert();
		goto cleanup;
	}

	/* MAC the data
	 */
	td1 = _gnutls_hmac_init(GNUTLS_MAC_SHA, key, sizeof(key));
	if (td1 == GNUTLS_MAC_FAILED) {
		gnutls_assert();
		result = GNUTLS_E_INTERNAL_ERROR;
		goto cleanup;
	}	 

	_gnutls_hmac(td1, tmp.data, tmp.size);
	_gnutls_free_datum( &tmp);
	
	_gnutls_hmac_deinit(td1, sha_mac);
	

	result = asn1_write_value(pkcs12->pkcs12, "macData.mac.digest", sha_mac, sizeof(sha_mac));
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	result = asn1_write_value(pkcs12->pkcs12, "macData.mac.digestAlgorithm.parameters", NULL, 0);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	result = asn1_write_value(pkcs12->pkcs12, "macData.mac.digestAlgorithm.algorithm", OID_SHA1, 1);
	if (result != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;
	}

	return 0;

	cleanup:
		_gnutls_free_datum( &tmp);
		return result;
}

/* Encodes the bag into a SafeContents structure, and puts the output in
 * the given datum. Enc is set to non zero if the data are encrypted;
 */
static int
_pkcs12_encode_safe_contents( gnutls_pkcs12_bag bag, ASN1_TYPE* contents, int *enc)
{
ASN1_TYPE c2 = ASN1_TYPE_EMPTY;
int result;
int i;
const char* oid;

	if (bag->bag_elements > 1) {
		/* A bag with a key or an encrypted bag, must have
		 * only one element.
		 */
	
		if (bag->type[0] == GNUTLS_BAG_PKCS8_KEY ||
			bag->type[0] == GNUTLS_BAG_PKCS8_ENCRYPTED_KEY ||
			bag->type[0] == GNUTLS_BAG_ENCRYPTED) {
			gnutls_assert();
			return GNUTLS_E_INVALID_REQUEST;
		}
	}

	*enc = 0;

	/* Step 1. Create the SEQUENCE.
	 */

	if ((result=asn1_create_element
	    (_gnutls_get_pkix(), "PKIX1.pkcs-12-SafeContents", &c2)) != ASN1_SUCCESS) {
		gnutls_assert();
		result = _gnutls_asn2err(result);
		goto cleanup;	
	}

	for (i=0;i<bag->bag_elements;i++) {

		if (bag->type[i] == GNUTLS_BAG_ENCRYPTED) *enc = 1;

		oid = _bag2oid( bag->type[i]);
		if (oid==NULL) continue;

		result = asn1_write_value(c2, "", "NEW", 1);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			result = _gnutls_asn2err(result);
			goto cleanup;
		}

		/* Copy the bag type.
		 */
		result = asn1_write_value(c2, "?LAST.bagId", oid, 1);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			result = _gnutls_asn2err(result);
			goto cleanup;
		}

		/* Set empty attributes
		 */
		result = asn1_write_value(c2, "?LAST.bagAttributes", NULL, 0);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			result = _gnutls_asn2err(result);
			goto cleanup;
		}


		/* Copy the Bag Value
		 */

		result = asn1_write_value( c2, "?LAST.bagValue", bag->data[i].data, bag->data[i].size);
		if (result != ASN1_SUCCESS) {
			gnutls_assert();
			result = _gnutls_asn2err(result);
			goto cleanup;
		}

	}
	
	/* Encode the data and copy them into the datum
	 */
	*contents = c2;

	return 0;

	cleanup:
		if (c2) asn1_delete_structure(&c2);
		return result;

}


#endif /* ENABLE_PKI */
