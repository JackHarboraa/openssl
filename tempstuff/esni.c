/*
 * Copyright 2018 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * This is a temporary library and main file to start in on esni
 * in OpenSSL style, as per https://tools.ietf.org/html/draft-ietf-tls-esni-02
 * Author: stephen.farrell@cs.tcd.ie
 * Date: 20181103
 */

#include <stdio.h>
#include <ssl_locl.h>
#include <../ssl/packet_locl.h>
#include <../apps/apps.h>
#include <openssl/kdf.h>

/*
 * For local testing
 */
#define TESTMAIN

/*
 * code within here should be openssl-style
 */
#ifndef OPENSSL_NO_ESNI

/*
 * define'd constants to go in various places
 */ 

/* destintion: include/openssl/tls1.h: */
#define TLSEXT_TYPE_esni_type           0xffce

/* destinatin: include/openssl/ssl.h: */
#define SSL_MAX_SSL_RECORD_DIGEST_LENGTH 255 
#define SSL_MAX_SSL_ENCRYPTED_SNI_LENGTH 255

/*
 * Wrap error handler for now
 */
#ifndef TESTMAIN
/* destination: include/openssl/err.h: */
#define ESNIerr(f,r) ERR_PUT_error(ERR_LIB_CT,(f),(r),OPENSSL_FILE,OPENSSL_LINE)
#else
#define ESNIerr(f,r) fprintf(stderr,"Error in %d,%d, File: %s,Line: %d\n",(f),(r),OPENSSL_FILE,OPENSSL_LINE)
#endif

/* destination: new include/openssl/esni_err.h and/or include/openssl.err.h */

/* 
 * Currently 53 is last one, but lest not be presumptious (yet:-)
 */
#define ERR_LIB_ESNI 									 99

/* 
 * ESNI function codes for ESNIerr
 * These may need to be >100 (or might be convention)
 */
#define ESNI_F_BASE64_DECODE							101
#define ESNI_F_NEW_FROM_BASE64							102
#define ESNI_F_ENC										103
#define ESNI_F_CHECKSUM_CHECK							104

/*
 * ESNI reason codes for ESNIerr
 * These should be >100
 */
#define ESNI_R_BASE64_DECODE_ERROR						110
#define ESNI_R_RR_DECODE_ERROR							111
#define ESNI_R_NOT_IMPL									112


/* 
 * ESNI error strings - inspired by crypto/ct/cterr.c
 */
static const ERR_STRING_DATA ESNI_str_functs[] = {
    {ERR_PACK(ERR_LIB_ESNI, ESNI_F_BASE64_DECODE, 0), "base64 decode"},
    {ERR_PACK(ERR_LIB_ESNI, ESNI_F_NEW_FROM_BASE64, 0), "read from RR"},
    {ERR_PACK(ERR_LIB_ESNI, ESNI_F_ENC, 0), "encrypt SNI details"},
    {0, NULL}
};

static const ERR_STRING_DATA ESNI_str_reasons[] = {
    {ERR_PACK(ERR_LIB_ESNI, 0, ESNI_R_BASE64_DECODE_ERROR), "base64 decode error"},
    {ERR_PACK(ERR_LIB_ESNI, 0, ESNI_R_RR_DECODE_ERROR), "DNS resources record decode error"},
    {ERR_PACK(ERR_LIB_ESNI, 0, ESNI_R_NOT_IMPL), "feature not implemented"},
    {0, NULL}
};

int ERR_load_ESNI_strings(void)
{
#ifndef OPENSSL_NO_ESNI
    if (ERR_func_error_string(ESNI_str_functs[0].error) == NULL) {
        ERR_load_strings_const(ESNI_str_functs);
        ERR_load_strings_const(ESNI_str_reasons);
    }
#endif
    return 1;
}

/*
 * destination: new file include/openssl/esni.h
 * Basic structs for ESNI
 */

/* 
 * From the -02 I-D, what we find in DNS:
 *     struct {
 *         uint16 version;
 *         uint8 checksum[4];
 *         KeyShareEntry keys<4..2^16-1>;
 *         CipherSuite cipher_suites<2..2^16-2>;
 *         uint16 padded_length;
 *         uint64 not_before;
 *         uint64 not_after;
 *         Extension extensions<0..2^16-1>;
 *     } ESNIKeys;
 * 
 * Note that I don't like the above, but it's what we have to
 * work with at the moment.
 */
typedef struct esni_record_st {
	unsigned int version;
	unsigned char checksum[4];
	unsigned int nkeys;
	unsigned int *group_ids;
	EVP_PKEY **keys;
	STACK_OF(SSL_CIPHER) *ciphersuites;
	unsigned int padded_length;
	uint64_t not_before;
	uint64_t not_after;
	unsigned int nexts;
	unsigned int *exttypes;
	void **exts;
	/*
	 * The Encoded (binary, after b64-decode) form of the RR
	 */
	size_t encoded_len;
	unsigned char *encoded;
} ESNI_RECORD;

/*
 * The plaintext form of SNI that we encrypt
 *
 *    struct {
 *        ServerNameList sni;
 *        opaque zeros[ESNIKeys.padded_length - length(sni)];
 *    } PaddedServerNameList;
 *
 *    struct {
 *        uint8 nonce[16];
 *        PaddedServerNameList realSNI;
 *    } ClientESNIInner;
 */
typedef struct client_esni_inner_st {
	size_t nonce_len;
	unsigned char *nonce;
	size_t realSNI_len;
	unsigned char *realSNI;
} CLIENT_ESNI_INNER; 

/* 
 * a struct used in key derivation
 * from the I-D:
 *    struct {
 *        opaque record_digest<0..2^16-1>;
 *        KeyShareEntry esni_key_share;
 *        Random client_hello_random;
 *     } ESNIContents;
 *
 */
typedef struct esni_contents_st {
	size_t rd_len;
	unsigned char *rd;
	size_t kse_len;
	unsigned char *kse;
	size_t cr_len;
	unsigned char *cr;
} ESNIContents;

