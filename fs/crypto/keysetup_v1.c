// SPDX-License-Identifier: GPL-2.0
/*
 * Key setup for v1 encryption policies
 *
 * Copyright 2015, 2019 Google LLC
 */

/*
 * This file implements compatibility functions for the original encryption
 * policy version ("v1"), including:
 *
 * - Deriving per-file encryption keys using the AES-128-ECB based KDF
 *   (rather than the new method of using HKDF-SHA512)
 *
 * - Retrieving fscrypt master keys from process-subscribed keyrings
 *   (rather than the new method of using a filesystem-level keyring)
 *
 * - Handling policies with the DIRECT_KEY flag set using a master key table
 *   (rather than the new method of implementing DIRECT_KEY with per-mode keys
 *    managed alongside the master keys in the filesystem-level keyring)
 */

#include <crypto/algapi.h>
#include <crypto/skcipher.h>
#include <keys/user-type.h>
#include <linux/hashtable.h>
#include <linux/scatterlist.h>
#include <linux/bio-crypt-ctx.h>
#include <linux/siphash.h>
#include <crypto/sha.h>

#include "fscrypt_private.h"

#ifdef CONFIG_FSCRYPT_SDP
static int derive_fek(struct inode *inode,
		struct fscrypt_info *crypt_info,
		u8 *fek, u32 fek_len);
#endif

/* Table of keys referenced by DIRECT_KEY policies */
static DEFINE_HASHTABLE(fscrypt_direct_keys, 6); /* 6 bits = 64 buckets */
static DEFINE_SPINLOCK(fscrypt_direct_keys_lock);

static struct crypto_shash *essiv_hash_tfm;

/*
 * v1 key derivation function.  This generates the derived key by encrypting the
 * master key with AES-128-ECB using the nonce as the AES key.  This provides a
 * unique derived key with sufficient entropy for each inode.  However, it's
 * nonstandard, non-extensible, doesn't evenly distribute the entropy from the
 * master key, and is trivially reversible: an attacker who compromises a
 * derived key can "decrypt" it to get back to the master key, then derive any
 * other key.  For all new code, use HKDF instead.
 *
 * The master key must be at least as long as the derived key.  If the master
 * key is longer, then only the first 'derived_keysize' bytes are used.
 */
static int derive_key_aes(const u8 *master_key,
			  const u8 nonce[FS_KEY_DERIVATION_NONCE_SIZE],
			  u8 *derived_key, unsigned int derived_keysize)
{
	int res = 0;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);

	if (IS_ERR(tfm)) {
		res = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			crypto_req_done, &wait);
	res = crypto_skcipher_setkey(tfm, nonce, FS_KEY_DERIVATION_NONCE_SIZE);
	if (res < 0)
		goto out;

	sg_init_one(&src_sg, master_key, derived_keysize);
	sg_init_one(&dst_sg, derived_key, derived_keysize);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, derived_keysize,
				   NULL);
	res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return res;
}

