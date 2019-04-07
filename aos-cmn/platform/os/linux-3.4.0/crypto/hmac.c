/*
 * Cryptographic API.
 *
 * HMAC: Keyed-Hashing for Message Authentication (RFC2104).
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * The HMAC implementation is derived from USAGI.
 * Copyright (c) 2002 Kazunori Miyazawa <miyazawa@linux-ipv6.org> / USAGI
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <crypto/internal/hash.h>
#include <crypto/scatterwalk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/string.h>

#ifdef CONFIG_ARUBA_AP
extern void aruba_save_kernel_kat_reboot_reason(const char *reason);
extern void kernel_restart(char *cmd);
static void fips_selftest_hmac_sha1(struct work_struct *);
static DECLARE_WORK(fips_hmac_sha1_kat_work, fips_selftest_hmac_sha1);
#endif

struct hmac_ctx {
	struct crypto_shash *hash;
};

static inline void *align_ptr(void *p, unsigned int align)
{
	return (void *)ALIGN((unsigned long)p, align);
}

static inline struct hmac_ctx *hmac_ctx(struct crypto_shash *tfm)
{
	return align_ptr(crypto_shash_ctx_aligned(tfm) +
			 crypto_shash_statesize(tfm) * 2,
			 crypto_tfm_ctx_alignment());
}

static int hmac_setkey(struct crypto_shash *parent,
		       const u8 *inkey, unsigned int keylen)
{
	int bs = crypto_shash_blocksize(parent);
	int ds = crypto_shash_digestsize(parent);
	int ss = crypto_shash_statesize(parent);
	char *ipad = crypto_shash_ctx_aligned(parent);
	char *opad = ipad + ss;
	struct hmac_ctx *ctx = align_ptr(opad + ss,
					 crypto_tfm_ctx_alignment());
	struct crypto_shash *hash = ctx->hash;
	struct {
		struct shash_desc shash;
		char ctx[crypto_shash_descsize(hash)];
	} desc;
	unsigned int i;

	desc.shash.tfm = hash;
	desc.shash.flags = crypto_shash_get_flags(parent) &
			    CRYPTO_TFM_REQ_MAY_SLEEP;

	if (keylen > bs) {
		int err;

		err = crypto_shash_digest(&desc.shash, inkey, keylen, ipad);
		if (err)
			return err;

		keylen = ds;
	} else
		memcpy(ipad, inkey, keylen);

	memset(ipad + keylen, 0, bs - keylen);
	memcpy(opad, ipad, bs);

	for (i = 0; i < bs; i++) {
		ipad[i] ^= 0x36;
		opad[i] ^= 0x5c;
	}

	return crypto_shash_init(&desc.shash) ?:
	       crypto_shash_update(&desc.shash, ipad, bs) ?:
	       crypto_shash_export(&desc.shash, ipad) ?:
	       crypto_shash_init(&desc.shash) ?:
	       crypto_shash_update(&desc.shash, opad, bs) ?:
	       crypto_shash_export(&desc.shash, opad);
}

static int hmac_export(struct shash_desc *pdesc, void *out)
{
	struct shash_desc *desc = shash_desc_ctx(pdesc);

	desc->flags = pdesc->flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_export(desc, out);
}

static int hmac_import(struct shash_desc *pdesc, const void *in)
{
	struct shash_desc *desc = shash_desc_ctx(pdesc);
	struct hmac_ctx *ctx = hmac_ctx(pdesc->tfm);

	desc->tfm = ctx->hash;
	desc->flags = pdesc->flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_import(desc, in);
}

static int hmac_init(struct shash_desc *pdesc)
{
	return hmac_import(pdesc, crypto_shash_ctx_aligned(pdesc->tfm));
}

static int hmac_update(struct shash_desc *pdesc,
		       const u8 *data, unsigned int nbytes)
{
	struct shash_desc *desc = shash_desc_ctx(pdesc);

	desc->flags = pdesc->flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_update(desc, data, nbytes);
}

static int hmac_final(struct shash_desc *pdesc, u8 *out)
{
	struct crypto_shash *parent = pdesc->tfm;
	int ds = crypto_shash_digestsize(parent);
	int ss = crypto_shash_statesize(parent);
	char *opad = crypto_shash_ctx_aligned(parent) + ss;
	struct shash_desc *desc = shash_desc_ctx(pdesc);

	desc->flags = pdesc->flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_final(desc, out) ?:
	       crypto_shash_import(desc, opad) ?:
	       crypto_shash_finup(desc, out, ds, out);
}

static int hmac_finup(struct shash_desc *pdesc, const u8 *data,
		      unsigned int nbytes, u8 *out)
{

	struct crypto_shash *parent = pdesc->tfm;
	int ds = crypto_shash_digestsize(parent);
	int ss = crypto_shash_statesize(parent);
	char *opad = crypto_shash_ctx_aligned(parent) + ss;
	struct shash_desc *desc = shash_desc_ctx(pdesc);

	desc->flags = pdesc->flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_finup(desc, data, nbytes, out) ?:
	       crypto_shash_import(desc, opad) ?:
	       crypto_shash_finup(desc, out, ds, out);
}

static int hmac_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_shash *parent = __crypto_shash_cast(tfm);
	struct crypto_shash *hash;
	struct crypto_instance *inst = (void *)tfm->__crt_alg;
	struct crypto_shash_spawn *spawn = crypto_instance_ctx(inst);
	struct hmac_ctx *ctx = hmac_ctx(parent);

	hash = crypto_spawn_shash(spawn);
	if (IS_ERR(hash))
		return PTR_ERR(hash);

	parent->descsize = sizeof(struct shash_desc) +
			   crypto_shash_descsize(hash);

	ctx->hash = hash;
	return 0;
}

static void hmac_exit_tfm(struct crypto_tfm *tfm)
{
	struct hmac_ctx *ctx = hmac_ctx(__crypto_shash_cast(tfm));
	crypto_free_shash(ctx->hash);
}

static int hmac_create(struct crypto_template *tmpl, struct rtattr **tb)
{
	struct shash_instance *inst;
	struct crypto_alg *alg;
	struct shash_alg *salg;
	int err;
	int ds;
	int ss;

	err = crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_SHASH);
	if (err)
		return err;

	salg = shash_attr_alg(tb[1], 0, 0);
	if (IS_ERR(salg))
		return PTR_ERR(salg);

	err = -EINVAL;
	ds = salg->digestsize;
	ss = salg->statesize;
	alg = &salg->base;
	if (ds > alg->cra_blocksize ||
	    ss < alg->cra_blocksize)
		goto out_put_alg;

	inst = shash_alloc_instance("hmac", alg);
	err = PTR_ERR(inst);
	if (IS_ERR(inst))
		goto out_put_alg;

	err = crypto_init_shash_spawn(shash_instance_ctx(inst), salg,
				      shash_crypto_instance(inst));
	if (err)
		goto out_free_inst;

	inst->alg.base.cra_priority = alg->cra_priority;
	inst->alg.base.cra_blocksize = alg->cra_blocksize;
	inst->alg.base.cra_alignmask = alg->cra_alignmask;

	ss = ALIGN(ss, alg->cra_alignmask + 1);
	inst->alg.digestsize = ds;
	inst->alg.statesize = ss;

	inst->alg.base.cra_ctxsize = sizeof(struct hmac_ctx) +
				     ALIGN(ss * 2, crypto_tfm_ctx_alignment());

	inst->alg.base.cra_init = hmac_init_tfm;
	inst->alg.base.cra_exit = hmac_exit_tfm;

	inst->alg.init = hmac_init;
	inst->alg.update = hmac_update;
	inst->alg.final = hmac_final;
	inst->alg.finup = hmac_finup;
	inst->alg.export = hmac_export;
	inst->alg.import = hmac_import;
	inst->alg.setkey = hmac_setkey;

	err = shash_register_instance(tmpl, inst);
	if (err) {
out_free_inst:
		shash_free_instance(shash_crypto_instance(inst));
	}

out_put_alg:
	crypto_mod_put(alg);
	return err;
}

#ifdef CONFIG_ARUBA_AP
/*Idealy this is needed only for FIPS ArubaOS AP build
 * but it doesnt harm to execute self test on all Regular
 * ArubaOS AP build*/

