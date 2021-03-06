#include "lib.h"
#include "buffer.h"
#include "str.h"
#include "hex-binary.h"
#include "safe-memset.h"
#include "randgen.h"
#include "array.h"
#include "module-dir.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/engine.h>
#include <openssl/hmac.h>
#include <openssl/objects.h>
#include "dcrypt.h"
#include "dcrypt-private.h"

/**

 key format documentation:
 =========================

 v1 key
 ------
 algo id = openssl NID
 enctype = 0 = none, 1 = ecdhe, 2 = password
 key id = sha256(hex encoded public point)

 public key
 ----------
 1<tab>algo id<tab>public point

 private key
 -----------
 - enctype none
 1<tab>algo id<tab>0<tab>private point<tab>key id

 - enctype ecdh (algorithm AES-256-CTR, key = SHA256(shared secret), IV = \0\0\0...)
 1<tab>algo id<tab>1<tab>private point<tab>ephemeral public key<tab>encryption key id<tab>key id

 - enctype password (algorithm AES-256-CTR, key = PBKDF2(SHA1, 16, password, salt), IV = \0\0\0...)
 1<tab>algo id<tab>2<tab>private point<tab>salt<tab>key id

 v2 key
 ------
 algo oid = ASN1 OID of key algorithm (RSA or EC curve)
 enctype = 0 = none, 1 = ecdhe, 2 = password
 key id = SHA256(i2d_PUBKEY)

 public key
 ----------
 2<tab>HEX(i2d_PUBKEY)

 - enctype none
 2<tab>key algo oid<tab>0<tab>(RSA = i2d_PrivateKey, EC=Private Point)<tab>key id

 - enctype ecdh, key,iv = PBKDF2(hash algo, rounds, shared secret, salt)
 2<tab>key algo oid<tab>1<tab>symmetric algo name<tab>salt<tab>hash algo<tab>rounds<tab>E(RSA = i2d_PrivateKey, EC=Private Point)<tab>ephemeral public key<tab>encryption key id<tab>key id

 - enctype password, key,iv = PBKDF2(hash algo, rounds, password, salt)
  2<tab>key algo oid<tab>1<tab>symmetric algo name<tab>salt<tab>hash algo<tab>rounds<tab>E(RSA = i2d_PrivateKey, EC=Private Point)<tab>key id
**/

#if SSLEAY_VERSION_NUMBER < 0x1010000fL
#define EVP_PKEY_get0_EC_KEY(x) x->pkey.ec
#define EVP_PKEY_get0_RSA(x) x->pkey.rsa
#endif

struct dcrypt_context_symmetric {
	pool_t pool;
	const EVP_CIPHER *cipher;
	EVP_CIPHER_CTX *ctx;
	unsigned char *key;
	unsigned char *iv;
	unsigned char *aad;
	size_t aad_len;
	unsigned char *tag;
	size_t tag_len;
	int padding;
	int mode;
};

struct dcrypt_context_hmac {
	pool_t pool;
	const EVP_MD *md;
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	HMAC_CTX *ctx;
#else
	HMAC_CTX ctx;
#endif
	unsigned char *key;
	size_t klen;
};

struct dcrypt_public_key {
	void *ctx;
};

struct dcrypt_private_key {
	void *ctx;
};

static
bool dcrypt_openssl_private_to_public_key(struct dcrypt_private_key *priv_key, struct dcrypt_public_key **pub_key, const char **error_r);
static
bool dcrypt_openssl_public_key_id(struct dcrypt_public_key *key, const char *algorithm, buffer_t *result, const char **error_r);
static
bool dcrypt_openssl_public_key_id_old(struct dcrypt_public_key *key, buffer_t *result, const char **error_r);
static
bool dcrypt_openssl_private_to_public_key(struct dcrypt_private_key *priv_key, struct dcrypt_public_key **pub_key_r, const char **error_r ATTR_UNUSED);
static
void dcrypt_openssl_free_private_key(struct dcrypt_private_key **key);
static
void dcrypt_openssl_free_public_key(struct dcrypt_public_key **key);
static
bool dcrypt_openssl_rsa_decrypt(struct dcrypt_private_key *key, const unsigned char *data, size_t data_len, buffer_t *result, const char **error_r);

static
bool dcrypt_openssl_error(const char **error_r)
{
	if(error_r == NULL) return FALSE; /* caller is not really interested */
	unsigned long ec = ERR_get_error();
	*error_r = t_strdup_printf("%s", ERR_error_string(ec, NULL));
	return FALSE;
}

/* legacy function for old formats that generates
   hex encoded point from EC public key
 */
static
char *ec_key_get_pub_point_hex(const EC_KEY *key)
{
	const EC_POINT *p;
	const EC_GROUP *g;

	p = EC_KEY_get0_public_key(key);
	g = EC_KEY_get0_group(key);
	return EC_POINT_point2hex(g, p, POINT_CONVERSION_COMPRESSED, NULL);
}

static
bool dcrypt_openssl_ctx_sym_create(const char *algorithm, enum dcrypt_sym_mode mode, struct dcrypt_context_symmetric **ctx_r, const char **error_r)
{
	struct dcrypt_context_symmetric *ctx;
	pool_t pool;
	const EVP_CIPHER *cipher;
	cipher = EVP_get_cipherbyname(algorithm);
	if (cipher == NULL) {
		if (error_r != NULL)
			*error_r = t_strdup_printf("Invalid cipher %s", algorithm);
		return FALSE;
	}
	/* allocate context */
	pool = pool_alloconly_create("dcrypt openssl", 1024);
	ctx = p_new(pool, struct dcrypt_context_symmetric, 1);
	ctx->pool = pool;
	ctx->cipher = cipher;
	ctx->padding = 1;
	ctx->mode =( mode == DCRYPT_MODE_ENCRYPT ? 1 : 0 );
	*ctx_r = ctx;
	return TRUE;
}

static
void dcrypt_openssl_ctx_sym_destroy(struct dcrypt_context_symmetric **ctx)
{
	pool_t pool = (*ctx)->pool;
	if ((*ctx)->ctx) EVP_CIPHER_CTX_free((*ctx)->ctx);
	pool_unref(&pool);
	*ctx = NULL;
}

static
void dcrypt_openssl_ctx_sym_set_key(struct dcrypt_context_symmetric *ctx, const unsigned char *key, size_t key_len)
{
	if(ctx->key != NULL) p_free(ctx->pool, ctx->key);
	ctx->key = p_malloc(ctx->pool, EVP_CIPHER_key_length(ctx->cipher));
	memcpy(ctx->key, key, I_MIN(key_len,(size_t)EVP_CIPHER_key_length(ctx->cipher)));
}

static
void dcrypt_openssl_ctx_sym_set_iv(struct dcrypt_context_symmetric *ctx, const unsigned char *iv, size_t iv_len)
{
	if(ctx->iv != NULL) p_free(ctx->pool, ctx->iv);
	ctx->iv = p_malloc(ctx->pool, EVP_CIPHER_iv_length(ctx->cipher));
	memcpy(ctx->iv, iv, I_MIN(iv_len,(size_t)EVP_CIPHER_iv_length(ctx->cipher)));
}

static
void dcrypt_openssl_ctx_sym_set_key_iv_random(struct dcrypt_context_symmetric *ctx)
{
	if(ctx->key != NULL) p_free(ctx->pool, ctx->key);
	if(ctx->iv != NULL) p_free(ctx->pool, ctx->iv);
	ctx->key = p_malloc(ctx->pool, EVP_CIPHER_key_length(ctx->cipher));
	random_fill(ctx->key, EVP_CIPHER_key_length(ctx->cipher));
	ctx->iv = p_malloc(ctx->pool, EVP_CIPHER_iv_length(ctx->cipher));
	random_fill(ctx->iv, EVP_CIPHER_iv_length(ctx->cipher));
}

static
void dcrypt_openssl_ctx_sym_set_padding(struct dcrypt_context_symmetric *ctx, bool padding)
{
	ctx->padding = (padding?1:0);
}

static
bool dcrypt_openssl_ctx_sym_get_key(struct dcrypt_context_symmetric *ctx, buffer_t *key)
{
	if(ctx->key == NULL) return FALSE;
	buffer_append(key, ctx->key, EVP_CIPHER_key_length(ctx->cipher));
	return TRUE;
}
static
bool dcrypt_openssl_ctx_sym_get_iv(struct dcrypt_context_symmetric *ctx, buffer_t *iv)
{
	if(ctx->iv == NULL) return FALSE;
	buffer_append(iv, ctx->iv, EVP_CIPHER_iv_length(ctx->cipher));
	return TRUE;
}

static
void dcrypt_openssl_ctx_sym_set_aad(struct dcrypt_context_symmetric *ctx, const unsigned char *aad, size_t aad_len)
{
	if (ctx->aad != NULL) p_free(ctx->pool, ctx->aad);
	/* allow empty aad */
	ctx->aad = p_malloc(ctx->pool, I_MAX(1,aad_len));
	memcpy(ctx->aad, aad, aad_len);
	ctx->aad_len = aad_len;
}

static
bool dcrypt_openssl_ctx_sym_get_aad(struct dcrypt_context_symmetric *ctx, buffer_t *aad)
{
	if (ctx->aad == NULL) return FALSE;
	buffer_append(aad, ctx->aad, ctx->aad_len);
	return TRUE;
}

static
void dcrypt_openssl_ctx_sym_set_tag(struct dcrypt_context_symmetric *ctx, const unsigned char *tag, size_t tag_len)
{
	if (ctx->tag != NULL) p_free(ctx->pool, ctx->tag);
	/* unlike aad, tag cannot be empty */
	ctx->tag = p_malloc(ctx->pool, tag_len);
	memcpy(ctx->tag, tag, tag_len);
	ctx->tag_len = tag_len;
}

static
bool dcrypt_openssl_ctx_sym_get_tag(struct dcrypt_context_symmetric *ctx, buffer_t *tag)
{
	if (ctx->tag == NULL) return FALSE;
	buffer_append(tag, ctx->tag, ctx->tag_len);
	return TRUE;
}