/*
 * Place to keep crypto vars for when we try interop.
 * This should probably (mostly) disappear when/if we end up with
 * a final working version that maps to an RFC.
 *
 * Fields below:
 * keyshare: is the client's ephemeral public value
 * shared: is the D-H shared secret
 * hi: encoded ESNIContents hash input 
 * hash: hash output from above
 * Zx: derived from D-H shared secret
 * key: derived from Zx as per I-D
 * iv: derived from Zx as per I-D
 * aad: the AAD for the AEAD
 * plain: encoded plaintext
 * cipher: ciphertext
 * tag: AEAD tag (exposed by OpenSSL api?)
 */
typedef struct esni_crypto_vars_st {
	EVP_PKEY *keyshare;
	size_t shared_len;
	unsigned char *shared; /* shared secret */
	size_t hi_len;
	unsigned char *hi;
	size_t hash_len;
	unsigned char *hash;
	size_t Zx_len;
	unsigned char *Zx;
	size_t key_len;
	unsigned char *key;
	size_t iv_len;
	unsigned char *iv;
	size_t aad_len;
	unsigned char *aad;
	size_t plain_len;
	unsigned char *plain;
	size_t cipher_len;
	unsigned char *cipher;
	size_t tag_len;
	unsigned char *tag;
} ESNI_CRYPTO_VARS;

/*
 * What we send in the esni CH extension:
 *
 *    struct {
 *        CipherSuite suite;
 *        KeyShareEntry key_share;
 *        opaque record_digest<0..2^16-1>;
 *        opaque encrypted_sni<0..2^16-1>;
 *    } ClientEncryptedSNI;
 *
 * We include some related non-transmitted 
 * e.g. key structures too
 *
 */
typedef struct client_esni_st {
	/*
	 * Fields encoded in extension
	 */
	const SSL_CIPHER *ciphersuite;
	size_t encoded_keyshare_len; /* my encoded key share */
	unsigned char *encoded_keyshare;
	size_t record_digest_len;
	unsigned char record_digest[SSL_MAX_SSL_RECORD_DIGEST_LENGTH];
	size_t encrypted_sni_len;
	unsigned char encrypted_sni[SSL_MAX_SSL_ENCRYPTED_SNI_LENGTH];
	/*
	 * Various intermediate/crypto vars
	 */
	ESNIContents econt;
	CLIENT_ESNI_INNER inner;
	ESNI_CRYPTO_VARS cvars;
} CLIENT_ESNI;

/*
 * Per connection ESNI state (inspired by include/internal/dane.h) 
 * Has DNS RR values and some more
 */
typedef struct ssl_esni_st {
	int nerecs; /* number of DNS RRs in RRset */
    ESNI_RECORD *erecs; /* array of these */
    ESNI_RECORD *mesni;      /* Matching esni record */
	CLIENT_ESNI *client;
	const char *encservername;
	const char *frontname;
	uint64_t ttl;
	uint64_t lastread;
} SSL_ESNI;


/*
 * TODO: Include function prototypes in esni.h
 * We'll do that later, with one file for now, no
 * need yet.
 */

/*
 * Utility functions
 */

/*
* Check names for length, maybe add more checks later before starting...
*/
static int esni_checknames(const char *encservername, const char *frontname)
{
	if (OPENSSL_strnlen(encservername,TLSEXT_MAXLEN_host_name)>TLSEXT_MAXLEN_host_name) 
		return(0);
	if (OPENSSL_strnlen(frontname,TLSEXT_MAXLEN_host_name)>TLSEXT_MAXLEN_host_name) 
		return(0);
	/*
	 * Possible checks:
	 * - If no covername, then send no (clear) SNI, so allow that
	 * - Check same A/AAAA exists for both names, if we have both
	 *   	- could be a privacy leak though
	 *   	- even if using DoT/DoH (but how'd we know for sure?)
	 * - check/retrive RR's from DNS if not already in-hand and
	 *   if (sufficiently) privacy preserving
	 */
	return(1);
}

/*
 * map 8 bytes in n/w byte order from PACKET to a 64-bit time value
 * TODO: there must be code for this somewhere - find it
 */
static uint64_t uint64_from_bytes(unsigned char *buf)
{
	uint64_t rv=0;
	rv = ((uint64_t)(*buf)) << 56;
	rv |= ((uint64_t)(*(buf + 1))) << 48;
	rv |= ((uint64_t)(*(buf + 2))) << 40;
	rv |= ((uint64_t)(*(buf + 3))) << 32;
	rv |= ((uint64_t)(*(buf + 4))) << 24;
	rv |= ((uint64_t)(*(buf + 5))) << 16;
	rv |= ((uint64_t)(*(buf + 6))) << 8;
	rv |= *(buf + 7);
	return(rv);
}

/*
 * Decode from TXT RR to binary buffer, this is the
 * exact same as ct_base64_decode from crypto/ct/ct_b64.c
 * which function is declared static but could otherwise
 * be re-used. Returns -1 for error or length of decoded
 * buffer length otherwise (wasn't clear to me at first
 * glance). Possible future change: re-use the ct code by
 * exporting it.
 *
 * Decodes the base64 string |in| into |out|.
 * A new string will be malloc'd and assigned to |out|. This will be owned by
 * the caller. Do not provide a pre-allocated string in |out|.
 */
