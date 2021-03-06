/* SPDX-License-Identifier: BSD-2-Clause */

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/rsa.h>

#include "checks.h"
#include "encrypt.h"
#include "mech.h"
#include "ssl_util.h"
#include "session.h"
#include "session_ctx.h"
#include "token.h"
#include "tpm.h"
#include "twist.h"

typedef CK_RV (*crypto_op)(crypto_op_data *enc_data, CK_BYTE_PTR in, CK_ULONG inlen, CK_BYTE_PTR out, CK_ULONG_PTR outlen);

static sw_encrypt_data *sw_encrypt_data_new(void) {

    return (sw_encrypt_data *)calloc(1, sizeof(sw_encrypt_data));
}

static void sw_encrypt_data_free(sw_encrypt_data **enc_data) {

    if (!enc_data || !*enc_data) {
        return;
    }

    if ((*enc_data)->key) {
        EVP_PKEY_free((*enc_data)->key);
    }

    twist_free((*enc_data)->label);

    free(*enc_data);
    *enc_data = NULL;
}

encrypt_op_data *encrypt_op_data_new(void) {

    return (encrypt_op_data *)calloc(1, sizeof(encrypt_op_data));
}

void encrypt_op_data_free(encrypt_op_data **opdata) {

    if (opdata) {
        (*opdata)->use_sw ?
                sw_encrypt_data_free(&(*opdata)->cryptopdata.sw_enc_data) :
                tpm_opdata_free(&(*opdata)->cryptopdata.tpm_opdata);
        free(*opdata);
        *opdata = NULL;
    }
}

CK_RV sw_encrypt_data_init(mdetail *mdtl, CK_MECHANISM *mechanism, tobject *tobj, sw_encrypt_data **enc_data) {

    EVP_PKEY *pkey = NULL;
    CK_RV rv = ssl_util_tobject_to_evp(&pkey, tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    int padding = 0;
    rv = mech_get_padding(mdtl, mechanism, &padding);
    if (rv != CKR_OK) {
        return rv;
    }

    const EVP_MD *md = NULL;
    bool is_hashing_needed = false;
    rv = mech_is_hashing_needed(
            mdtl,
            mechanism,
            &is_hashing_needed);
    if (rv != CKR_OK) {
        return rv;
    }

    if (is_hashing_needed) {
        rv = mech_get_digester(mdtl, mechanism, &md);
        if (rv != CKR_OK) {
            return rv;
        }
    }

    twist label = NULL;
    rv = mech_get_label(mechanism, &label);
    if (rv != CKR_OK) {
        return rv;
    }

    sw_encrypt_data *d = sw_encrypt_data_new();
    if (!d) {
        LOGE("oom");
        twist_free(label);
        EVP_PKEY_free(pkey);
        return CKR_HOST_MEMORY;
    }

    d->key = pkey;
    d->padding = padding;
    d->label = label;
    d->md = md;

    *enc_data = d;

    return CKR_OK;
}

static CK_RV sw_encrypt(crypto_op_data *opdata,
        CK_BYTE_PTR ptext, CK_ULONG ptextlen,
        CK_BYTE_PTR ctext, CK_ULONG_PTR ctextlen) {
    assert(opdata);

    sw_encrypt_data *sw_enc_data = opdata->sw_enc_data;

    assert(sw_enc_data);
    assert(sw_enc_data->key);

    return ssl_util_encrypt(sw_enc_data->key,
            sw_enc_data->padding,
            sw_enc_data->label,
            sw_enc_data->md,
            ptext, ptextlen,
            ctext, ctextlen);
}

static CK_RV common_init_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, operation op, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key) {

    check_pointer(mechanism);

    LOGV("mechanism->mechanism: %lu\n"
            "mechanism->ulParameterLen: %lu\n"
            "mechanism->pParameter: %s",
            mechanism->mechanism,
            mechanism->ulParameterLen,
            mechanism->pParameter ? "set" : "(null)");

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    if (!supplied_opdata) {
        bool is_active = session_ctx_opdata_is_active(ctx);
        if (is_active) {
            return CKR_OPERATION_ACTIVE;
        }
    }

    tobject *tobj;
    CK_RV rv = token_load_object(tok, key, &tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    rv = object_mech_is_supported(tobj, mechanism);
    if (rv != CKR_OK) {
        tobject_user_decrement(tobj);
        return rv;
    }

    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tobj->attrs, CKA_CLASS);
    if (!a) {
        LOGE("Expected tobject to have attribute CKA_CLASS");
        return CKR_GENERAL_ERROR;
    }

    CK_OBJECT_CLASS obj_class = 0;
    rv = attr_CK_OBJECT_CLASS(a, &obj_class);
    if (rv != CKR_OK) {
        LOGE("Could not convert CKA_CLASS");
        return rv;
    }

    encrypt_op_data *opdata;
    if (!supplied_opdata) {
        opdata = encrypt_op_data_new();
        if (!opdata) {
            tobject_user_decrement(tobj);
            return CKR_HOST_MEMORY;
        }
    } else {
        opdata = supplied_opdata;
    }

    /*
     * Public Key Objects don't need to hit the TPM
     */
    if (obj_class == CKO_PUBLIC_KEY) {
        opdata->use_sw = true;
        rv = sw_encrypt_data_init(tok->mdtl, mechanism, tobj, &opdata->cryptopdata.sw_enc_data);
    } else {
        rv = mech_get_tpm_opdata(tok->mdtl,
                tok->tctx, mechanism, tobj,
                &opdata->cryptopdata.tpm_opdata);
    }

    if (rv != CKR_OK) {
        tobject_user_decrement(tobj);
        if (!supplied_opdata) {
            encrypt_op_data_free(&opdata);
        }
        return rv;
    }

    if (!supplied_opdata) {
        session_ctx_opdata_set(ctx, op, tobj, opdata, (opdata_free_fn)encrypt_op_data_free);
    }

    return CKR_OK;
}