static
unsigned int dcrypt_openssl_ctx_sym_get_key_length(struct dcrypt_context_symmetric *ctx)
{
	return EVP_CIPHER_iv_length(ctx->cipher);
}
static
unsigned int dcrypt_openssl_ctx_sym_get_iv_length(struct dcrypt_context_symmetric *ctx)
{
	return EVP_CIPHER_iv_length(ctx->cipher);
}
static
unsigned int dcrypt_openssl_ctx_sym_get_block_size(struct dcrypt_context_symmetric *ctx)
{
	return EVP_CIPHER_block_size(ctx->cipher);
}

static
bool dcrypt_openssl_ctx_sym_init(struct dcrypt_context_symmetric *ctx, const char **error_r)
{
	int ec;
	int len;
	i_assert(ctx->key != NULL);
	i_assert(ctx->iv != NULL);
	i_assert(ctx->ctx == NULL);

	if((ctx->ctx = EVP_CIPHER_CTX_new()) == NULL)
		return dcrypt_openssl_error(error_r);

	ec = EVP_CipherInit_ex(ctx->ctx, ctx->cipher, NULL, ctx->key, ctx->iv, ctx->mode);
	if (ec != 1) return dcrypt_openssl_error(error_r);
	EVP_CIPHER_CTX_set_padding(ctx->ctx, ctx->padding);
	len = 0;
	if (ctx->aad != NULL) ec = EVP_CipherUpdate(ctx->ctx, NULL, &len, ctx->aad, ctx->aad_len);
	if (ec != 1) return dcrypt_openssl_error(error_r);
	return TRUE;
}

static
bool dcrypt_openssl_ctx_sym_update(struct dcrypt_context_symmetric *ctx, const unsigned char *data, size_t data_len, buffer_t *result, const char **error_r)
{
	const size_t block_size = (size_t)EVP_CIPHER_block_size(ctx->cipher);
	size_t buf_used = result->used;
	unsigned char *buf;
	int outl;

	i_assert(ctx->ctx != NULL);

	/* From `man 3 evp_cipherupdate`:

	   EVP_EncryptUpdate() encrypts inl bytes from the buffer in and writes
	   the encrypted version to out. This function can be called multiple
	   times to encrypt successive blocks of data. The amount of data written
	   depends on the block alignment of the encrypted data: as a result the
	   amount of data written may be anything from zero bytes to
	   (inl + cipher_block_size - 1) so out should contain sufficient room.
	   The actual number of bytes written is placed in outl.
	 */

	buf = buffer_append_space_unsafe(result, data_len + block_size);
	outl = 0;
	if (EVP_CipherUpdate
		(ctx->ctx, buf, &outl, data, data_len) != 1)
		return dcrypt_openssl_error(error_r);
	buffer_set_used_size(result, buf_used + outl);
	return TRUE;
}

static
bool dcrypt_openssl_ctx_sym_final(struct dcrypt_context_symmetric *ctx, buffer_t *result, const char **error_r)
{
	const size_t block_size = (size_t)EVP_CIPHER_block_size(ctx->cipher);
	size_t buf_used = result->used;
	unsigned char *buf;
	int outl;
	int ec;

	i_assert(ctx->ctx != NULL);

	/* From `man 3 evp_cipherupdate`:

	   If padding is enabled (the default) then EVP_EncryptFinal_ex() encrypts
	   the "final" data, that is any data that remains in a partial block. It
	   uses standard block padding (aka PKCS padding). The encrypted final data
	   is written to out which should have sufficient space for one cipher
	   block. The number of bytes written is placed in outl. After this
	   function is called the encryption operation is finished and no further
	   calls to EVP_EncryptUpdate() should be made.
	 */

	buf = buffer_append_space_unsafe(result, block_size);
	outl = 0;

	/* when **DECRYPTING** set expected tag */
	if (ctx->mode == 0 && ctx->tag != NULL) {
		ec = EVP_CIPHER_CTX_ctrl(ctx->ctx, EVP_CTRL_GCM_SET_TAG, ctx->tag_len, ctx->tag);
	} else ec = 1;

	if (ec == 1)
		ec = EVP_CipherFinal_ex(ctx->ctx, buf, &outl);

	if (ec == 1) {
		buffer_set_used_size(result, buf_used + outl);
		/* when **ENCRYPTING** recover tag */
		if (ctx->mode == 1 && ctx->aad != NULL) {
			/* tag should be NULL here */
			i_assert(ctx->tag == NULL);
			/* openssl claims taglen is always 16, go figure .. */
			ctx->tag = p_malloc(ctx->pool, EVP_GCM_TLS_TAG_LEN);
			ec = EVP_CIPHER_CTX_ctrl(ctx->ctx, EVP_CTRL_GCM_GET_TAG, EVP_GCM_TLS_TAG_LEN, ctx->tag);
			ctx->tag_len = EVP_GCM_TLS_TAG_LEN;
		}
	}

	if (ec == 0 && error_r != NULL)
		*error_r = "data authentication failed";
	else if (ec < 0) dcrypt_openssl_error(error_r);

	EVP_CIPHER_CTX_free(ctx->ctx);
	ctx->ctx = NULL;

	return ec == 1;
}

static
bool dcrypt_openssl_ctx_hmac_create(const char *algorithm, struct dcrypt_context_hmac **ctx_r, const char **error_r)
{
	struct dcrypt_context_hmac *ctx;
	pool_t pool;
	const EVP_MD *md;
	md = EVP_get_digestbyname(algorithm);
	if(md == NULL) {
		if (error_r != NULL)
			*error_r = t_strdup_printf("Invalid digest %s", algorithm);
		return FALSE;
	}
	/* allocate context */
	pool = pool_alloconly_create("dcrypt openssl", 1024);
	ctx = p_new(pool, struct dcrypt_context_hmac, 1);
	ctx->pool = pool;
	ctx->md = md;
	*ctx_r = ctx;
	return TRUE;
}

static
void dcrypt_openssl_ctx_hmac_destroy(struct dcrypt_context_hmac **ctx)
{
	pool_t pool = (*ctx)->pool;
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	if ((*ctx)->ctx) HMAC_CTX_free((*ctx)->ctx);
#else
	HMAC_cleanup(&((*ctx)->ctx));
#endif
	pool_unref(&pool);
	*ctx = NULL;
}

static
void dcrypt_openssl_ctx_hmac_set_key(struct dcrypt_context_hmac *ctx, const unsigned char *key, size_t key_len)
{
	if(ctx->key != NULL) p_free(ctx->pool, ctx->key);
	ctx->klen = I_MIN(key_len, HMAC_MAX_MD_CBLOCK);
	ctx->key = p_malloc(ctx->pool, ctx->klen);
	memcpy(ctx->key, key, ctx->klen);
}
static
bool dcrypt_openssl_ctx_hmac_get_key(struct dcrypt_context_hmac *ctx, buffer_t *key)
{
	if(ctx->key == NULL) return FALSE;
	buffer_append(key, ctx->key, ctx->klen);
	return TRUE;
}
static
void dcrypt_openssl_ctx_hmac_set_key_random(struct dcrypt_context_hmac *ctx)
{
	ctx->klen = HMAC_MAX_MD_CBLOCK;
	ctx->key = p_malloc(ctx->pool, ctx->klen);
	random_fill(ctx->key, ctx->klen);
}

static
unsigned int dcrypt_openssl_ctx_hmac_get_digest_length(struct dcrypt_context_hmac *ctx)
{
	return EVP_MD_size(ctx->md);
}

static
bool dcrypt_openssl_ctx_hmac_init(struct dcrypt_context_hmac *ctx, const char **error_r)
{
	int ec;
	i_assert(ctx->md != NULL);
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	ctx->ctx = HMAC_CTX_new();
	if (ctx->ctx == NULL) return FALSE;
	ec = HMAC_Init_ex(ctx->ctx, ctx->key, ctx->klen, ctx->md, NULL);
#else
	HMAC_CTX_init(&ctx->ctx);
	ec = HMAC_Init_ex(&(ctx->ctx), ctx->key, ctx->klen, ctx->md, NULL);
#endif
	if (ec != 1) return dcrypt_openssl_error(error_r);
	return TRUE;
}
static
bool dcrypt_openssl_ctx_hmac_update(struct dcrypt_context_hmac *ctx, const unsigned char *data, size_t data_len, const char **error_r)
{
	int ec;
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	ec = HMAC_Update(ctx->ctx, data, data_len);
#else
	ec = HMAC_Update(&(ctx->ctx), data, data_len);
#endif
	if (ec != 1) return dcrypt_openssl_error(error_r);
	return TRUE;
}
static
bool dcrypt_openssl_ctx_hmac_final(struct dcrypt_context_hmac *ctx, buffer_t *result, const char **error_r)
{
	int ec;
	unsigned char buf[HMAC_MAX_MD_CBLOCK];
	unsigned int outl;
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	ec = HMAC_Final(ctx->ctx, buf, &outl);
	HMAC_CTX_free(ctx->ctx);
	ctx->ctx = NULL;
#else
	ec = HMAC_Final(&(ctx->ctx), buf, &outl);
	HMAC_cleanup(&(ctx->ctx));
#endif
	if (ec == 1) {
		buffer_append(result, buf, outl);
	} else return dcrypt_openssl_error(error_r);
	return TRUE;
}

static
bool dcrypt_openssl_generate_ec_key(int nid, EVP_PKEY **key, const char **error_r)
{
	EVP_PKEY_CTX *pctx;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *params = NULL;

	/* generate parameters for EC */
	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
	if (pctx == NULL ||
	    EVP_PKEY_paramgen_init(pctx) < 1 ||
	    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, nid) < 1 ||
	    EVP_PKEY_paramgen(pctx, &params) < 1)
	{
		dcrypt_openssl_error(error_r);
		EVP_PKEY_CTX_free(pctx);
		return FALSE;
	}

	/* generate key from parameters */
	ctx = EVP_PKEY_CTX_new(params, NULL);
	if (EVP_PKEY_keygen_init(ctx) < 1 ||
	    EVP_PKEY_keygen(ctx, key) < 1)
	{
		dcrypt_openssl_error(error_r);
		EVP_PKEY_free(params);
		EVP_PKEY_CTX_free(pctx);
		EVP_PKEY_CTX_free(ctx);
		return FALSE;
	}

	EVP_PKEY_free(params);
	EVP_PKEY_CTX_free(pctx);
	EVP_PKEY_CTX_free(ctx);
	EC_KEY_set_asn1_flag(EVP_PKEY_get0_EC_KEY((*key)), OPENSSL_EC_NAMED_CURVE);
	EC_KEY_set_conv_form(EVP_PKEY_get0_EC_KEY((*key)), POINT_CONVERSION_COMPRESSED);
	return TRUE;
}