static int esni_base64_decode(char *in, unsigned char **out)
{
    size_t inlen = strlen(in);
    int outlen, i;
    unsigned char *outbuf = NULL;

    if (inlen == 0) {
        *out = NULL;
        return 0;
    }

    outlen = (inlen / 4) * 3;
    outbuf = OPENSSL_malloc(outlen);
    if (outbuf == NULL) {
        ESNIerr(ESNI_F_BASE64_DECODE, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    outlen = EVP_DecodeBlock(outbuf, (unsigned char *)in, inlen);
    if (outlen < 0) {
        ESNIerr(ESNI_F_BASE64_DECODE, ESNI_R_BASE64_DECODE_ERROR);
        goto err;
    }

    /* Subtract padding bytes from |outlen|.  Any more than 2 is malformed. */
    i = 0;
    while (in[--inlen] == '=') {
        --outlen;
        if (++i > 2) {
        	ESNIerr(ESNI_F_BASE64_DECODE, ESNI_R_BASE64_DECODE_ERROR);
            goto err;
		}
    }

    *out = outbuf;
    return outlen;
err:
    OPENSSL_free(outbuf);
	ESNIerr(ESNI_F_BASE64_DECODE, ESNI_R_BASE64_DECODE_ERROR);
    return -1;
}

/*
 * Free up a CLIENT_ESNI structure
 * We don't free the top level
 */
void CLIENT_ESNI_free(CLIENT_ESNI *c)
{
	if (c == NULL) return;
	if (c->encoded_keyshare != NULL) OPENSSL_free(c->encoded_keyshare);
	if (c->inner.nonce != NULL ) OPENSSL_free(c->inner.nonce);
	if (c->inner.realSNI != NULL ) OPENSSL_free(c->inner.realSNI);
	if (c->econt.rd != NULL) OPENSSL_free(c->econt.rd);
    if (c->cvars.keyshare != NULL) EVP_PKEY_free(c->cvars.keyshare);
	if (c->cvars.shared != NULL) OPENSSL_free(c->cvars.shared);
	if (c->cvars.hi != NULL) OPENSSL_free(c->cvars.hi);
	if (c->cvars.hash != NULL) OPENSSL_free(c->cvars.hash);
	if (c->cvars.Zx != NULL) OPENSSL_free(c->cvars.Zx);
	if (c->cvars.key != NULL) OPENSSL_free(c->cvars.key);
	if (c->cvars.iv != NULL) OPENSSL_free(c->cvars.iv);
	if (c->cvars.aad != NULL) OPENSSL_free(c->cvars.aad);
	if (c->cvars.plain != NULL) OPENSSL_free(c->cvars.plain);
	if (c->cvars.cipher != NULL) OPENSSL_free(c->cvars.cipher);
	if (c->cvars.tag != NULL) OPENSSL_free(c->cvars.tag);
	return;
}

/*
 * Free up an SSL_ESNI structure - note that we don't
 * free the top level
 */
void SSL_ESNI_free(SSL_ESNI *esnikeys)
{
	if (esnikeys==NULL) 
		return;
	if (esnikeys->erecs != NULL) {
		for (int i=0;i!=esnikeys->nerecs;i++) {
			/*
	 		* ciphersuites
	 		*/
			if (esnikeys->erecs[i].ciphersuites!=NULL) {
				STACK_OF(SSL_CIPHER) *sk=esnikeys->erecs->ciphersuites;
				sk_SSL_CIPHER_free(sk);
			}
			/*
	 		* keys
	 		*/
			if (esnikeys->erecs[i].nkeys!=0) {
				for (int j=0;j!=esnikeys->erecs[i].nkeys;j++) {
					EVP_PKEY *pk=esnikeys->erecs[i].keys[j];
					EVP_PKEY_free(pk);
				}
				OPENSSL_free(esnikeys->erecs[i].group_ids);
				OPENSSL_free(esnikeys->erecs[i].keys);
			}
			if (esnikeys->erecs[i].encoded!=NULL) OPENSSL_free(esnikeys->erecs[i].encoded);
		}
	}
	if (esnikeys->erecs!=NULL)
		OPENSSL_free(esnikeys->erecs);
	if (esnikeys->client!=NULL) {
		CLIENT_ESNI_free(esnikeys->client);
		OPENSSL_free(esnikeys->client);
	}
	return;
}

int esni_checksum_check(unsigned char *buf, size_t buf_len)
{
	/* 
	 * copy input with zero'd checksum, do SHA256 hash, compare with checksum, tedious but easy enough
	 */
	unsigned char *buf_zeros=OPENSSL_malloc(buf_len);
	if (buf_zeros==NULL) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ERR_R_MALLOC_FAILURE);
		goto err;
	}
	memcpy(buf_zeros,buf,buf_len);
	memset(buf_zeros+2,0,4);

	unsigned char md[EVP_MAX_MD_SIZE];

    SHA256_CTX context;
    if(!SHA256_Init(&context)) {
		ESNIerr(ESNI_F_CHECKSUM_CHECK, ERR_R_INTERNAL_ERROR);
		goto err;
	}

    if(!SHA256_Update(&context, buf_zeros, buf_len)) {
		ESNIerr(ESNI_F_CHECKSUM_CHECK, ERR_R_INTERNAL_ERROR);
		goto err;
	}

    if(!SHA256_Final(md, &context)) {
		ESNIerr(ESNI_F_CHECKSUM_CHECK, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	OPENSSL_free(buf_zeros);

	if (memcmp(buf+2,md,4)) {
		/* non match - bummer */
		return 0;
	} else {
		return 1;
	}
err:
	if (buf_zeros!=NULL) OPENSSL_free(buf_zeros);
	return 0;
}

/*
 * Decode from TXT RR to SSL_ESNI
 * This time inspired by, but not the same as,
 * SCT_new_from_base64 from crypto/ct/ct_b64.c
 * TODO: handle >1 of the many things that can 
 * have >1 instance (maybe at a higher layer)
 */
SSL_ESNI* SSL_ESNI_new_from_base64(char *esnikeys)
{
	if (esnikeys==NULL)
		return(NULL);

    unsigned char *outbuf = NULL; /* binary representation of ESNIKeys */
    int declen; /* length of binary representation of ESNIKeys */
	SSL_ESNI *newesni=NULL; /* decoded ESNIKeys */

    declen = esni_base64_decode(esnikeys, &outbuf);
    if (declen < 0) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_BASE64_DECODE_ERROR);
        goto err;
    }

	int cksum_ok=esni_checksum_check(outbuf,declen);
	if (cksum_ok!=1) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	PACKET pkt={outbuf,declen};

	newesni=OPENSSL_malloc(sizeof(SSL_ESNI));
	if (newesni==NULL) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ERR_R_MALLOC_FAILURE);
		goto err;
	}

	/* sanity check: version + checksum + KeyShareEntry have to be there - min len >= 10 */
	if (declen < 10) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_BASE64_DECODE_ERROR);
		goto err;
	}

	newesni->nerecs=1;
	newesni->erecs=NULL;
	newesni->erecs=OPENSSL_malloc(sizeof(ESNI_RECORD));
	if (newesni->erecs==NULL) { 
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ERR_R_MALLOC_FAILURE);
		goto err;
	}
	ESNI_RECORD *crec=newesni->erecs;
	crec->encoded_len=declen;
	crec->encoded=outbuf;

	/* version */
	if (!PACKET_get_net_2(&pkt,&crec->version)) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}

	/* checksum decode */
	if (!PACKET_copy_bytes(&pkt,crec->checksum,4)) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	/* 
	 * list of KeyShareEntry elements - 
	 * inspiration: ssl/statem/extensions_srvr.c:tls_parse_ctos_key_share 
	 */
	PACKET key_share_list;
	if (!PACKET_get_length_prefixed_2(&pkt, &key_share_list)) {
        ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
    }

	unsigned int group_id;
	PACKET encoded_pt;
	int nkeys=0;
	unsigned int *group_ids=NULL;
	EVP_PKEY **keys=NULL;

    while (PACKET_remaining(&key_share_list) > 0) {
        if (!PACKET_get_net_2(&key_share_list, &group_id)
                || !PACKET_get_length_prefixed_2(&key_share_list, &encoded_pt)
                || PACKET_remaining(&encoded_pt) == 0) {
        	ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
            goto err;
        }
		/* 
		 * TODO: ensure that we can call this - likely this calling code will need to be
		 * in libssl.so as that seems to hide this symbol, for now, we hack the build
		 * by copying the .a files locally and linking statically
		 */
		EVP_PKEY *kn=ssl_generate_param_group(group_id);
		if (kn==NULL) {
        	ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
            goto err;
		}
        if (!EVP_PKEY_set1_tls_encodedpoint(kn,
                PACKET_data(&encoded_pt),
                PACKET_remaining(&encoded_pt))) {
        	ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
            goto err;
        }
		nkeys++;
		EVP_PKEY** tkeys=(EVP_PKEY**)OPENSSL_realloc(keys,nkeys*sizeof(EVP_PKEY*));
		if (tkeys == NULL ) {
        	ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
            goto err;
		}
		keys=tkeys;
		keys[nkeys-1]=kn;
		group_ids=(unsigned int*)OPENSSL_realloc(group_ids,nkeys*sizeof(unsigned int));
		if (keys == NULL ) {
        	ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
            goto err;
		}
    }
	crec->nkeys=nkeys;
	crec->keys=keys;
	crec->group_ids=group_ids;

	/*
	 * List of ciphersuites - 2 byte len + 2 bytes per ciphersuite
	 * Code here inspired by ssl/ssl_lib.c:bytes_to_cipher_list
	 */
	PACKET cipher_suites;
	if (!PACKET_get_length_prefixed_2(&pkt, &cipher_suites)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	int nsuites=PACKET_remaining(&cipher_suites);
	if (!nsuites || (nsuites % 1)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
    const SSL_CIPHER *c;
    STACK_OF(SSL_CIPHER) *sk = NULL;
    int n;
    unsigned char cipher[TLS_CIPHER_LEN];
    n = TLS_CIPHER_LEN;
    sk = sk_SSL_CIPHER_new_null();
    if (sk == NULL) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
        goto err;
    }
    while (PACKET_copy_bytes(&cipher_suites, cipher, n)) {
        c = ssl3_get_cipher_by_char(cipher);
        if (c != NULL) {
            if (c->valid && !sk_SSL_CIPHER_push(sk, c)) {
				ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
                goto err;
            }
        }
    }
    if (PACKET_remaining(&cipher_suites) > 0) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
        goto err;
    }
    newesni->erecs->ciphersuites=sk;

	if (!PACKET_get_net_2(&pkt,&crec->padded_length)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	unsigned char nbs[8];
	if (!PACKET_copy_bytes(&pkt,nbs,8)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	crec->not_before=uint64_from_bytes(nbs);
	if (!PACKET_copy_bytes(&pkt,nbs,8)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	crec->not_after=uint64_from_bytes(nbs);
	/*
	 * Extensions: we don't yet support any (does anyone?)
	 * TODO: add extensions support at some level 
	 */
	if (!PACKET_get_net_2(&pkt,&crec->nexts)) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	if (crec->nexts != 0 ) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;

	}
	int leftover=PACKET_remaining(&pkt);
	if (leftover!=0) {
		ESNIerr(ESNI_F_NEW_FROM_BASE64, ESNI_R_RR_DECODE_ERROR);
		goto err;
	}
	newesni->client=NULL;
	newesni->mesni=&newesni->erecs[0];
	/*
	 * TODO: check bleedin not_before/not_after as if that's gonna help;-)
	 */

	return(newesni);
err:
	if (newesni!=NULL) {
		SSL_ESNI_free(newesni);
		OPENSSL_free(newesni);
	}
	return(NULL);
}

static void esni_pbuf(BIO *out,char *msg,unsigned char *buf,size_t blen,int indent)
{
	if (buf==NULL) {
		BIO_printf(out,"%s is NULL",msg);
		return;
	}
	BIO_printf(out,"%s (%ld):\n    ",msg,blen);
	int i;
	for (i=0;i!=blen;i++) {
		if ((i!=0) && (i%16==0))
			BIO_printf(out,"\n    ");
		BIO_printf(out,"%02x:",buf[i]);
	}
	BIO_printf(out,"\n");
	return;
}

/*
 * Print out the DNS RR value(s)
 */
int SSL_ESNI_print(BIO* out, SSL_ESNI *esni)
{
	int indent=0;
	int rv=0;
	if (esni==NULL) {
		BIO_printf(out,"ESNI is NULL!\n");
		return(1);
	}
	BIO_printf(out,"ESNI has %d RRsets\n",esni->nerecs);
	if (esni->erecs==NULL) {
		BIO_printf(out,"ESNI has no keys!\n");
		return(1);
	}
	for (int e=0;e!=esni->nerecs;e++) {
		BIO_printf(out,"ESNI Server version: 0x%x\n",esni->erecs[e].version);
		BIO_printf(out,"ESNI Server checksum: 0x");
		for (int i=0;i!=4;i++) {
			BIO_printf(out,"%02x",esni->erecs[e].checksum[i]);
		}
		BIO_printf(out,"\n");
		BIO_printf(out,"ESNI Server Keys: %d\n",esni->erecs[e].nkeys);
		for (int i=0;i!=esni->erecs[e].nkeys;i++) {
			BIO_printf(out,"ESNI Server Key[%d]: ",i);
			if (esni->erecs->keys && esni->erecs[e].keys[i]) {
				rv=EVP_PKEY_print_public(out, esni->erecs[e].keys[i], indent, NULL); 
				if (!rv) {
					BIO_printf(out,"Oops: %d\n",rv);
				}
			} else {
				BIO_printf(out,"Key %d is NULL!\n",i);
			}
		}
    	STACK_OF(SSL_CIPHER) *sk = esni->erecs[e].ciphersuites;
		if (sk==NULL) {
			BIO_printf(out,"ESNI Server, No ciphersuites!\n");
		} else {
			for (int i = 0; i < sk_SSL_CIPHER_num(sk); i++) {
				const SSL_CIPHER *c = sk_SSL_CIPHER_value(sk, i);
				if (c!=NULL) {
					BIO_printf(out,"ESNI Server Ciphersuite %d is %s\n",i,c->name);
				} else {
					BIO_printf(out,"ENSI Server Ciphersuite %d is NULL\n",i);
				}
			}
	
		}
		BIO_printf(out,"ESNI Server padded_length: %d\n",esni->erecs[e].padded_length);
		BIO_printf(out,"ESNI Server not_before: %lu\n",esni->erecs[e].not_before);
		BIO_printf(out,"ESNI Server not_after: %lu\n",esni->erecs[e].not_after);
		BIO_printf(out,"ESNI Server number of extensions: %d\n",esni->erecs[e].nexts);
	}
	CLIENT_ESNI *c=esni->client;
	if (c == NULL) {
		BIO_printf(out,"ESNI client not done yet.\n");
	} else {
		if (c->ciphersuite!=NULL) {
			BIO_printf(out,"ESNI Client Ciphersuite is %s\n",c->ciphersuite->name);
		} else {
			BIO_printf(out,"ESNI Client Ciphersuite is NULL\n");
		}


		esni_pbuf(out,"ESNI CLient encoded_keyshare",c->encoded_keyshare,c->encoded_keyshare_len,indent);
		CLIENT_ESNI_INNER *ci=&c->inner;
		esni_pbuf(out,"ESNI CLient inner nonce",ci->nonce,ci->nonce_len,indent);
		esni_pbuf(out,"ESNI CLient inner realSNI",ci->realSNI,
							esni->mesni->padded_length,
							indent);
		/* TODO: when these values figured out - print 'em
	size_t encrypted_sni_len;
	unsigned char encrypted_sni[SSL_MAX_SSL_ENCRYPTED_SNI_LENGTH];
		*/

		esni_pbuf(out,"ESNI CLient ESNIContents record_digest",
							c->econt.rd,c->econt.rd_len,indent);
		esni_pbuf(out,"ESNI CLient ESNIContents client_random",
							c->econt.cr,c->econt.cr_len,indent);

		if (c->cvars.keyshare != NULL) {
			BIO_printf(out,"ESNI Cryptovars Keyshare:\n");
			rv=EVP_PKEY_print_public(out, c->cvars.keyshare, indent, NULL); 
			if (!rv) {
				BIO_printf(out,"Oops: %d\n",rv);
			}
		} else {
			BIO_printf(out,"ESNI Client Keyshare is NULL!\n");
		}
		esni_pbuf(out,"ESNI Cryptovars shared",c->cvars.shared,c->cvars.shared_len,indent);
		esni_pbuf(out,"ESNI Cryptovars hash input",c->cvars.hi,c->cvars.hi_len,indent);
		/* don't bother with key share - it's above already */
		esni_pbuf(out,"ESNI Cryptovars hash(ESNIContents)",
							c->cvars.hash,c->cvars.hash_len,indent);
		esni_pbuf(out,"ESNI Cryptovars Zx",c->cvars.Zx,c->cvars.Zx_len,indent);
		esni_pbuf(out,"ESNI Cryptovars key",c->cvars.key,c->cvars.key_len,indent);
		esni_pbuf(out,"ESNI Cryptovars iv",c->cvars.iv,c->cvars.iv_len,indent);
		esni_pbuf(out,"ESNI Cryptovars plain",c->cvars.plain,c->cvars.plain_len,indent);
		esni_pbuf(out,"ESNI Cryptovars cipher",c->cvars.cipher,c->cvars.cipher_len,indent);
		esni_pbuf(out,"ESNI Cryptovars tag",c->cvars.tag,c->cvars.tag_len,indent);
	}
	return(1);
}

/*
 * Make a 16 octet nonce for ESNI
 */
static unsigned char *esni_nonce(size_t nl)
{
	unsigned char *ln=OPENSSL_malloc(nl);
	RAND_bytes(ln,nl);
	return ln;
}

/*
 * Pad an SNI before encryption
 */
static unsigned char *esni_pad(char *name, unsigned int padded_len)
{
	/*
	 * usual function is statem/extensions_clnt.c:tls_construct_ctos_server_name
	 * encoding is 2 byte overall length, 0x00 for hostname, 2 byte length of name, name
	 */
	size_t nl=OPENSSL_strnlen(name,padded_len);
	size_t oh=5; /* encoding overhead */
	if ((nl+oh)>=padded_len) return(NULL);
	unsigned char *buf=OPENSSL_malloc(padded_len);
	memset(buf,0,padded_len);
	buf[0]=((nl+oh)/256);
	buf[1]=((nl+oh)%256);
	buf[2]=0x00;
	buf[3]=(nl/256);
	buf[4]=(nl%256);
	memcpy(buf+oh,name,nl);
	return buf;
}

/*
 * Hash up ESNIContents as per I-D
 */
static int esni_contentshash(ESNIContents *e, ESNI_CRYPTO_VARS *cv, const EVP_MD *md)
{
	size_t oh=2+2+2;
	cv->hi_len=oh+e->rd_len+e->kse_len+e->cr_len;
	cv->hi=OPENSSL_zalloc(cv->hi_len);
	if (cv->hi==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_MALLOC_FAILURE);
        goto err;
	}
	unsigned char *hip=cv->hi;
	*hip++=e->rd_len/256;
	*hip++=e->rd_len%256;
	memcpy(hip,e->rd,e->rd_len); 
	hip+=e->rd_len;
	*hip++=e->kse_len/256;
	*hip++=e->kse_len%256;
	memcpy(hip,e->kse,e->kse_len); 
	hip+=e->kse_len;
	*hip++=e->cr_len/256;
	*hip++=e->cr_len%256;
	memcpy(hip,e->cr,e->cr_len); 
	hip+=e->cr_len;
	cv->hi_len=hip-cv->hi;
	EVP_MD_CTX *mctx = NULL;
	mctx = EVP_MD_CTX_new();
	cv->hash_len = EVP_MD_size(md);
	cv->hash=OPENSSL_malloc(cv->hash_len);
	if (cv->hash==NULL) {
		goto err;
	}
    if (mctx == NULL
            || EVP_DigestInit_ex(mctx, md, NULL) <= 0
			|| EVP_DigestUpdate(mctx, cv->hi, cv->hi_len) <= 0
            || EVP_DigestFinal_ex(mctx, cv->hash, NULL) <= 0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}
	EVP_MD_CTX_free(mctx);
	return 1;
err:
	if (mctx!=NULL) EVP_MD_CTX_free(mctx);
	if (cv->hash!=NULL) OPENSSL_free(cv->hash);
    return 0;
}