static CK_RV common_update_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, operation op,
        CK_BYTE_PTR part, CK_ULONG part_len,
        CK_BYTE_PTR encrypted_part, CK_ULONG_PTR encrypted_part_len) {

    check_pointer(part);
    check_pointer(encrypted_part_len);

    CK_RV rv = CKR_GENERAL_ERROR;

    twist output = NULL;

    encrypt_op_data *opdata = NULL;
    if (!supplied_opdata) {
        rv = session_ctx_opdata_get(ctx, op, &opdata);
        if (rv != CKR_OK) {
            goto out;
        }

        rv = session_ctx_tobject_authenticated(ctx);
        if (rv != CKR_OK) {
            goto out;
        }
    } else {
        opdata = supplied_opdata;
    }

    /*
     * Public key crypto operations have the use_sw flag set. Currently,
     * they are restricted to working only with RSA AFAIK. Thus,
     * they only perform RSA Encrypt (RSA operation w/pub key). Thus, the
     * SW path will always call sw_encrypt.
     *
     * If we add proper EC support, we likely need a sw_decrypt interface
     * that does the right thing with respect to EC and RSA. For RSA it
     * should always call Encrypt and for EC, likely can do Encrypt.
     */
    crypto_op fop;
    switch(op) {
    case operation_encrypt:
        fop = opdata->use_sw ? sw_encrypt : tpm_encrypt;
        break;
    case operation_decrypt:
        fop = opdata->use_sw ? sw_encrypt : tpm_decrypt;
        break;
    default:
        return CKR_GENERAL_ERROR;
    }

    rv = fop(&opdata->cryptopdata, part, part_len,
            encrypted_part, encrypted_part_len);
    if (rv != CKR_OK) {
        goto out;
    }

    rv = CKR_OK;

out:
    twist_free(output);

    return rv;
}

static CK_RV common_final_op(session_ctx *ctx, encrypt_op_data *supplied_opdata, operation op,
        CK_BYTE_PTR last_part, CK_ULONG_PTR last_part_len) {

    /*
     * We have no use for these.
     */
    UNUSED(last_part);
    UNUSED(last_part_len);

    CK_RV rv = CKR_GENERAL_ERROR;

    /* nothing to do if opdata is supplied externally */
    if (supplied_opdata) {
        /* do not goto out, no opdata to clear */
        return CKR_OK;
    }

    encrypt_op_data *opdata = NULL;
    rv = session_ctx_opdata_get(ctx, op, &opdata);
    if (rv != CKR_OK) {
        return rv;
    }

    rv = session_ctx_tobject_authenticated(ctx);
    if (rv != CKR_OK) {
        return rv;
    }

    tobject *tobj = session_ctx_opdata_get_tobject(ctx);
    assert(tobj);
    tobj->is_authenticated = false;
    rv = tobject_user_decrement(tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    session_ctx_opdata_clear(ctx);

    return CKR_OK;
}

CK_RV encrypt_init_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {

    return common_init_op(ctx, supplied_opdata, operation_encrypt, mechanism, key);
}

CK_RV decrypt_init_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {

    return common_init_op(ctx, supplied_opdata, operation_decrypt, mechanism, key);
}

CK_RV encrypt_update_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR part, CK_ULONG part_len, CK_BYTE_PTR encrypted_part, CK_ULONG_PTR encrypted_part_len) {

    return common_update_op(ctx, supplied_opdata, operation_encrypt, part, part_len, encrypted_part, encrypted_part_len);
}

CK_RV decrypt_update_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR part, CK_ULONG part_len, CK_BYTE_PTR encrypted_part, CK_ULONG_PTR encrypted_part_len) {

    return common_update_op(ctx, supplied_opdata, operation_decrypt, part, part_len, encrypted_part, encrypted_part_len);
}

CK_RV encrypt_final_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR last_encrypted_part, CK_ULONG_PTR last_encrypted_part_len) {

    return common_final_op(ctx, supplied_opdata, operation_encrypt, last_encrypted_part, last_encrypted_part_len);
}

CK_RV decrypt_final_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR last_part, CK_ULONG_PTR last_part_len) {

    return common_final_op(ctx, supplied_opdata, operation_decrypt, last_part, last_part_len);
}

CK_RV decrypt_oneshot_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR encrypted_data, CK_ULONG encrypted_data_len, CK_BYTE_PTR data, CK_ULONG_PTR data_len) {

    CK_RV rv = decrypt_update_op(ctx, supplied_opdata, encrypted_data, encrypted_data_len,
            data, data_len);
    if (rv != CKR_OK || !data) {
        return rv;
    }

    return decrypt_final_op(ctx, supplied_opdata, NULL, NULL);
}

CK_RV encrypt_oneshot_op (session_ctx *ctx, encrypt_op_data *supplied_opdata, CK_BYTE_PTR data, CK_ULONG data_len, CK_BYTE_PTR encrypted_data, CK_ULONG_PTR encrypted_data_len) {

    CK_RV rv = encrypt_update_op (ctx, supplied_opdata, data, data_len, encrypted_data, encrypted_data_len);
    if (rv != CKR_OK || !encrypted_data) {
        return rv;
    }

    return encrypt_final_op(ctx, supplied_opdata, NULL, NULL);
}