static
bool dcrypt_openssl_generate_rsa_key(int bits, EVP_PKEY **key, const char **error_r)
{
	int ec = 0;

	EVP_PKEY_CTX *ctx;
	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	if (ctx == NULL ||
	    EVP_PKEY_keygen_init(ctx) < 1 ||
	    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) < 1 ||
	    EVP_PKEY_keygen(ctx, key) < 1) {
		dcrypt_openssl_error(error_r);
		ec = -1;
	}

	EVP_PKEY_CTX_free(ctx);
	return ec == 0;
}

static
bool dcrypt_openssl_ecdh_derive_secret_local(struct dcrypt_private_key *local_key, buffer_t *R, buffer_t *S, const char **error_r)
{
	EVP_PKEY *local = (EVP_PKEY*)local_key;
	BN_CTX *bn_ctx = BN_CTX_new();
	const EC_GROUP *grp = EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(local));
	EC_POINT *pub = EC_POINT_new(grp);
	/* convert ephemeral key data EC point */
	if (EC_POINT_oct2point(grp, pub, R->data, R->used, bn_ctx) != 1)
	{
		EC_POINT_free(pub);
		BN_CTX_free(bn_ctx);
		return dcrypt_openssl_error(error_r);
	}
	EC_KEY *ec_key = EC_KEY_new();
	/* convert point to public key */
	EC_KEY_set_conv_form(ec_key, POINT_CONVERSION_COMPRESSED);
	EC_KEY_set_group(ec_key, grp);
	EC_KEY_set_public_key(ec_key, pub);
	EC_POINT_free(pub);
	BN_CTX_free(bn_ctx);

	/* make sure it looks like a valid key */
	if (EC_KEY_check_key(ec_key) != 1) {
		EC_KEY_free(ec_key);
		return dcrypt_openssl_error(error_r);
	}

	EVP_PKEY *peer = EVP_PKEY_new();
	EVP_PKEY_set1_EC_KEY(peer, ec_key);
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(local, NULL);

	/* initialize derivation */
	if (pctx == NULL ||
	    EVP_PKEY_derive_init(pctx) != 1 ||
	    EVP_PKEY_derive_set_peer(pctx, peer) != 1) {
		EVP_PKEY_CTX_free(pctx);
		EC_KEY_free(ec_key);
		return dcrypt_openssl_error(error_r);
	}

	/* have to do it twice to get the data length */
	size_t len;
	if (EVP_PKEY_derive(pctx, NULL, &len) != 1) {
		EVP_PKEY_CTX_free(pctx);
		EC_KEY_free(ec_key);
		return dcrypt_openssl_error(error_r);
	}
	unsigned char buf[len];
	memset(buf,0,len);
	if (EVP_PKEY_derive(pctx, buf, &len) != 1) {
		EVP_PKEY_CTX_free(pctx);
		EC_KEY_free(ec_key);
		return dcrypt_openssl_error(error_r);
	}
	EVP_PKEY_CTX_free(pctx);
	buffer_append(S, buf, len);
	EC_KEY_free(ec_key);
	EVP_PKEY_free(peer);
	return TRUE;
}

static
bool dcrypt_openssl_ecdh_derive_secret_peer(struct dcrypt_public_key *peer_key, buffer_t *R, buffer_t *S, const char **error_r)
{
	/* ensure peer_key is EC key */
	EVP_PKEY *local = NULL;
	EVP_PKEY *peer = (EVP_PKEY*)peer_key;
	if (EVP_PKEY_base_id(peer) != EVP_PKEY_EC) {
		if (error_r != NULL)
			*error_r = "Only ECC key can be used";
		return FALSE;
	}

	/* generate another key from same group */
	int nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(peer)));
	if (!dcrypt_openssl_generate_ec_key(nid, &local, error_r)) return FALSE;

	/* initialize */
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(local, NULL);
	if (pctx == NULL ||
	    EVP_PKEY_derive_init(pctx) != 1 ||
	    EVP_PKEY_derive_set_peer(pctx, peer) != 1) {
		EVP_PKEY_CTX_free(pctx);
		return dcrypt_openssl_error(error_r);
	}

	/* derive */
	size_t len;
	if (EVP_PKEY_derive(pctx, NULL, &len) != 1) {
		EVP_PKEY_CTX_free(pctx);
		return dcrypt_openssl_error(error_r);
	}
	unsigned char buf[len];
	if (EVP_PKEY_derive(pctx, buf, &len) != 1) {
		EVP_PKEY_CTX_free(pctx);
		return dcrypt_openssl_error(error_r);
	}

	EVP_PKEY_CTX_free(pctx);
	buffer_append(S, buf, len);

	/* get ephemeral key (=R) */
	BN_CTX *bn_ctx = BN_CTX_new();
	const EC_POINT *pub = EC_KEY_get0_public_key(EVP_PKEY_get0_EC_KEY(local));
	const EC_GROUP *grp = EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(local));
	len = EC_POINT_point2oct(grp, pub, POINT_CONVERSION_COMPRESSED, NULL, 0, bn_ctx);
	unsigned char R_buf[len];
	EC_POINT_point2oct(grp, pub, POINT_CONVERSION_COMPRESSED, R_buf, len, bn_ctx);
	BN_CTX_free(bn_ctx);
	buffer_append(R, R_buf, len);
	EVP_PKEY_free(local);

	return TRUE;
}

static
bool dcrypt_openssl_pbkdf2(const unsigned char *password, size_t password_len, const unsigned char *salt, size_t salt_len,
	const char *hash, unsigned int rounds, buffer_t *result, unsigned int result_len, const char **error_r)
{
	int ret;
	i_assert(rounds > 0);
	i_assert(result_len > 0);
	i_assert(result != NULL);
	T_BEGIN {
		/* determine MD */
		const EVP_MD* md = EVP_get_digestbyname(hash);
		if (md == NULL) {
			if (error_r != NULL)
				*error_r = t_strdup_printf("Invalid digest %s", hash);
			return FALSE;
		}

		unsigned char buffer[result_len];
		if ((ret = PKCS5_PBKDF2_HMAC((const char*)password, password_len, salt, salt_len, rounds,
						md, result_len, buffer)) == 1) {
			buffer_append(result, buffer, result_len);
		}
	} T_END;
	if (ret != 1) return dcrypt_openssl_error(error_r);
	return TRUE;
}

static
bool dcrypt_openssl_generate_keypair(struct dcrypt_keypair *pair_r, enum dcrypt_key_type kind, unsigned int bits, const char *curve, const char **error_r)
{
	EVP_PKEY *pkey = NULL;
	if (kind == DCRYPT_KEY_RSA) {
		if (dcrypt_openssl_generate_rsa_key(bits, &pkey, error_r) == 0) {
			pair_r->priv = (struct dcrypt_private_key*)pkey;
			return dcrypt_openssl_private_to_public_key(pair_r->priv, &(pair_r->pub), error_r);
		} else return dcrypt_openssl_error(error_r);
	} else if (kind == DCRYPT_KEY_EC) {
		int nid = OBJ_sn2nid(curve);
		if (nid == NID_undef) {
			if (error_r != NULL)
				*error_r = t_strdup_printf("Unknown EC curve %s", curve);
			return FALSE;
		}
		if (dcrypt_openssl_generate_ec_key(nid, &pkey, error_r) == 0) {
			pair_r->priv = (struct dcrypt_private_key*)pkey;
			return dcrypt_openssl_private_to_public_key(pair_r->priv, &(pair_r->pub), error_r);
		} else return dcrypt_openssl_error(error_r);
	}
	if (error_r != NULL)
		*error_r = "Key type not supported in this build";
	return FALSE;
}

static
bool dcrypt_openssl_decrypt_point_v1(buffer_t *data, buffer_t *key, BIGNUM **point_r, const char **error_r)
{
	struct dcrypt_context_symmetric *dctx;
	buffer_t *tmp = buffer_create_dynamic(pool_datastack_create(), 64);

	if (!dcrypt_openssl_ctx_sym_create("aes-256-ctr", DCRYPT_MODE_DECRYPT, &dctx, error_r)) {
		return FALSE;
	}

	/* v1 KEYS have all-zero IV - have to use it ourselves too */
	dcrypt_openssl_ctx_sym_set_iv(dctx, (const unsigned char*)"\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0", 16);
	dcrypt_openssl_ctx_sym_set_key(dctx, key->data, key->used);

	if (!dcrypt_openssl_ctx_sym_init(dctx, error_r) ||
	    !dcrypt_openssl_ctx_sym_update(dctx, data->data, data->used, tmp, error_r) ||
	    !dcrypt_openssl_ctx_sym_final(dctx, tmp, error_r)) {
		dcrypt_openssl_ctx_sym_destroy(&dctx);
		return FALSE;
	}

	dcrypt_openssl_ctx_sym_destroy(&dctx);

	*point_r = BN_bin2bn(tmp->data, tmp->used, NULL);
	safe_memset(buffer_get_modifiable_data(tmp, NULL), 0,tmp->used);
	buffer_set_used_size(key, 0);

	if (*point_r == FALSE)
		return dcrypt_openssl_error(error_r);

	return TRUE;
}