/*
 * Local wrapper for HKDF-Extract(salt,IVM)=HMAC-Hash(salt,IKM) according
 * to RFC5689
 */
static unsigned char *esni_hkdf_extract(unsigned char *secret,size_t slen,size_t *olen, const EVP_MD *md)
{
	int ret=1;
	unsigned char *outsecret=NULL;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	if (pctx==NULL) {
		return NULL;
	}

	outsecret=OPENSSL_zalloc(EVP_MAX_MD_SIZE);
	if (outsecret==NULL) {
		EVP_PKEY_CTX_free(pctx);
		return NULL;
	}

	/* 
	 * based on ssl/tls13_enc.c:tls13_generate_secret
	 */

    ret = EVP_PKEY_derive_init(pctx) <= 0
            || EVP_PKEY_CTX_hkdf_mode(pctx, EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) <= 0
            || EVP_PKEY_CTX_set_hkdf_md(pctx, md) <= 0
            || EVP_PKEY_CTX_set1_hkdf_key(pctx, secret, slen) <= 0
            || EVP_PKEY_CTX_set1_hkdf_salt(pctx, NULL, 0) <= 0
            || EVP_PKEY_derive(pctx, outsecret, olen) <= 0;

	EVP_PKEY_CTX_free(pctx);

	if (ret!=0) {
		OPENSSL_free(outsecret);
		return NULL;
	}
	return outsecret;
}