static int fscrypt_do_sha256(const u8 *src, int srclen, u8 *dst)
{
	struct crypto_shash *tfm = READ_ONCE(essiv_hash_tfm);

	/* init hash transform on demand */
	if (unlikely(!tfm)) {
		struct crypto_shash *prev_tfm;

		tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(tfm)) {
			fscrypt_warn(NULL,
				     "error allocating SHA-256 transform: %ld",
				     PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		prev_tfm = cmpxchg(&essiv_hash_tfm, NULL, tfm);
		if (prev_tfm) {
			crypto_free_shash(tfm);
			tfm = prev_tfm;
		}
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		desc->flags = 0;
		return crypto_shash_digest(desc, src, srclen, dst);
	}
}

/*
 * Search the current task's subscribed keyrings for a "logon" key with
 * description prefix:descriptor, and if found acquire a read lock on it and
 * return a pointer to its validated payload in *payload_ret.
 */
static struct key *
find_and_lock_process_key(const char *prefix,
			  const u8 descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE],
			  unsigned int min_keysize,
			  const struct fscrypt_key **payload_ret)
{
	char *description;
	struct key *key;
	const struct user_key_payload *ukp;
	const struct fscrypt_key *payload;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FSCRYPT_KEY_DESCRIPTOR_SIZE, descriptor);
	if (!description)
		return ERR_PTR(-ENOMEM);

	key = request_key(&key_type_logon, description, NULL);
	kfree(description);
	if (IS_ERR(key))
		return key;

	down_read(&key->sem);
	ukp = user_key_payload_locked(key);

	if (!ukp) /* was the key revoked before we acquired its semaphore? */
		goto invalid;

	payload = (const struct fscrypt_key *)ukp->data;

	if (ukp->datalen != sizeof(struct fscrypt_key) ||
	    payload->size < 1 || payload->size > FSCRYPT_MAX_KEY_SIZE) {
		fscrypt_warn(NULL,
			     "key with description '%s' has invalid payload",
			     key->description);
		goto invalid;
	}

	if (payload->size < min_keysize) {
		fscrypt_warn(NULL,
			     "key with description '%s' is too short (got %u bytes, need %u+ bytes)",
			     key->description, payload->size, min_keysize);
		goto invalid;
	}

	*payload_ret = payload;
	return key;

invalid:
	up_read(&key->sem);
	key_put(key);
	return ERR_PTR(-ENOKEY);
}

/* Master key referenced by DIRECT_KEY policy */
struct fscrypt_direct_key {
	struct hlist_node		dk_node;
	refcount_t			dk_refcount;
	const struct fscrypt_mode	*dk_mode;
	struct fscrypt_prepared_key	dk_key;
	u8				dk_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
	u8				dk_raw[FSCRYPT_MAX_KEY_SIZE];
};

static void free_direct_key(struct fscrypt_direct_key *dk)
{
	if (dk) {
		fscrypt_destroy_prepared_key(&dk->dk_key);
		kzfree(dk);
	}
}

void fscrypt_put_direct_key(struct fscrypt_direct_key *dk)
{
	if (!refcount_dec_and_lock(&dk->dk_refcount, &fscrypt_direct_keys_lock))
		return;
	hash_del(&dk->dk_node);
	spin_unlock(&fscrypt_direct_keys_lock);

	free_direct_key(dk);
}

/*
 * Find/insert the given key into the fscrypt_direct_keys table.  If found, it
 * is returned with elevated refcount, and 'to_insert' is freed if non-NULL.  If
 * not found, 'to_insert' is inserted and returned if it's non-NULL; otherwise
 * NULL is returned.
 */
static struct fscrypt_direct_key *
find_or_insert_direct_key(struct fscrypt_direct_key *to_insert,
			  const u8 *raw_key, const struct fscrypt_info *ci)
{
	unsigned long hash_key;
	struct fscrypt_direct_key *dk;

	/*
	 * Careful: to avoid potentially leaking secret key bytes via timing
	 * information, we must key the hash table by descriptor rather than by
	 * raw key, and use crypto_memneq() when comparing raw keys.
	 */

	BUILD_BUG_ON(sizeof(hash_key) > FSCRYPT_KEY_DESCRIPTOR_SIZE);
	memcpy(&hash_key, ci->ci_policy.v1.master_key_descriptor,
	       sizeof(hash_key));

	spin_lock(&fscrypt_direct_keys_lock);
	hash_for_each_possible(fscrypt_direct_keys, dk, dk_node, hash_key) {
		if (memcmp(ci->ci_policy.v1.master_key_descriptor,
			   dk->dk_descriptor, FSCRYPT_KEY_DESCRIPTOR_SIZE) != 0)
			continue;
		if (ci->ci_mode != dk->dk_mode)
			continue;
		if (!fscrypt_is_key_prepared(&dk->dk_key, ci))
			continue;
		if (crypto_memneq(raw_key, dk->dk_raw, ci->ci_mode->keysize))
			continue;
		/* using existing tfm with same (descriptor, mode, raw_key) */
		refcount_inc(&dk->dk_refcount);
		spin_unlock(&fscrypt_direct_keys_lock);
		free_direct_key(to_insert);
		return dk;
	}
	if (to_insert)
		hash_add(fscrypt_direct_keys, &to_insert->dk_node, hash_key);
	spin_unlock(&fscrypt_direct_keys_lock);
	return to_insert;
}