static
bool dcrypt_openssl_decrypt_point_ec_v1(struct dcrypt_private_key *dec_key,
	const char *data_hex, const char *peer_key_hex, BIGNUM **point_r, const char **error_r)
{
	buffer_t *peer_key, *data, key, *secret;
	bool res;

	data = buffer_create_dynamic(pool_datastack_create(), 128);
	peer_key = buffer_create_dynamic(pool_datastack_create(), 64);

	hex_to_binary(data_hex, data);
	hex_to_binary(peer_key_hex, peer_key);

	secret = buffer_create_dynamic(pool_datastack_create(), 64);

	if (!dcrypt_openssl_ecdh_derive_secret_local(dec_key, peer_key, secret, error_r))
		return FALSE;

	/* run it thru SHA256 once */
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(secret->data, secret->used, digest);
	safe_memset(buffer_get_modifiable_data(secret, NULL), 0, secret->used);
	buffer_set_used_size(secret, 0);
	buffer_create_from_const_data(&key, digest, SHA256_DIGEST_LENGTH);

	/* then use this as key */
	res = dcrypt_openssl_decrypt_point_v1(data, &key, point_r, error_r);
	memset(digest, 0, sizeof(digest));
	safe_memset(digest, 0, SHA256_DIGEST_LENGTH);

	return res;
}

static
bool dcrypt_openssl_decrypt_point_password_v1(const char *data_hex, const char *password_hex,
	const char *salt_hex, BIGNUM **point_r, const char **error_r)
{
	buffer_t *salt, *data, *password, *key;
	struct dcrypt_context_symmetric *dctx;

	data = buffer_create_dynamic(pool_datastack_create(), 128);
	salt = buffer_create_dynamic(pool_datastack_create(), 16);
	password = buffer_create_dynamic(pool_datastack_create(), 32);
	key = buffer_create_dynamic(pool_datastack_create(), 32);

	hex_to_binary(data_hex, data);
	hex_to_binary(salt_hex, salt);
	hex_to_binary(password_hex, password);

	/* aes-256-ctr uses 32 byte key, and v1 uses all-zero IV */
	if (!dcrypt_openssl_pbkdf2(password->data, password->used, salt->data, salt->used,
				   "sha256", 16, key, 32, error_r)) {
		dcrypt_ctx_sym_destroy(&dctx);
		return FALSE;
	}

	return dcrypt_openssl_decrypt_point_v1(data, key, point_r, error_r);
}

static
bool dcrypt_openssl_load_private_key_dovecot_v1(struct dcrypt_private_key **key_r,
	int len, const char **input, const char *password, struct dcrypt_private_key *dec_key,
	const char **error_r)
{
	int nid, ec, enctype;
	EC_KEY *eckey = NULL;
	BIGNUM *point = NULL;

	if (str_to_int(input[1], &nid) != 0) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return FALSE;
	}

	if (str_to_int(input[2], &enctype) != 0) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return FALSE;
	}

	eckey = EC_KEY_new_by_curve_name(nid);
	if (eckey == NULL) return dcrypt_openssl_error(error_r);

	/* decode and optionally decipher private key value */
	if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_NONE) {
		point = BN_new();
		if (BN_hex2bn(&point, input[3]) < 1) {
			BN_free(point);
			EC_KEY_free(eckey);
			return dcrypt_openssl_error(error_r);
		}
	} else if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PASSWORD) {
		/* by password */
		const char *enc_priv_pt = input[3];
		const char *salt = input[4];
		if (!dcrypt_openssl_decrypt_point_password_v1(enc_priv_pt, password, salt, &point, error_r)) {
			EC_KEY_free(eckey);
			return FALSE;
		}
	} else if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PK) {
		/* by key */
		const char *enc_priv_pt = input[3];
		const char *peer_key = input[4];
		if (!dcrypt_openssl_decrypt_point_ec_v1(dec_key, enc_priv_pt, peer_key, &point, error_r)) {
			EC_KEY_free(eckey);
			return FALSE;
		}
	} else {
		if (error_r != NULL)
			*error_r = "Invalid key data";
		EC_KEY_free(eckey);
		return FALSE;
	}

	/* assign private key */
	BN_CTX *bnctx = BN_CTX_new();
	EC_KEY_set_conv_form(eckey, POINT_CONVERSION_COMPRESSED);
	EC_KEY_set_private_key(eckey, point);
	EC_KEY_precompute_mult(eckey, bnctx);
	EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);
	EC_POINT *pub = EC_POINT_new(EC_KEY_get0_group(eckey));
	/* calculate public key */
	ec = EC_POINT_mul(EC_KEY_get0_group(eckey), pub, point, NULL, NULL, bnctx);
	EC_KEY_set_public_key(eckey, pub);
	BN_free(point);
	EC_POINT_free(pub);
	BN_CTX_free(bnctx);

	/* make sure it looks OK and is correct */
	if (ec == 1 && EC_KEY_check_key(eckey) == 1) {
		unsigned char digest[SHA256_DIGEST_LENGTH];
		/* validate that the key was loaded correctly */
		char *id = ec_key_get_pub_point_hex(eckey);
		SHA256((unsigned char*)id, strlen(id), digest);
		OPENSSL_free(id);
		const char *digest_hex = binary_to_hex(digest, SHA256_DIGEST_LENGTH);
		if (strcmp(digest_hex, input[len-1]) != 0) {
			if (error_r != NULL)
				*error_r = "Key id mismatch after load";
			EC_KEY_free(eckey);
			return FALSE;
		}
		EVP_PKEY *key = EVP_PKEY_new();
		EVP_PKEY_set1_EC_KEY(key, eckey);
		EC_KEY_free(eckey);
		*key_r = (struct dcrypt_private_key *)key;
		return TRUE;
	}

	EC_KEY_free(eckey);

	return dcrypt_openssl_error(error_r);
}

/* encrypt/decrypt private keys */
static
bool dcrypt_openssl_cipher_key_dovecot_v2(const char *cipher, enum dcrypt_sym_mode mode,
	buffer_t *input, buffer_t *secret, buffer_t *salt, const char *digalgo, unsigned int rounds,
	buffer_t *result_r, const char **error_r)
{
	struct dcrypt_context_symmetric *dctx;
	bool res;

	if (!dcrypt_openssl_ctx_sym_create(cipher, mode, &dctx, error_r)) {
		return FALSE;
	}

	/* generate encryption key/iv based on secret/salt */
	buffer_t *key_data = buffer_create_dynamic(pool_datastack_create(), 128);
	res = dcrypt_openssl_pbkdf2(secret->data, secret->used, salt->data, salt->used,
		digalgo, rounds, key_data,
		dcrypt_openssl_ctx_sym_get_key_length(dctx)+dcrypt_openssl_ctx_sym_get_iv_length(dctx), error_r);

	if (!res) {
		dcrypt_openssl_ctx_sym_destroy(&dctx);
		return FALSE;
	}

	buffer_t *tmp = buffer_create_dynamic(pool_datastack_create(), 128);
	const unsigned char *kd = buffer_free_without_data(&key_data);

	/* perform ciphering */
	dcrypt_openssl_ctx_sym_set_key(dctx, kd, dcrypt_openssl_ctx_sym_get_key_length(dctx));
	dcrypt_openssl_ctx_sym_set_iv(dctx, kd+dcrypt_openssl_ctx_sym_get_key_length(dctx), dcrypt_openssl_ctx_sym_get_iv_length(dctx));

	if (!dcrypt_openssl_ctx_sym_init(dctx, error_r) ||
	    !dcrypt_openssl_ctx_sym_update(dctx, input->data, input->used, tmp, error_r) ||
	    !dcrypt_openssl_ctx_sym_final(dctx, tmp, error_r)) {
		res = FALSE;
	} else {
		/* provide result if succeeded */
		buffer_append_buf(result_r, tmp, 0, (size_t)-1);
		res = TRUE;
	}
	/* and ensure no data leaks */
	safe_memset(buffer_get_modifiable_data(tmp, NULL), 0, tmp->used);

	dcrypt_openssl_ctx_sym_destroy(&dctx);
	return res;
}