unsigned char *esni_hkdf_expand_label(
			unsigned char *Zx, size_t Zx_len,
			const char *label,
			unsigned char *hash, size_t hash_len,
			size_t *expanded_len,
			const EVP_MD *md)
{
	int ret=1;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	if (pctx==NULL) {
		return NULL;
	}

	unsigned char *out=OPENSSL_zalloc(EVP_MAX_MD_SIZE);
	if (out==NULL) {
		EVP_PKEY_CTX_free(pctx);
		return NULL;
	}

    ret = EVP_PKEY_derive_init(pctx) <= 0
            || EVP_PKEY_CTX_hkdf_mode(pctx, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY)
               <= 0
            || EVP_PKEY_CTX_set_hkdf_md(pctx, md) <= 0
            || EVP_PKEY_CTX_set1_hkdf_key(pctx, Zx, Zx_len) <= 0
            || EVP_PKEY_CTX_add1_hkdf_info(pctx, label, strlen(label)) <= 0
            || EVP_PKEY_derive(pctx, out, expanded_len) <= 0;

    EVP_PKEY_CTX_free(pctx);
	if (ret!=0) {
		OPENSSL_free(out);
		return NULL;
	}
	return out;
}

unsigned char *esni_aead_enc(
			unsigned char *key, size_t key_len,
			unsigned char *iv, size_t iv_len,
			unsigned char *aad, size_t aad_len,
			unsigned char *plain, size_t plain_len,
			unsigned char *tag, size_t tag_len, 
			size_t *cipher_len,
			const SSL_CIPHER *ciph)
{
	/*
	 * From https://wiki.openssl.org/index.php/EVP_Authenticated_Encryption_and_Decryption
	 */

	EVP_CIPHER_CTX *ctx;
	int len;
	size_t ciphertext_len;
	unsigned char *ciphertext=NULL;

	ciphertext=OPENSSL_malloc(plain_len+16);
	if (ciphertext==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Create and initialise the context */
	if(!(ctx = EVP_CIPHER_CTX_new())) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Initialise the encryption operation. */
	/*
	 * derive EVP settings from SSL_CIPHER input via fake SSL session (for now)
	 */
	const EVP_CIPHER *enc=EVP_get_cipherbynid(SSL_CIPHER_get_cipher_nid(ciph));
	if (enc == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	if(1 != EVP_EncryptInit_ex(ctx, enc, NULL, NULL, NULL)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Set IV length if default 12 bytes (96 bits) is not appropriate */
	if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Initialise key and IV */
	if(1 != EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv))  {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Provide any AAD data. This can be called zero or more times as
	 * required
	 */
	if(1 != EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/* Provide the message to be encrypted, and obtain the encrypted output.
	 * EVP_EncryptUpdate can be called multiple times if necessary
	 */
	if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plain, plain_len)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	ciphertext_len = len;

	/* Finalise the encryption. Normally ciphertext bytes may be written at
	 * this stage, but this does not occur in GCM mode
	 */
	if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))  {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	ciphertext_len += len;

	/* Get the tag */
	/*
	 * This isn't a duplicate so needs to be added to the ciphertext
	 */
	if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_len, tag)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}
	memcpy(ciphertext+plain_len,tag,tag_len);
	ciphertext_len += tag_len;

	/* Clean up */
	EVP_CIPHER_CTX_free(ctx);

	*cipher_len=ciphertext_len;

	return ciphertext;

err:
	EVP_CIPHER_CTX_free(ctx);
	if (ciphertext!=NULL) OPENSSL_free(ciphertext);
	return NULL;
}