/* Prepare to encrypt directly using the master key in the given mode */
static struct fscrypt_direct_key *
fscrypt_get_direct_key(const struct fscrypt_info *ci, const u8 *raw_key)
{
	struct fscrypt_direct_key *dk;
	int err;

	/* Is there already a tfm for this key? */
	dk = find_or_insert_direct_key(NULL, raw_key, ci);
	if (dk)
		return dk;

	/* Nope, allocate one. */
	dk = kzalloc(sizeof(*dk), GFP_NOFS);
	if (!dk)
		return ERR_PTR(-ENOMEM);
	refcount_set(&dk->dk_refcount, 1);
	dk->dk_mode = ci->ci_mode;
	err = fscrypt_prepare_key(&dk->dk_key, raw_key, ci->ci_mode->keysize,
				  false /*is_hw_wrapped*/, ci);
	if (err)
		goto err_free_dk;
	memcpy(dk->dk_descriptor, ci->ci_policy.v1.master_key_descriptor,
	       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	memcpy(dk->dk_raw, raw_key, ci->ci_mode->keysize);

	return find_or_insert_direct_key(dk, raw_key, ci);

err_free_dk:
	free_direct_key(dk);
	return ERR_PTR(err);
}

/* v1 policy, DIRECT_KEY: use the master key directly */
static int setup_v1_file_key_direct(struct fscrypt_info *ci,
				    const u8 *raw_master_key)
{
	struct fscrypt_direct_key *dk;

	dk = fscrypt_get_direct_key(ci, raw_master_key);
	if (IS_ERR(dk))
		return PTR_ERR(dk);
	ci->ci_direct_key = dk;
	ci->ci_key = dk->dk_key;
	return 0;
}

/* v1 policy, !DIRECT_KEY: derive the file's encryption key */
static int setup_v1_file_key_derived(struct fscrypt_info *ci,
				     const u8 *raw_master_key)
{
	u8 *derived_key = NULL;
	int err;
	int i;
	union {
		u8 bytes[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE];
		u32 words[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE / sizeof(u32)];
	} key_new;

#ifdef CONFIG_FSCRYPT_SDP
	sdp_fs_command_t *cmd = NULL;

	if (fscrypt_sdp_is_classified(ci)) {
		/*
		 * This cannot be a stack buffer because it will be passed to the
		 * scatterlist crypto API during derive_key_aes().
		 */
		derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
		if (!derived_key)
			return -ENOMEM;

		err = derive_fek(ci->ci_inode, ci, derived_key, ci->ci_mode->keysize);
		if (err) {
			if (fscrypt_sdp_is_sensitive(ci)) {
				cmd = sdp_fs_command_alloc(FSOP_AUDIT_FAIL_DECRYPT,
						current->tgid, ci->ci_sdp_info->engine_id, -1,
						ci->ci_inode->i_ino, err, GFP_NOFS);
				if (cmd) {
					sdp_fs_request(cmd, NULL);
					sdp_fs_command_free(cmd);
				}
			}

			goto out;
		}

		memcpy(key_new.bytes, derived_key, ci->ci_mode->keysize);
		for (i = 0; i < ARRAY_SIZE(key_new.words); i++)
			__cpu_to_be32s(&key_new.words[i]);
		memcpy(derived_key, key_new.bytes, ci->ci_mode->keysize);
		memzero_explicit(key_new.bytes, sizeof(key_new.bytes));

		fscrypt_sdp_update_conv_status(ci);
		goto sdp_dek;
	}
#endif

	/*Support legacy ice based content encryption mode*/
	if ((fscrypt_policy_contents_mode(&ci->ci_policy) ==
					  FSCRYPT_MODE_PRIVATE) &&
					  fscrypt_using_inline_encryption(ci)) {
		if (ci->ci_policy.v1.flags &
		    FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) {
			union {
				siphash_key_t k;
				u8 bytes[SHA256_DIGEST_SIZE];
			} ino_hash_key;
			int err;

			/* hashed_ino = SipHash(key=SHA256(master_key),
			 * data=i_ino)
			 */
			err = fscrypt_do_sha256(raw_master_key,
						ci->ci_mode->keysize / 2,
						ino_hash_key.bytes);
			if (err)
				return err;
			ci->ci_hashed_ino = siphash_1u64(ci->ci_inode->i_ino,
							 &ino_hash_key.k);
		}

#if IS_ENABLED(CONFIG_ENABLE_LEGACY_PFK)
		derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
		if (!derived_key)
			return -ENOMEM;

		err = derive_key_aes(raw_master_key, ci->ci_nonce,
				     derived_key, ci->ci_mode->keysize);
		if (err)
			goto out;

		memcpy(key_new.bytes, derived_key, ci->ci_mode->keysize);
#else
		memcpy(key_new.bytes, raw_master_key, ci->ci_mode->keysize);
#endif

		for (i = 0; i < ARRAY_SIZE(key_new.words); i++)
			__cpu_to_be32s(&key_new.words[i]);

		err = setup_v1_file_key_direct(ci, key_new.bytes);

		if (derived_key)
			kzfree(derived_key);

		return err;
	}
	/*
	 * This cannot be a stack buffer because it will be passed to the
	 * scatterlist crypto API during derive_key_aes().
	 */
	derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
	if (!derived_key)
		return -ENOMEM;

	err = derive_key_aes(raw_master_key, ci->ci_nonce,
			     derived_key, ci->ci_mode->keysize);
	if (err)
		goto out;

#ifdef CONFIG_FSCRYPT_SDP
sdp_dek:
#endif
	err = fscrypt_set_per_file_enc_key(ci, derived_key);
out:
	if (derived_key)
		kzfree(derived_key);

	return err;
}

int fscrypt_setup_v1_file_key(struct fscrypt_info *ci, const u8 *raw_master_key)
{
	if (ci->ci_policy.v1.flags & FSCRYPT_POLICY_FLAG_DIRECT_KEY)
		return setup_v1_file_key_direct(ci, raw_master_key);
	else
		return setup_v1_file_key_derived(ci, raw_master_key);
}

int fscrypt_setup_v1_file_key_via_subscribed_keyrings(struct fscrypt_info *ci)
{
	struct key *key;
	const struct fscrypt_key *payload;
	int err;

	key = find_and_lock_process_key(FSCRYPT_KEY_DESC_PREFIX,
					ci->ci_policy.v1.master_key_descriptor,
					ci->ci_mode->keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && ci->ci_inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(ci->ci_inode->i_sb->s_cop->key_prefix,
						ci->ci_policy.v1.master_key_descriptor,
						ci->ci_mode->keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);

	err = fscrypt_setup_v1_file_key(ci, payload->raw);
	up_read(&key->sem);
	key_put(key);
	return err;
}

#ifdef CONFIG_FSCRYPT_SDP
static int __find_and_derive_v1_file_key(
					struct fscrypt_key *key,
					struct fscrypt_info *ci,
					const u8 *raw_master_key)
{
	u8 *derived_key;
	int err;

	/*Support legacy ice based content encryption mode*/
	if ((fscrypt_policy_contents_mode(&ci->ci_policy) ==
					  FSCRYPT_MODE_PRIVATE) &&
					  fscrypt_using_inline_encryption(ci)) {
		memcpy(key->raw, raw_master_key, ci->ci_mode->keysize);
		key->size = ci->ci_mode->keysize;
		return 0;
	}

	derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
	if (!derived_key)
		return -ENOMEM;

	err = derive_key_aes(raw_master_key, ci->ci_nonce,
			     derived_key, ci->ci_mode->keysize);
	if (err)
		goto out;

	memcpy(key->raw, derived_key, ci->ci_mode->keysize);
	key->size = ci->ci_mode->keysize;

out:
	kzfree(derived_key);
	return err;
}

static inline int __find_and_derive_v1_fskey_via_subscribed_keyrings(
					const struct fscrypt_info *ci,
					struct fscrypt_key *fskey)
{
	struct key *key;
	const struct fscrypt_key *payload;

	key = find_and_lock_process_key(FSCRYPT_KEY_DESC_PREFIX,
					ci->ci_policy.v1.master_key_descriptor,
					ci->ci_mode->keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && ci->ci_inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(ci->ci_inode->i_sb->s_cop->key_prefix,
						ci->ci_policy.v1.master_key_descriptor,
						ci->ci_mode->keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);

	memcpy(fskey, payload, sizeof(struct fscrypt_key));

	up_read(&key->sem);
	key_put(key);
	return 0;
}

static inline int __find_and_derive_v1_fskey(
					const struct fscrypt_info *ci,
					struct fscrypt_key *fskey)
{
	struct key *key;
	struct fscrypt_master_key *mk = NULL;
	struct fscrypt_key_specifier mk_spec;
	int err;

	if (!ci)
		return -EINVAL;

	switch (ci->ci_policy.version) {
	case FSCRYPT_POLICY_V1:
		mk_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
		memcpy(mk_spec.u.descriptor,
		       ci->ci_policy.v1.master_key_descriptor,
		       FSCRYPT_KEY_DESCRIPTOR_SIZE);
		break;
//	case FSCRYPT_POLICY_V2:
//		mk_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
//		memcpy(mk_spec.u.identifier,
//		       ci->ci_policy.v2.master_key_identifier,
//		       FSCRYPT_KEY_IDENTIFIER_SIZE);
//		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	key = fscrypt_find_master_key(ci->ci_inode->i_sb, &mk_spec);
	if (IS_ERR(key)) {
		if (key != ERR_PTR(-ENOKEY) ||
		    ci->ci_policy.version != FSCRYPT_POLICY_V1) {
			return PTR_ERR(key);
		}

		return __find_and_derive_v1_fskey_via_subscribed_keyrings(ci, fskey);
	}

	mk = key->payload.data[0];
	down_read(&mk->mk_secret_sem);

	/* Has the secret been removed (via FS_IOC_REMOVE_ENCRYPTION_KEY)? */
	if (!is_master_key_secret_present(&mk->mk_secret)) {
		err = -ENOKEY;
		goto out_release_key;
	}

	/*
	 * Require that the master key be at least as long as the derived key.
	 * Otherwise, the derived key cannot possibly contain as much entropy as
	 * that required by the encryption mode it will be used for.  For v1
	 * policies it's also required for the KDF to work at all.
	 */
	if (mk->mk_secret.size < ci->ci_mode->keysize) {
		fscrypt_warn(NULL,
			     "key with %s %*phN is too short (got %u bytes, need %u+ bytes)",
			     master_key_spec_type(&mk_spec),
			     master_key_spec_len(&mk_spec), (u8 *)&mk_spec.u,
			     mk->mk_secret.size, ci->ci_mode->keysize);
		err = -ENOKEY;
		goto out_release_key;
	}

	switch (ci->ci_policy.version) {
	case FSCRYPT_POLICY_V1:
		memcpy(fskey->raw, mk->mk_secret.raw, mk->mk_secret.size);
		fskey->size = mk->mk_secret.size;
		err = 0;
		break;
//	case FSCRYPT_POLICY_V2:
//		err = fscrypt_setup_v2_file_key(ci, mk);
//		break;
	default:
		WARN_ON(1);
		err = -EINVAL;
		break;
	}

out_release_key:
	up_read(&mk->mk_secret_sem);
	key_put(key);
	return err;
}

/* The function is only for regular files */
static int derive_fek(struct inode *inode,
						struct fscrypt_info *crypt_info,
						u8 *fek, u32 fek_len)
{
	int res = 0;
	/*
	 * 1. [ Native / Uninitialized / To_sensitive ]  --> Plain fek
	 * 2. [ Native / Uninitialized / Non_sensitive ] --> Plain fek
	 */
	if (fscrypt_sdp_is_uninitialized(crypt_info))
	{
		res = fscrypt_sdp_derive_uninitialized_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 3. [ Native / Initialized / Sensitive ]     --> { fek }_SDPK
	 * 4. [ Non_native / Initialized / Sensitive ] --> { fek }_SDPK
	 */
	else if (fscrypt_sdp_is_sensitive(crypt_info))
	{
		res = fscrypt_sdp_derive_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 5. [ Native / Initialized / Non_sensitive ] --> { fek }_cekey
	 */
	else if (fscrypt_sdp_is_native(crypt_info))
	{
		res = fscrypt_sdp_derive_fek(inode, crypt_info, fek, fek_len);
	}
	/*
	 * else { N/A }
	 *
	 * Not classified file.
	 * 6. [ Non_native / Initialized / Non_sensitive ]
	 * 7. [ Non_native / Initialized / To_sensitive ]
	 */
	return res;
}

int fscrypt_get_encryption_key(
		struct fscrypt_info *crypt_info,
		struct fscrypt_key *key)
{
	struct fscrypt_key *kek = NULL;
	int res;

	if (!crypt_info)
		return -EINVAL;

	kek = kzalloc(sizeof(struct fscrypt_key), GFP_NOFS);
	if (!kek)
		return -ENOMEM;

	res = fscrypt_get_encryption_kek(crypt_info, kek);
	if (res)
		goto out;

	res = __find_and_derive_v1_file_key(key, crypt_info, kek->raw);

out:
	kzfree(kek);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_key);

int fscrypt_get_encryption_key_classified(
		struct fscrypt_info *crypt_info,
		struct fscrypt_key *key)
{
	u8 *derived_key;
	int err;

	if (!crypt_info)
		return -EINVAL;

	derived_key = kmalloc(crypt_info->ci_mode->keysize, GFP_NOFS);
	if (!derived_key)
		return -ENOMEM;

	err = derive_fek(crypt_info->ci_inode, crypt_info, derived_key, crypt_info->ci_mode->keysize);
	if (err)
		goto out;

	memcpy(key->raw, derived_key, crypt_info->ci_mode->keysize);
	key->size = crypt_info->ci_mode->keysize;

out:
	kzfree(derived_key);
	return err;
}
EXPORT_SYMBOL(fscrypt_get_encryption_key_classified);

int fscrypt_get_encryption_kek(
		struct fscrypt_info *crypt_info,
		struct fscrypt_key *kek)
{
	int res;

	if (!crypt_info)
		return -EINVAL;

	res = __find_and_derive_v1_fskey(crypt_info, kek);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_kek);

void fscrypt_sdp_finalize_v1(struct fscrypt_info *ci)
{
	int err;
	struct fscrypt_key raw_key;
	memset(&raw_key, 0, sizeof(struct fscrypt_key));

	err = fscrypt_get_encryption_key_classified(ci, &raw_key);
	if (err)
		return;

	fscrypt_sdp_finalize_tasks(ci->ci_inode, raw_key.raw, raw_key.size);
}
#endif