static
bool dcrypt_openssl_load_private_key_dovecot_v2(struct dcrypt_private_key **key_r,
	int len, const char **input, const char *password, struct dcrypt_private_key *dec_key,
	const char **error_r)
{
	int enctype;
	buffer_t *key_data = buffer_create_dynamic(pool_datastack_create(), 256);

	/* check for encryption type */
	if (str_to_int(input[2], &enctype) != 0) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return FALSE;
	}

	if (enctype < 0 || enctype > 2) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return FALSE;
	}

	/* match encryption type to field counts */
	if ((enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_NONE && len != 5) ||
	    (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PASSWORD && len != 9) ||
 	    (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PK && len != 11)) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return FALSE;
	}

	/* get key type */
	int nid = OBJ_txt2nid(input[1]);

	if (nid == NID_undef)
		return dcrypt_openssl_error(error_r);

	/* decode and possibly decipher private key value */
	if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_NONE) {
		if (hex_to_binary(input[3], key_data) != 0) {
			if (error_r != NULL)
				*error_r = "Corrupted data";
		}
	} else if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PK) {
		unsigned int rounds;
		struct dcrypt_public_key *pubkey = NULL;
		if (str_to_uint(input[6], &rounds) != 0) {
			if (error_r != NULL)
				*error_r = "Corrupted data";
			return FALSE;
		}

		buffer_t *data = buffer_create_dynamic(pool_datastack_create(), 128);

		/* check that we have correct decryption key */
		if (!dcrypt_openssl_private_to_public_key(dec_key, &pubkey, error_r) ||
		    !dcrypt_openssl_public_key_id(pubkey, "sha256", data, error_r)) {
			if (pubkey != NULL) dcrypt_openssl_free_public_key(&pubkey);
			return FALSE;
		}

		dcrypt_openssl_free_public_key(&pubkey);

		if (strcmp(binary_to_hex(data->data, data->used), input[9]) != 0) {
			dcrypt_openssl_free_public_key(&pubkey);
			if (error_r != NULL)
				*error_r = "No private key available";
			return FALSE;
		}


		buffer_t *salt, *peer_key, *secret;
		salt = buffer_create_dynamic(pool_datastack_create(), strlen(input[4])/2);
		peer_key = buffer_create_dynamic(pool_datastack_create(), strlen(input[8])/2);
		secret = buffer_create_dynamic(pool_datastack_create(), 128);

		buffer_set_used_size(data, 0);
		hex_to_binary(input[4], salt);
		hex_to_binary(input[8], peer_key);
		hex_to_binary(input[7], data);

		/* get us secret value to use for key/iv generation */
		if (EVP_PKEY_base_id((EVP_PKEY*)dec_key) == EVP_PKEY_RSA) {
			if (!dcrypt_openssl_rsa_decrypt(dec_key, peer_key->data, peer_key->used, secret, error_r))
				return FALSE;
		} else {
			/* perform ECDH */
			if (!dcrypt_openssl_ecdh_derive_secret_local(dec_key, peer_key, secret, error_r))
				return FALSE;
		}
		/* decrypt key */
		if (!dcrypt_openssl_cipher_key_dovecot_v2(input[3], DCRYPT_MODE_DECRYPT, data, secret, salt,
		    input[5], rounds, key_data, error_r)) {
			return FALSE;
		}
	} else if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PASSWORD) {
		unsigned int rounds;
		if (str_to_uint(input[6], &rounds) != 0) {
			if (error_r != NULL)
				*error_r = "Corrupted data";
			return FALSE;
		}

		buffer_t *salt, secret, *data;
		salt = buffer_create_dynamic(pool_datastack_create(), strlen(input[4])/2);
		buffer_create_from_const_data(&secret, password, strlen(password));
		data = buffer_create_dynamic(pool_datastack_create(), strlen(input[7])/2);
		if (hex_to_binary(input[4], salt) != 0 ||
		    hex_to_binary(input[7], data) != 0) {
			if (error_r != NULL)
				*error_r = "Corrupted data";
			return FALSE;
		}

		if (!dcrypt_openssl_cipher_key_dovecot_v2(input[3], DCRYPT_MODE_DECRYPT, data, &secret, salt,
		    input[5], rounds, key_data, error_r)) {
			return FALSE;
		}
	}

	/* decode actual key */
	if (EVP_PKEY_type(nid) == EVP_PKEY_RSA) {
		RSA *rsa = RSA_new();
		const unsigned char *ptr = buffer_get_data(key_data, NULL);
		if (d2i_RSAPrivateKey(&rsa, &ptr, key_data->used) == NULL ||
		    RSA_check_key(rsa) != 1) {
			safe_memset(buffer_get_modifiable_data(key_data, NULL), 0, key_data->used);
			RSA_free(rsa);
			return dcrypt_openssl_error(error_r);
		}
		safe_memset(buffer_get_modifiable_data(key_data, NULL), 0, key_data->used);
		buffer_set_used_size(key_data, 0);
		EVP_PKEY *pkey = EVP_PKEY_new();
		EVP_PKEY_set1_RSA(pkey, rsa);
		*key_r = (struct dcrypt_private_key *)pkey;
	} else {
		int ec;
		BIGNUM *point = BN_new();
		if (BN_mpi2bn(key_data->data, key_data->used, point) == NULL) {
			safe_memset(buffer_get_modifiable_data(key_data, NULL), 0, key_data->used);
			BN_free(point);
			return dcrypt_openssl_error(error_r);
		}
		EC_KEY *eckey = EC_KEY_new_by_curve_name(nid);
		safe_memset(buffer_get_modifiable_data(key_data, NULL), 0, key_data->used);
		buffer_set_used_size(key_data, 0);
		if (eckey == NULL) {
			return dcrypt_openssl_error(error_r);
		}
		BN_CTX *bnctx = BN_CTX_new();
		EC_KEY_set_conv_form(eckey, POINT_CONVERSION_COMPRESSED);
		EC_KEY_set_private_key(eckey, point);
		EC_KEY_precompute_mult(eckey, bnctx);
		EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);
		EC_POINT *pub = EC_POINT_new(EC_KEY_get0_group(eckey));
		/* calculate public key */
		ec = EC_POINT_mul(EC_KEY_get0_group(eckey), pub, point, NULL, NULL, bnctx);
		EC_KEY_set_public_key(eckey, pub);
		BN_free(point);
		EC_POINT_free(pub);
		BN_CTX_free(bnctx);
		/* make sure the EC key is valid */
		if (ec == 1 && EC_KEY_check_key(eckey) == 1) {
			EVP_PKEY *key = EVP_PKEY_new();
			EVP_PKEY_set1_EC_KEY(key, eckey);
			EC_KEY_free(eckey);
			*key_r = (struct dcrypt_private_key *)key;
		} else {
			EC_KEY_free(eckey);
			return dcrypt_openssl_error(error_r);
		}
	}

	/* finally compare key to key id */
	struct dcrypt_public_key *pubkey = NULL;
	dcrypt_openssl_private_to_public_key(*key_r, &pubkey, NULL);
	dcrypt_openssl_public_key_id(pubkey, "sha256", key_data, NULL);
	dcrypt_openssl_free_public_key(&pubkey);

	if (strcmp(binary_to_hex(key_data->data, key_data->used), input[len-1]) != 0) {
		dcrypt_openssl_free_private_key(key_r);
		if (error_r != NULL)
			*error_r = "Key id mismatch after load";
		return FALSE;
	}

	return TRUE;
}


static
bool dcrypt_openssl_load_private_key_dovecot(struct dcrypt_private_key **key_r,
	const char *data, const char *password, struct dcrypt_private_key *key,
	const char **error_r)
{
	bool ret;
	T_BEGIN {
		const char **input = t_strsplit_tab(data);
		size_t len;
		for(len=0;input[len]!=NULL;len++);
		if (len < 4) {
			if (error_r != NULL)
				*error_r = "Corrupted data";
			ret = FALSE;
		} else if (*(input[0])== '1')
			ret = dcrypt_openssl_load_private_key_dovecot_v1(key_r, len, input, password, key, error_r);
		else if (*(input[0])== '2')
			ret = dcrypt_openssl_load_private_key_dovecot_v2(key_r, len, input, password, key, error_r);
		else {
			if (error_r != NULL)
				*error_r = "Unsupported key version";
			ret = FALSE;
		}
	} T_END;
	return ret;
}

static
int dcrypt_openssl_load_public_key_dovecot_v1(struct dcrypt_public_key **key_r,
	int len, const char **input, const char **error_r)
{
	int nid;
	if (len != 3) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return -1;
	}
	if (str_to_int(input[1], &nid) != 0) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return -1;
	}

	EC_KEY *eckey = EC_KEY_new_by_curve_name(nid);
	if (eckey == NULL) {
		dcrypt_openssl_error(error_r);
		return -1;
	}

	EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);
	BN_CTX *bnctx = BN_CTX_new();

	EC_POINT *point = EC_POINT_new(EC_KEY_get0_group(eckey));
	if (EC_POINT_hex2point(EC_KEY_get0_group(eckey),
 	    input[2], point, bnctx) == NULL) {
		BN_CTX_free(bnctx);
		EC_KEY_free(eckey);
		EC_POINT_free(point);
		dcrypt_openssl_error(error_r);
		return -1;
	}
	BN_CTX_free(bnctx);

	EC_KEY_set_public_key(eckey, point);
	EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

	EC_POINT_free(point);

	if (EC_KEY_check_key(eckey) == 1) {
		EVP_PKEY *key = EVP_PKEY_new();
		EVP_PKEY_set1_EC_KEY(key, eckey);
		*key_r = (struct dcrypt_public_key *)key;
		return 0;
	}

	dcrypt_openssl_error(error_r);
	return -1;
}

static
bool dcrypt_openssl_load_public_key_dovecot_v2(struct dcrypt_public_key **key_r,
	int len, const char **input, const char **error_r)
{
	if (len != 2 || strlen(input[1]) < 2 || (strlen(input[1])%2) != 0) {
		if (error_r != NULL)
			*error_r = "Corrupted data";
		return -1;
	}
	buffer_t tmp;
	size_t keylen = strlen(input[1])/2;
	unsigned char keybuf[keylen];
	buffer_create_from_data(&tmp, keybuf, keylen);
	hex_to_binary(input[1], &tmp);

	EVP_PKEY *pkey = EVP_PKEY_new();
	if (d2i_PUBKEY(&pkey, (const unsigned char**)(tmp.data), tmp.used)==NULL) {
		EVP_PKEY_free(pkey);
		dcrypt_openssl_error(error_r);
		return -1;
	}

	*key_r = (struct dcrypt_public_key *)pkey;
	return 0;
}

static
bool dcrypt_openssl_load_public_key_dovecot(struct dcrypt_public_key **key_r,
	const char *data, const char **error_r)
{
	int ec = 0;

	T_BEGIN {
		const char **input = t_strsplit_tab(data);
		size_t len;
		for(len=0;input[len]!=NULL;len++);
		if (len < 2) ec = -1;
		if (ec == 0 && *(input[0]) == '1') {
			ec = dcrypt_openssl_load_public_key_dovecot_v1(key_r, len,
				input, error_r);
		} else if (ec == 0 && *(input[0]) == '2') {
			ec = dcrypt_openssl_load_public_key_dovecot_v2(key_r, len,
				input, error_r);
		} else {
			if (error_r != NULL)
				*error_r = "Unsupported key version";
			ec = -1;
		}
	} T_END;

	return (ec == 0 ? TRUE : FALSE);
}

static
bool dcrypt_openssl_encrypt_private_key_dovecot(buffer_t *key, int enctype, const char *cipher, const char *password,
	struct dcrypt_public_key *enc_key, buffer_t *destination, const char **error_r)
{
	bool res;
	unsigned char *ptr;

	unsigned char salt[8];
	buffer_t *peer_key = buffer_create_dynamic(pool_datastack_create(), 128);
	buffer_t *secret = buffer_create_dynamic(pool_datastack_create(), 128);
	cipher = t_str_lcase(cipher);

	str_append(destination, cipher);
	str_append_c(destination, '\t');
	random_fill(salt, sizeof(salt));
	binary_to_hex_append(destination, salt, sizeof(salt));
	buffer_t saltbuf;
	buffer_create_from_const_data(&saltbuf, salt, sizeof(salt));