int esni_make_rd(ESNI_RECORD *er,ESNIContents *ec)
{
	const SSL_CIPHER *sc=sk_SSL_CIPHER_value(er->ciphersuites,0);
	const EVP_MD *md=ssl_md(sc->algorithm2);

	if (er->encoded_len<=2) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}
	unsigned char *hip=er->encoded+2;
	size_t hi_len=er->encoded_len-2;

	EVP_MD_CTX *mctx = NULL;
	mctx = EVP_MD_CTX_new();
	ec->rd_len=EVP_MD_size(md);
	ec->rd=OPENSSL_malloc(ec->rd_len);
	if (ec->rd==NULL) {
		goto err;
	}
    if (mctx == NULL
            || EVP_DigestInit_ex(mctx, md, NULL) <= 0
			|| EVP_DigestUpdate(mctx, hip, hi_len) <= 0
            || EVP_DigestFinal_ex(mctx, ec->rd, NULL) <= 0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}
	EVP_MD_CTX_free(mctx);

	return 1;
err:
	return 0;
}

/*
 * Produce the encrypted SNI value for the CH
 */
int SSL_ESNI_enc(SSL_ESNI *esnikeys, 
				char *protectedserver, 
				char *frontname, 
				PACKET *the_esni, 
				size_t  cr_len,
				unsigned char *client_random)
{

	/*
	 * - make my private key
	 * - generate shared secret
	 * - encrypt protectedserver
	 * - encode packet and return
	 */
	if (esnikeys->client != NULL ) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}
	CLIENT_ESNI *cesni=OPENSSL_zalloc(sizeof(CLIENT_ESNI));
	if (cesni==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_MALLOC_FAILURE);
		goto err;
	}
	ESNI_CRYPTO_VARS *cv=&cesni->cvars;
	memset(cv,0,sizeof(ESNI_CRYPTO_VARS));
	CLIENT_ESNI_INNER *inner=&cesni->inner;

	/*
	 * D-H stuff inspired by openssl/statem/statem_clnt.c:tls_construct_cke_ecdhe
	 */
    EVP_PKEY *skey = NULL;
    int ret = 0;

	if (esnikeys->erecs==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	if (esnikeys->erecs->nkeys==0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
		goto err;
	}

	/*
	 * TODO: handle cases of >1 thing, for now we just pick 1st and hope...
	 */
	if (esnikeys->nerecs>1) {
		ESNIerr(ESNI_F_ENC, ESNI_R_NOT_IMPL);
	}
	if (esnikeys->erecs[0].nkeys>1) {
		ESNIerr(ESNI_F_ENC, ESNI_R_NOT_IMPL);
	}
	if (sk_SSL_CIPHER_num(esnikeys->erecs[0].ciphersuites)>1) {
		ESNIerr(ESNI_F_ENC, ESNI_R_NOT_IMPL);
	}

	cesni->ciphersuite=sk_SSL_CIPHER_value(esnikeys->erecs[0].ciphersuites,0);

    skey = esnikeys->erecs[0].keys[0];
    if (skey == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    cesni->cvars.keyshare = ssl_generate_pkey(skey);
    if (cesni->cvars.keyshare == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }

	/*
	 * code from ssl/s3_lib.c:ssl_derive
	 */
    EVP_PKEY_CTX *pctx;
	pctx = EVP_PKEY_CTX_new(cesni->cvars.keyshare, NULL);
    if (EVP_PKEY_derive_init(pctx) <= 0
        || EVP_PKEY_derive_set_peer(pctx, skey) <= 0
        || EVP_PKEY_derive(pctx, NULL, &cesni->cvars.shared_len) <= 0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    cesni->cvars.shared = OPENSSL_malloc(cesni->cvars.shared_len);
    if (cesni->cvars.shared == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if (EVP_PKEY_derive(pctx, cesni->cvars.shared, &cesni->cvars.shared_len) <= 0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Generate encoding of client key */
    cesni->encoded_keyshare_len = EVP_PKEY_get1_tls_encodedpoint(cesni->cvars.keyshare, &cesni->encoded_keyshare);
    if (cesni->encoded_keyshare_len == 0) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
    }

	/*
	 * Form up the inner SNI stuff
	 */
	inner->realSNI_len=esnikeys->mesni->padded_length;
	inner->realSNI=esni_pad(protectedserver,inner->realSNI_len);
	if (inner->realSNI==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}
	inner->nonce_len=16;
	inner->nonce=esni_nonce(inner->nonce_len);
	if (!inner->nonce) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	/*
	 * encode into our plaintext
	 */
	int oh=2;
	cv->plain_len=oh+inner->nonce_len+inner->realSNI_len;
	cv->plain=OPENSSL_malloc(cv->plain_len);
	if (cv->plain == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}
	unsigned char *pip=cv->plain;
	memcpy(pip,inner->nonce,inner->nonce_len); pip+=inner->nonce_len;
	*pip++=inner->realSNI_len/256;
	*pip++=inner->realSNI_len%256;
	memcpy(pip,inner->realSNI,inner->realSNI_len); pip+=inner->realSNI_len;

	/* 
	 * encrypt the actual SNI based on shared key, Z - the I-D says:
	 *    Zx = HKDF-Extract(0, Z)
     *    key = HKDF-Expand-Label(Zx, "esni key", Hash(ESNIContents), key_length)
     *    iv = HKDF-Expand-Label(Zx, "esni iv", Hash(ESNIContents), iv_length)
	 *
     *    struct {
     *        opaque record_digest<0..2^16-1>;
     *        KeyShareEntry esni_key_share;
	 *        Random client_hello_random;
     *    } ESNIContents;
	 *
	 * The above implies we need the CH random as an input (or
	 * the SSL context, but not yet for that)
	 *
	 * client_random is unsigned char client_random[SSL3_RANDOM_SIZE];
	 * from ssl/ssl_locl.h
	 */
	ESNIContents *esnicontents=&cesni->econt;

	/*
	 * Calculate digest of input RR as per I-D
	 */
	if (!esni_make_rd(esnikeys->mesni,esnicontents)) {
		ESNIerr(ESNI_F_ENC, ERR_R_MALLOC_FAILURE);
        goto err;
	}
	esnicontents->kse_len=cesni->encoded_keyshare_len;
	esnicontents->kse=cesni->encoded_keyshare;
	esnicontents->cr_len=SSL3_RANDOM_SIZE;
	esnicontents->cr=client_random;

	/*
	 * Form up input for hashing, and hash it
	 */

	const SSL_CIPHER *sc=sk_SSL_CIPHER_value(esnikeys->mesni->ciphersuites,0);
	const EVP_MD *md=ssl_md(sc->algorithm2);
	if (!esni_contentshash(esnicontents,cv,md)) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	/*
	 * Derive key and encrypt
	 * encrypt the actual SNI based on shared key, Z - the I-D says:
	 *    Zx = HKDF-Extract(0, Z)
     *    key = HKDF-Expand-Label(Zx, "esni key", Hash(ESNIContents), key_length)
     *    iv = HKDF-Expand-Label(Zx, "esni iv", Hash(ESNIContents), iv_length)
	 */
	cv->Zx_len=0;
	cv->Zx=esni_hkdf_extract(cv->shared,cv->shared_len,&cv->Zx_len,md);
	if (cv->Zx==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	cv->key_len=32;
	cv->key=esni_hkdf_expand_label(cv->Zx,cv->Zx_len,"esni keys",
					cv->hash,cv->hash_len,&cv->key_len,md);
	if (cv->key==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}
	cv->iv_len=32;
	cv->iv=esni_hkdf_expand_label(cv->Zx,cv->Zx_len,"esni iv",
					cv->hash,cv->hash_len,&cv->iv_len,md);
	if (cv->iv==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	/*
	 * The actual encryption... from the I-D:
	 *     encrypted_sni = AEAD-Encrypt(key, iv, ClientHello.KeyShareClientHello, ClientESNIInner)
	 */

	/*
	 * Copy the ClientHello.KeyShareClientHello in here as aad. 
	 */
	cv->aad_len=cr_len;
	cv->aad=OPENSSL_zalloc(cv->aad_len); 
	if (!cv->aad) {
		ESNIerr(ESNI_F_ENC, ERR_R_MALLOC_FAILURE);
        goto err;
	}
	memcpy(cv->aad,client_random,cr_len);

	/*
	 * Tag is in ciphertext anyway, but sure may as well keep it
	 */
	cv->tag_len=16;
	cv->tag=OPENSSL_malloc(cv->tag_len);
	if (cv->tag == NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_MALLOC_FAILURE);
        goto err;
	}

	cv->cipher=esni_aead_enc(cv->key, cv->key_len,
			cv->iv, cv->iv_len,
			cv->aad, cv->aad_len,
			cv->plain, cv->plain_len,
			cv->tag, cv->tag_len,
			&cv->cipher_len,
			cesni->ciphersuite);
	if (cv->cipher==NULL) {
		ESNIerr(ESNI_F_ENC, ERR_R_INTERNAL_ERROR);
        goto err;
	}

	/* 
	 * finish up
	 */
	esnikeys->client=cesni;
	EVP_PKEY_CTX_free(pctx);

    ret = 1;
	return(ret);
 err:
	if (pctx!=NULL) EVP_PKEY_CTX_free(pctx);
	if (cesni!=NULL) {
		CLIENT_ESNI_free(cesni);
		OPENSSL_free(cesni);
	}
    return ret;
}

#endif

#ifdef TESTMAIN
// code within here need not be openssl-style, but we'll migrate there:-)
int main(int argc, char **argv)
{
	int rv;
	// s_client gets stuff otherwise but for now...
	// usage: esni frontname esniname
	if (argc!=3 && argc!=4) {
		printf("usage: esni frontname esniname [esnikeys]\n");
		exit(1);
	}
	if (!ERR_load_ESNI_strings()) {
		printf("Can't init error strings - exiting\n");
		exit(1);
	}
	// init ciphers
	if (!ssl_load_ciphers()) {
		printf("Can't init ciphers - exiting\n");
		exit(1);
	}
	if (!RAND_set_rand_method(NULL)) {
		printf("Can't init (P)RNG - exiting\n");
		exit(1);
	}
	char *encservername=OPENSSL_strdup(argv[1]);
	char *frontname=OPENSSL_strdup(argv[2]);
	char *esnikeys_b64=NULL;
	char *deffront="cloudflare.net";
	FILE *fp=NULL;
	BIO *out=NULL;
	SSL_ESNI *esnikeys=NULL;
	PACKET the_esni={NULL,0};
	/* 
	 * fake client random
	 */
	size_t cr_len=SSL3_RANDOM_SIZE;
	unsigned char client_random[SSL3_RANDOM_SIZE];
	RAND_bytes(client_random,cr_len);


	if (argc==4) 
		esnikeys_b64=OPENSSL_strdup(argv[3]);
	else
		esnikeys_b64=deffront;

	if (!(rv=esni_checknames(encservername,frontname))) {
		printf("Bad names! %d\n",rv);
		goto end;
	}

	esnikeys=SSL_ESNI_new_from_base64(esnikeys_b64);
	if (esnikeys == NULL) {
		printf("Can't create SSL_ESNI from b64!\n");
		goto end;
	}

	fp=fopen("/dev/stdout","w");
	if (fp==NULL)
		goto end;

	out=BIO_new_fp(fp,BIO_CLOSE|BIO_FP_TEXT);
	if (out == NULL)
		goto end;

	if (!SSL_ESNI_enc(esnikeys,encservername,frontname,&the_esni,cr_len,client_random)) {
		printf("Can't encrypt SSL_ESNI!\n");
		goto end;
	}

	if (!SSL_ESNI_print(out,esnikeys)) {
		printf("Can't print SSL_ESNI!\n");
		goto end;
	}

end:
	BIO_free_all(out);
	OPENSSL_free(encservername);
	OPENSSL_free(frontname);
	if (argc==4) 
		OPENSSL_free(esnikeys_b64);
	if (esnikeys!=NULL) {
		SSL_ESNI_free(esnikeys);
		OPENSSL_free(esnikeys);
	}
	if (the_esni.curr!=NULL) {
		OPENSSL_free((char*)the_esni.curr);
	}
	return(0);
}
#endif