extern char* fips_fail_test;

static void fips_selftest_hmac_sha1(struct work_struct *w)
{
	char* key  = "0123456789:;<=>?@ABC";
	char result[64];
	struct scatterlist sg[8];
	struct crypto_hash *tfm;
	struct hash_desc desc;
	char* plaintext = "Sample #2";
    int fail_test = 0;
	unsigned char hmac_sha_value[] = { 0x09,0x22,0xd3,0x40,0x5f,0xaa,0x3d,0x19,
		0x4f,0x82,0xa4,0x58,0x30,0x73,0x7d,0x5c,
		0xc6,0xc7,0x5d,0x24 };
		char *tvmem;
		int result_len, ptext_len, key_len;
		printk("Starting Kernel HMAC-SHA1 KAT ...");
		ptext_len = strlen(plaintext);
		key_len = strlen(key);

		/*Allocate KERNEL mem*/
		tvmem = kmalloc(ptext_len, GFP_KERNEL);
		if (tvmem == NULL) {
			aruba_save_kernel_kat_reboot_reason("HMAC-SHA1 Alloc Failed");
			kernel_restart(NULL);
		}
		memcpy(tvmem, plaintext, ptext_len);

		sg_init_one(&sg[0], tvmem, ptext_len);

		/*Allocate tfm mem*/
		tfm = crypto_alloc_hash("hmac(sha1)", 0, 0);
		if (!tfm) {
			aruba_save_kernel_kat_reboot_reason("TFM alloc failed\n");
			kernel_restart(NULL);
		}
		crypto_hash_setkey(tfm, key, key_len);
		desc.tfm = tfm;
		desc.flags = 0;
		crypto_hash_digest(&desc, sg, ptext_len, result);

		result_len = crypto_hash_digestsize(tfm);
        if(fips_fail_test && !memcmp(fips_fail_test, "hmac-sha1", 9))
            fail_test = 1;
		if (memcmp(result, hmac_sha_value, result_len) || fail_test) {
			printk("Failed Kernel HMAC-SHA1 KAT \n");
			aruba_save_kernel_kat_reboot_reason("HMAC-SHA1 Computation Failed");
			kernel_restart(NULL);
		}
		/*Free up KERNEL & tfm mem*/
		crypto_free_hash(tfm);
		kfree(tvmem);
		printk("Completed Kernel HMAC-SHA1 KAT \n");
}

void handle_hmac_sha1_selftest(void)
{
	schedule_work(&fips_hmac_sha1_kat_work);
}
EXPORT_SYMBOL(handle_hmac_sha1_selftest);

#endif /* #ifdef CONFIG_ARUBA_AP */

static struct crypto_template hmac_tmpl = {
	.name = "hmac",
	.create = hmac_create,
	.free = shash_free_instance,
	.module = THIS_MODULE,
};

static int __init hmac_module_init(void)
{
	return crypto_register_template(&hmac_tmpl);
}

static void __exit hmac_module_exit(void)
{
	crypto_unregister_template(&hmac_tmpl);
}

module_init(hmac_module_init);
module_exit(hmac_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HMAC hash algorithm");