	/* so we don't have to make new version if we ever upgrade these */
	str_append(destination, t_strdup_printf("\t%s\t%d\t",
		DCRYPT_DOVECOT_KEY_ENCRYPT_HASH,
		DCRYPT_DOVECOT_KEY_ENCRYPT_ROUNDS));

	if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PK) {
		if (EVP_PKEY_base_id((EVP_PKEY*)enc_key) == EVP_PKEY_RSA) {
			size_t used = buffer_get_used_size(secret);
			/* peer key, in this case, is encrypted secret, which is 16 bytes of data */
			ptr = buffer_append_space_unsafe(secret, 16);
			random_fill(ptr, 16);
			buffer_set_used_size(secret, used+16);
			if (!dcrypt_rsa_encrypt(enc_key, secret->data, secret->used, peer_key, error_r)) {
				return FALSE;
			}
		} else if (EVP_PKEY_base_id((EVP_PKEY*)enc_key) == EVP_PKEY_EC) {
			/* generate secret by ECDHE */
			if (!dcrypt_openssl_ecdh_derive_secret_peer(enc_key, peer_key, secret, error_r)) {
				return FALSE;
			}
		} else {
			if (error_r != NULL)
				*error_r = "Unsupported encryption key";
			return FALSE;
		}
		/* add encryption key id, reuse peer_key buffer */
	} else if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PASSWORD) {
		str_append(secret, password);
	}

	/* encrypt key using secret and salt */
	buffer_t *tmp = buffer_create_dynamic(pool_datastack_create(), 128);
	res = dcrypt_openssl_cipher_key_dovecot_v2(cipher, DCRYPT_MODE_ENCRYPT, key, secret, &saltbuf,
		DCRYPT_DOVECOT_KEY_ENCRYPT_HASH, DCRYPT_DOVECOT_KEY_ENCRYPT_ROUNDS, tmp, error_r);
	safe_memset(buffer_get_modifiable_data(secret, NULL), 0, secret->used);
	binary_to_hex_append(destination, tmp->data, tmp->used);

	/* some additional fields or private key version */
	if (enctype == DCRYPT_DOVECOT_KEY_ENCRYPT_PK) {
		str_append_c(destination, '\t');

		/* for RSA, this is the actual encrypted secret */
		binary_to_hex_append(destination, peer_key->data, peer_key->used);
		str_append_c(destination, '\t');

		buffer_set_used_size(peer_key, 0);
		if (!dcrypt_openssl_public_key_id(enc_key, "sha256", peer_key, error_r))
			return FALSE;
		binary_to_hex_append(destination, peer_key->data, peer_key->used);
	}
	return res;
}

static
bool dcrypt_openssl_store_private_key_dovecot(struct dcrypt_private_key *key, const char *cipher, buffer_t *destination,
	const char *password, struct dcrypt_public_key *enc_key, const char **error_r)
{
	size_t dest_used = buffer_get_used_size(destination);
	const char *cipher2 = NULL;
	EVP_PKEY *pkey = (EVP_PKEY*)key;
	char objtxt[80]; /* openssl manual says this is OK */
	ASN1_OBJECT *obj;
	if (EVP_PKEY_base_id(pkey) == EVP_PKEY_EC) {
		/* because otherwise we get wrong nid */
		obj = OBJ_nid2obj(EC_GROUP_get_curve_name(EC_KEY_get0_group(EVP_PKEY_get0_EC_KEY(pkey))));

	} else {
		obj = OBJ_nid2obj(EVP_PKEY_id(pkey));
	}

	int enctype = 0;
	int ln = OBJ_obj2txt(objtxt, sizeof(objtxt), obj, 1);
	if (ln < 1)
		return dcrypt_openssl_error(error_r);
	if (ln > (int)sizeof(objtxt)) {
		if (error_r != NULL)
			*error_r = "Object identifier too long";
		return FALSE;
	}

	buffer_t *buf = buffer_create_dynamic(pool_datastack_create(), 256);

	/* convert key to private key value */
	if (EVP_PKEY_base_id(pkey) == EVP_PKEY_RSA) {
		unsigned char *ptr;
		RSA *rsa = EVP_PKEY_get0_RSA(pkey);
		int ln = i2d_RSAPrivateKey(rsa, &ptr);
		if (ln < 1)
			return dcrypt_openssl_error(error_r);
		buffer_append(buf, ptr, ln);
	} else if (EVP_PKEY_base_id(pkey) == EVP_PKEY_EC) {
		unsigned char *ptr;
		EC_KEY *eckey = EVP_PKEY_get0_EC_KEY(pkey);
		const BIGNUM *pk = EC_KEY_get0_private_key(eckey);
		/* serialize to MPI which is portable */
		int len = BN_bn2mpi(pk, NULL);
		ptr = buffer_append_space_unsafe(buf, len);
		BN_bn2mpi(pk, ptr);
	} else {
		if (*error_r != NULL)
			*error_r = "Unsupported key type";
		return FALSE;
	}

	/* see if we want ECDH based or password based encryption */
	if (cipher != NULL && strncasecmp(cipher, "ecdh-", 5) == 0) {
		i_assert(enc_key != NULL);
		i_assert(password == NULL);
		enctype = DCRYPT_DOVECOT_KEY_ENCRYPT_PK;
		cipher2 = cipher+5;
	} else if (cipher != NULL) {
		i_assert(enc_key == NULL);
		i_assert(password != NULL);
		enctype = DCRYPT_DOVECOT_KEY_ENCRYPT_PASSWORD;
		cipher2 = cipher;
	}

	/* put in OID and encryption type */
	str_append(destination, t_strdup_printf("2\t%s\t%d\t",
		objtxt, enctype));

	/* perform encryption if desired */
	if (enctype > 0) {
		if (!dcrypt_openssl_encrypt_private_key_dovecot(buf, enctype, cipher2, password, enc_key, destination, error_r)) {
			buffer_set_used_size(destination, dest_used);
			return FALSE;
		}
	} else {
		binary_to_hex_append(destination, buf->data, buf->used);
	}

	/* append public key id */
	struct dcrypt_public_key *pubkey = NULL;
	if (!dcrypt_openssl_private_to_public_key(key, &pubkey, error_r)) {
		buffer_set_used_size(destination, dest_used);
		return FALSE;
	}

	str_append_c(destination, '\t');
	buffer_set_used_size(buf, 0);
	bool res = dcrypt_openssl_public_key_id(pubkey, "sha256", buf, error_r);
	dcrypt_openssl_free_public_key(&pubkey);
	binary_to_hex_append(destination, buf->data, buf->used);

	if (!res) {
		/* well, that didn't end well */
		buffer_set_used_size(destination, dest_used);
		return FALSE;
	}
	return TRUE;
}

static
bool dcrypt_openssl_store_public_key_dovecot(struct dcrypt_public_key *key, buffer_t *destination, const char **error_r)
{
	EVP_PKEY *pubkey = (EVP_PKEY*)key;
	unsigned char *tmp = NULL;

	int rv = i2d_PUBKEY(pubkey, &tmp);

	if (tmp == NULL)
		return dcrypt_openssl_error(error_r);
	/* then store it */
	str_append_c(destination, '2');
	str_append_c(destination, '\t');
	binary_to_hex_append(destination, tmp, rv);
	OPENSSL_free(tmp);

	return TRUE;
}

static
bool dcrypt_openssl_load_private_key(struct dcrypt_private_key **key_r, enum dcrypt_key_format format,
	const char *data, const char *password, struct dcrypt_private_key *dec_key,
	const char **error_r)
{
	EVP_PKEY *key = NULL, *key2;
	if (format == DCRYPT_FORMAT_DOVECOT)
		return dcrypt_openssl_load_private_key_dovecot(key_r, data, password, dec_key, error_r);

	BIO *key_in = BIO_new_mem_buf((void*)data, strlen(data));

	key = EVP_PKEY_new();

	key2 = PEM_read_bio_PrivateKey(key_in, &key, NULL, (void*)password);

	BIO_vfree(key_in);

	if (key2 == NULL) {
		EVP_PKEY_free(key);
		return dcrypt_openssl_error(error_r);
	}

	if (EVP_PKEY_base_id(key) == EVP_PKEY_EC) {
		EC_KEY_set_conv_form(EVP_PKEY_get0_EC_KEY(key), POINT_CONVERSION_COMPRESSED);
	}

	*key_r = (struct dcrypt_private_key *)key;

	return TRUE;
}

static
bool dcrypt_openssl_load_public_key(struct dcrypt_public_key **key_r, enum dcrypt_key_format format,
	const char *data, const char **error_r)
{
	EVP_PKEY *key = NULL;
	if (format == DCRYPT_FORMAT_DOVECOT)
		return dcrypt_openssl_load_public_key_dovecot(key_r, data, error_r);

	BIO *key_in = BIO_new_mem_buf((void*)data, strlen(data));

	key = PEM_read_bio_PUBKEY(key_in, &key, NULL, NULL);
	(void)BIO_reset(key_in);
	if (key == NULL) { /* ec keys are bother */
		/* read the header */
		char buf[27]; /* begin public key */
		if (BIO_gets(key_in, buf, sizeof(buf)) != 1) {
			BIO_vfree(key_in);
			return dcrypt_openssl_error(error_r);
		}
		if (strcmp(buf, "-----BEGIN PUBLIC KEY-----") != 0) {
			if (error_r != NULL)
				*error_r = "Missing public key header";
			return FALSE;
		}
		BIO *b64 = BIO_new(BIO_f_base64());
		EC_KEY *eckey = d2i_EC_PUBKEY_bio(b64, NULL);
		if (eckey != NULL) {
			EC_KEY_set_conv_form(eckey, POINT_CONVERSION_COMPRESSED);
			EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);
			key = EVP_PKEY_new();
			EVP_PKEY_set1_EC_KEY(key, eckey);
		}
	}

	BIO_vfree(key_in);

	if (key == NULL)
		return dcrypt_openssl_error(error_r);

	*key_r = (struct dcrypt_public_key *)key;

	return TRUE;
}

static
bool dcrypt_openssl_store_private_key(struct dcrypt_private_key *key, enum dcrypt_key_format format,
	const char *cipher, buffer_t *destination, const char *password, struct dcrypt_public_key *enc_key,
	const char **error_r)
{
	int ec;
	if (format == DCRYPT_FORMAT_DOVECOT) {
		bool ret;
		T_BEGIN {
			ret = dcrypt_openssl_store_private_key_dovecot(key, cipher, destination, password, enc_key, error_r);
		} T_END;
		return ret;
	}

	EVP_PKEY *pkey = (EVP_PKEY*)key;
	BIO *key_out = BIO_new(BIO_s_mem());
	const EVP_CIPHER *algo = NULL;
	if (cipher != NULL) {
		algo = EVP_get_cipherbyname(cipher);
		if (algo == NULL) {
			if (error_r != NULL)
				*error_r = t_strdup_printf("Invalid cipher %s", cipher);
			return FALSE;
		}
	}

	ec = PEM_write_bio_PrivateKey(key_out, pkey, algo, NULL, 0, NULL, (void*)password);

	(void)BIO_flush(key_out);

	if (ec != 1) {
		BIO_vfree(key_out);
		return dcrypt_openssl_error(error_r);
	}

	long bs;
	char *buf;
	bs = BIO_get_mem_data(key_out, &buf);
	buffer_append(destination, buf, bs);
	BIO_vfree(key_out);

	return TRUE;
}

static
bool dcrypt_openssl_store_public_key(struct dcrypt_public_key *key, enum dcrypt_key_format format, buffer_t *destination, const char **error_r)
{
	int ec;
	if (format == DCRYPT_FORMAT_DOVECOT)
		return dcrypt_openssl_store_public_key_dovecot(key, destination, error_r);

	EVP_PKEY *pkey = (EVP_PKEY*)key;
	BIO *key_out = BIO_new(BIO_s_mem());

	if (EVP_PKEY_base_id(pkey) == EVP_PKEY_RSA)
		ec = PEM_write_bio_PUBKEY(key_out, pkey);
	else {
		BIO *b64 = BIO_new(BIO_f_base64());
		(void)BIO_puts(key_out, "-----BEGIN PUBLIC KEY-----\n");
		(void)BIO_push(b64, key_out);
		ec = i2d_EC_PUBKEY_bio(b64, EVP_PKEY_get0_EC_KEY(pkey));
		(void)BIO_flush(b64);
		(void)BIO_pop(b64);
		BIO_vfree(b64);
		(void)BIO_puts(key_out, "-----END PUBLIC KEY-----");
	}

	if (ec != 1) {
		BIO_vfree(key_out);
		return dcrypt_openssl_error(error_r);
	}

	long bs;
	char *buf;
	bs = BIO_get_mem_data(key_out, &buf);
	buffer_append(destination, buf, bs);
	BIO_vfree(key_out);

	return TRUE;
}

static
bool dcrypt_openssl_private_to_public_key(struct dcrypt_private_key *priv_key, struct dcrypt_public_key **pub_key_r, const char **error_r)
{
	EVP_PKEY *pkey = (EVP_PKEY*)priv_key;
	EVP_PKEY *pk;

	if (*pub_key_r == NULL)
		pk = EVP_PKEY_new();
	else
		pk = (EVP_PKEY*)*pub_key_r;

	if (EVP_PKEY_base_id(pkey) == EVP_PKEY_RSA)
	{
		EVP_PKEY_set1_RSA(pk, RSAPublicKey_dup(EVP_PKEY_get0_RSA(pkey)));
	} else if (EVP_PKEY_base_id(pkey) == EVP_PKEY_EC) {
		EC_KEY* eck = EVP_PKEY_get1_EC_KEY(pkey);
		EC_KEY_set_asn1_flag(eck, OPENSSL_EC_NAMED_CURVE);
		EVP_PKEY_set1_EC_KEY(pk, eck);
		EC_KEY_free(eck);
	} else {
		if (error_r != NULL)
			*error_r = "Invalid private key";
		return FALSE;
	}

	*pub_key_r = (struct dcrypt_public_key*)pk;
	return TRUE;
}

static
bool dcrypt_openssl_key_string_get_info(const char *key_data, enum dcrypt_key_format *format_r, enum dcrypt_key_version *version_r,
	enum dcrypt_key_kind *kind_r, enum dcrypt_key_encryption_type *encryption_type_r, const char **encryption_key_hash_r,
	const char **key_hash_r, const char **error_r)
{
	enum dcrypt_key_format format = DCRYPT_FORMAT_PEM;
	enum dcrypt_key_version version = DCRYPT_KEY_VERSION_NA;
	enum dcrypt_key_encryption_type encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_NONE;
	enum dcrypt_key_kind kind = DCRYPT_KEY_KIND_PUBLIC;
	char *encryption_key_hash = NULL;
	char *key_hash = NULL;

	if (key_data == NULL) {
		if (error_r != NULL)
			*error_r = "NULL key passed";
		return FALSE;
	}

	/* is it PEM key */
	if (strstr(key_data, "----- BEGIN ") != NULL) {
		format = DCRYPT_FORMAT_PEM;
		version = DCRYPT_KEY_VERSION_NA;
		if (strstr(key_data, "ENCRYPTED") != NULL) {
			encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_PASSWORD;
		}
		if (strstr(key_data, "----- BEGIN PRIVATE KEY") != NULL)
			kind = DCRYPT_KEY_KIND_PRIVATE;
		else if (strstr(key_data, "----- BEGIN PUBLIC KEY") != NULL)
			kind = DCRYPT_KEY_KIND_PUBLIC;
		else {
			if (error_r != NULL)
				*error_r = "Unknown/invalid PEM key type";
			return FALSE;
		}
	} else T_BEGIN {
		const char **fields = t_strsplit_tab(key_data);
		int nfields;
		for(nfields=0;fields[nfields]!=NULL;nfields++);
		if (nfields < 2) {
			if (error_r != NULL)
				*error_r = "Unknown key format";
			return FALSE;
		}

		format = DCRYPT_FORMAT_DOVECOT;

		/* field 1 - version */
		if (strcmp(fields[0], "1") == 0) {
			version = DCRYPT_KEY_VERSION_1;
			if (nfields == 3) {
				kind = DCRYPT_KEY_KIND_PUBLIC;
			} else if (nfields == 5 && strcmp(fields[2],"0") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_NONE;
			} else if (nfields == 6 && strcmp(fields[2],"2") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_PASSWORD;
			} else if (nfields == 11 && strcmp(fields[2],"1") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_KEY;
				if (encryption_key_hash_r != NULL)
					encryption_key_hash = i_strdup(fields[nfields-2]);
			} else {
				if (error_r != NULL)
					*error_r = "Invalid dovecot v1 encoding";
				return FALSE;
			}
		} else if (strcmp(fields[0], "2") == 0) {
			version = DCRYPT_KEY_VERSION_1;
			if (nfields == 2) {
				kind = DCRYPT_KEY_KIND_PUBLIC;
			} else if (nfields == 5 && strcmp(fields[2],"0") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_NONE;
			} else if (nfields == 9 && strcmp(fields[2],"2") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_PASSWORD;
			} else if (nfields == 11 && strcmp(fields[2],"1") == 0) {
				kind = DCRYPT_KEY_KIND_PRIVATE;
				encryption_type = DCRYPT_KEY_ENCRYPTION_TYPE_KEY;
				if (encryption_key_hash_r != NULL)
					encryption_key_hash = i_strdup(fields[nfields-2]);
			} else {
				if (error_r != NULL)
					*error_r = "Invalid dovecot v2 encoding";
				return FALSE;
			}
		}

		/* last field is always key hash */
		if (key_hash_r != NULL)
			key_hash = i_strdup(fields[nfields-1]);
	} T_END;

	if (format_r != NULL) *format_r = format;
	if (version_r != NULL) *version_r = version;
	if (encryption_type_r != NULL) *encryption_type_r = encryption_type;
	if (encryption_key_hash_r != NULL) {
		*encryption_key_hash_r = t_strdup(encryption_key_hash);
		i_free(encryption_key_hash);
	}
	if (kind_r != NULL) *kind_r = kind;
	if (key_hash_r != NULL) {
		*key_hash_r = t_strdup(key_hash);
		i_free(key_hash);
	}
	return TRUE;
}

static
void dcrypt_openssl_free_public_key(struct dcrypt_public_key **key)
{
	EVP_PKEY_free((EVP_PKEY*)*key);
	*key = NULL;
}
static
void dcrypt_openssl_free_private_key(struct dcrypt_private_key **key)
{
	EVP_PKEY_free((EVP_PKEY*)*key);
	*key = NULL;
}
static
void dcrypt_openssl_free_keypair(struct dcrypt_keypair *keypair)
{
	dcrypt_openssl_free_public_key(&(keypair->pub));
	dcrypt_openssl_free_private_key(&(keypair->priv));
}

static
bool dcrypt_openssl_rsa_encrypt(struct dcrypt_public_key *key, const unsigned char *data, size_t data_len, buffer_t *result, const char **error_r)
{
	int ec;

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY*)key, NULL);
	size_t outl = EVP_PKEY_size((EVP_PKEY*)key);
	unsigned char buf[outl];

	if (ctx == NULL ||
	    EVP_PKEY_encrypt_init(ctx) < 1 ||
	    EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) < 1 ||
	    EVP_PKEY_encrypt(ctx, buf, &outl, data, data_len) < 1) {
		dcrypt_openssl_error(error_r);
		ec = -1;
	} else {
		buffer_append(result, buf, outl);
		ec = 0;
	}

	EVP_PKEY_CTX_free(ctx);

	return ec == 0;
}
static
bool dcrypt_openssl_rsa_decrypt(struct dcrypt_private_key *key, const unsigned char *data, size_t data_len, buffer_t *result, const char **error_r)
{
	int ec;

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY*)key, NULL);
	size_t outl = EVP_PKEY_size((EVP_PKEY*)key);
	unsigned char buf[outl];

	if (ctx == NULL ||
	    EVP_PKEY_decrypt_init(ctx) < 1 ||
	    EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) < 1 ||
	    EVP_PKEY_decrypt(ctx, buf, &outl, data, data_len) < 1) {
		dcrypt_openssl_error(error_r);
		ec = -1;
	} else {
		buffer_append(result, buf, outl);
		ec = 0;
	}

	EVP_PKEY_CTX_free(ctx);

	return ec == 0;
}

static
const char *dcrypt_openssl_oid2name(const unsigned char *oid, size_t oid_len, const char **error_r)
{
	const char *name;
	ASN1_OBJECT *obj = d2i_ASN1_OBJECT(NULL, &oid, oid_len);
	if (obj == NULL) {
		dcrypt_openssl_error(error_r);
		return NULL;
	}
	name = OBJ_nid2sn(OBJ_obj2nid(obj));
	ASN1_OBJECT_free(obj);
	return name;
}

static
bool dcrypt_openssl_name2oid(const char *name, buffer_t *oid, const char **error_r)
{
	ASN1_OBJECT *obj = OBJ_txt2obj(name, 0);
	if (obj == NULL)
		return dcrypt_openssl_error(error_r);
	if (obj->length == 0) {
		if (error_r != NULL)
			*error_r = "Object has no OID assigned";
		return FALSE;
	}
	unsigned char *bufptr = buffer_append_space_unsafe(oid, obj->length + 2);
	i2d_ASN1_OBJECT(obj, &bufptr);
	ASN1_OBJECT_free(obj);
	if (bufptr != NULL) {
		return TRUE;
	}
	return dcrypt_openssl_error(error_r);
}

static
bool dcrypt_openssl_private_key_type(struct dcrypt_private_key *key, enum dcrypt_key_type *key_type)
{
	EVP_PKEY *priv = (EVP_PKEY*)key;
	if (priv == NULL) return FALSE;
	if (EVP_PKEY_base_id(priv) == EVP_PKEY_RSA) *key_type = DCRYPT_KEY_RSA;
	else if (EVP_PKEY_base_id(priv) == EVP_PKEY_EC) *key_type = DCRYPT_KEY_EC;
	return FALSE;
}

static
bool dcrypt_openssl_public_key_type(struct dcrypt_public_key *key, enum dcrypt_key_type *key_type)
{
	EVP_PKEY *pub = (EVP_PKEY*)key;
	if (pub == NULL) return FALSE;
	if (EVP_PKEY_base_id(pub) == EVP_PKEY_RSA) *key_type = DCRYPT_KEY_RSA;
	else if (EVP_PKEY_base_id(pub) == EVP_PKEY_EC) *key_type = DCRYPT_KEY_EC;
	return FALSE;
}

/** this is the v1 old legacy way of doing key id's **/
static
bool dcrypt_openssl_public_key_id_old(struct dcrypt_public_key *key, buffer_t *result, const char **error_r)
{
	unsigned char buf[SHA256_DIGEST_LENGTH];
	EVP_PKEY *pub = (EVP_PKEY*)key;

	if (pub == NULL) {
		if (error_r != NULL)
			*error_r = "key is NULL";
		return FALSE;
	}
	if (EVP_PKEY_base_id(pub) != EVP_PKEY_EC) {
		if (error_r != NULL)
			*error_r = "Only EC key supported";
		return FALSE;
	}

	char *pub_pt_hex = ec_key_get_pub_point_hex(EVP_PKEY_get0_EC_KEY(pub));
	/* digest this */
	SHA256((const unsigned char*)pub_pt_hex, strlen(pub_pt_hex), buf);
	buffer_append(result, buf, SHA256_DIGEST_LENGTH);
	OPENSSL_free(pub_pt_hex);
	return TRUE;
}

/** this is the new which uses H(der formatted public key) **/
static
bool dcrypt_openssl_public_key_id(struct dcrypt_public_key *key, const char *algorithm, buffer_t *result, const char **error_r)
{
	const EVP_MD *md = EVP_get_digestbyname(algorithm);
	if (md == NULL) {
		if (error_r != NULL)
			*error_r = t_strdup_printf("Unknown cipher %s", algorithm);
		return FALSE;
	}
	unsigned char buf[EVP_MD_size(md)];
	EVP_PKEY *pub = (EVP_PKEY*)key;
	const char *ptr;
	int ec;
	if (pub == NULL) {
		if (error_r != NULL)
			*error_r = "key is NULL";
		return FALSE;
	}
	if (EVP_PKEY_base_id(pub) == EVP_PKEY_EC) {
		EC_KEY_set_conv_form(EVP_PKEY_get0_EC_KEY(pub), POINT_CONVERSION_COMPRESSED);
	}
	BIO *b = BIO_new(BIO_s_mem());
	if (i2d_PUBKEY_bio(b, pub) < 1) {
		BIO_vfree(b);
		return dcrypt_openssl_error(error_r);
	}
	long len = BIO_get_mem_data(b, &ptr);
	unsigned int hlen = sizeof(buf);
	/* then hash it */
#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
#else
	EVP_MD_CTX *ctx = EVP_MD_CTX_create();
#endif
	if (EVP_DigestInit_ex(ctx, md, NULL) < 1 ||
	    EVP_DigestUpdate(ctx, (const unsigned char*)ptr, len) < 1 ||
	    EVP_DigestFinal_ex(ctx, buf, &hlen) < 1) {
		ec = dcrypt_openssl_error(error_r);
	} else {
		buffer_append(result, buf, hlen);
		ec = 0;
	}

#if SSLEAY_VERSION_NUMBER >= 0x1010000fL
	EVP_MD_CTX_free(ctx);
#else
	EVP_MD_CTX_destroy(ctx);
#endif
	BIO_vfree(b);

	return ec == 0;
}

static struct dcrypt_vfs dcrypt_openssl_vfs = {
	.ctx_sym_create = dcrypt_openssl_ctx_sym_create,
	.ctx_sym_destroy = dcrypt_openssl_ctx_sym_destroy,
	.ctx_sym_set_key = dcrypt_openssl_ctx_sym_set_key,
	.ctx_sym_set_iv = dcrypt_openssl_ctx_sym_set_iv,
	.ctx_sym_set_key_iv_random = dcrypt_openssl_ctx_sym_set_key_iv_random,
	.ctx_sym_set_padding = dcrypt_openssl_ctx_sym_set_padding,
	.ctx_sym_get_key = dcrypt_openssl_ctx_sym_get_key,
	.ctx_sym_get_iv = dcrypt_openssl_ctx_sym_get_iv,
	.ctx_sym_set_aad = dcrypt_openssl_ctx_sym_set_aad,
	.ctx_sym_get_aad = dcrypt_openssl_ctx_sym_get_aad,
	.ctx_sym_set_tag = dcrypt_openssl_ctx_sym_set_tag,
	.ctx_sym_get_tag = dcrypt_openssl_ctx_sym_get_tag,
	.ctx_sym_get_key_length = dcrypt_openssl_ctx_sym_get_key_length,
	.ctx_sym_get_iv_length = dcrypt_openssl_ctx_sym_get_iv_length,
	.ctx_sym_get_block_size = dcrypt_openssl_ctx_sym_get_block_size,
	.ctx_sym_init = dcrypt_openssl_ctx_sym_init,
	.ctx_sym_update = dcrypt_openssl_ctx_sym_update,
	.ctx_sym_final = dcrypt_openssl_ctx_sym_final,
	.ctx_hmac_create = dcrypt_openssl_ctx_hmac_create,
	.ctx_hmac_destroy = dcrypt_openssl_ctx_hmac_destroy,
	.ctx_hmac_set_key = dcrypt_openssl_ctx_hmac_set_key,
	.ctx_hmac_set_key_random = dcrypt_openssl_ctx_hmac_set_key_random,
	.ctx_hmac_get_digest_length = dcrypt_openssl_ctx_hmac_get_digest_length,
	.ctx_hmac_get_key = dcrypt_openssl_ctx_hmac_get_key,
	.ctx_hmac_init = dcrypt_openssl_ctx_hmac_init,
	.ctx_hmac_update = dcrypt_openssl_ctx_hmac_update,
	.ctx_hmac_final = dcrypt_openssl_ctx_hmac_final,
	.ecdh_derive_secret_local = dcrypt_openssl_ecdh_derive_secret_local,
	.ecdh_derive_secret_peer = dcrypt_openssl_ecdh_derive_secret_peer,
	.pbkdf2 = dcrypt_openssl_pbkdf2,
	.generate_keypair = dcrypt_openssl_generate_keypair,
	.load_private_key = dcrypt_openssl_load_private_key,
	.load_public_key = dcrypt_openssl_load_public_key,
	.store_private_key = dcrypt_openssl_store_private_key,
	.store_public_key = dcrypt_openssl_store_public_key,
	.private_to_public_key = dcrypt_openssl_private_to_public_key,
	.key_string_get_info = dcrypt_openssl_key_string_get_info,
	.free_keypair = dcrypt_openssl_free_keypair,
	.free_public_key = dcrypt_openssl_free_public_key,
	.free_private_key = dcrypt_openssl_free_private_key,
	.rsa_encrypt = dcrypt_openssl_rsa_encrypt,
	.rsa_decrypt = dcrypt_openssl_rsa_decrypt,
	.oid2name = dcrypt_openssl_oid2name,
	.name2oid = dcrypt_openssl_name2oid,
	.private_key_type = dcrypt_openssl_private_key_type,
	.public_key_type = dcrypt_openssl_public_key_type,
	.public_key_id = dcrypt_openssl_public_key_id,
	.public_key_id_old = dcrypt_openssl_public_key_id_old,
};

void dcrypt_openssl_init(struct module *module ATTR_UNUSED)
{
	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();
	dcrypt_set_vfs(&dcrypt_openssl_vfs);
}

void dcrypt_openssl_deinit(void)
{
#if OPENSSL_API_COMPAT < 0x10100000L
	OBJ_cleanup();
#endif
}
