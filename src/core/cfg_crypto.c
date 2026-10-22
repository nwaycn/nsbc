#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_common.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* REQ-CFG-009：敏感字段 AES-256-GCM
 *
 * enc 密文格式（v1）：base64( 0x01 || salt[16] || iv[12] || ciphertext || tag[16])
 * 数据密钥：SHA-256(master_key || salt)（32B，AES-256）；
 * 主密钥优先级：环境变量 NSBC_MASTER_KEY > 本地密钥文件
 * （NSBC_MASTER_KEY_FILE，默认 /etc/nsbc/master.key，权限 0400）。
 * KMS/外部密钥服务接入点在生产环境以同名环境变量注入实现，无需改本文件。
 * 主密钥缺失或密文不匹配：拒绝（不降级明文）。
 */

#ifdef NSBC_WITH_OPENSSL
#include "openssl_compat.h"   /* 版本兼容层（自动 include evp.h/sha.h + new/free 回退）*/

static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_key[256];
static int g_key_loaded;

const char *cfg_master_key(void)
{
    pthread_mutex_lock(&key_lock);
    if (!g_key_loaded) {
        const char *env = getenv("NSBC_MASTER_KEY");
        if (env && *env) {
            snprintf(g_key, sizeof(g_key), "%s", env);
        } else {
            const char *kf = getenv("NSBC_MASTER_KEY_FILE");
            if (!kf || !*kf)
                kf = "/etc/nsbc/master.key";
            FILE *fp = fopen(kf, "re");
            if (fp) {
                size_t n = fread(g_key, 1, sizeof(g_key) - 1, fp);
                fclose(fp);
                while (n && (g_key[n - 1] == '\n' || g_key[n - 1] == '\r'))
                    --n;
                g_key[n] = '\0';
            }
        }
        g_key_loaded = 1;
    }
    pthread_mutex_unlock(&key_lock);
    return g_key[0] ? g_key : NULL;
}

static int b64_decode(const char *in, uint8_t *out, size_t outsz, size_t *n)
{
    static const char *tab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    signed char d[256];
    memset(d, -1, sizeof(d));
    for (int i = 0; i < 64; ++i)
        d[(unsigned char)tab[i]] = (signed char)i;
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p && *p != '='; ++p) {
        signed char x = d[(unsigned char)*p];
        if (x < 0) {
            if (*p == '\n' || *p == '\r')
                continue;
            return NSBC_PROTO;
        }
        v = (v << 6) | (uint32_t)x;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsz)
                return NSBC_NOMEM;
            out[o++] = (uint8_t)((v >> bits) & 0xff);
        }
    }
    *n = o;
    return NSBC_OK;
}

#define V1_HDR 1
#define V1_SALT 16
#define V1_IV 12
#define V1_TAG 16

int cfg_secret_decrypt_gcm(const char *b64, char *out, size_t outsz)
{
    static uint8_t raw[8192];
    size_t raw_n = 0;
    if (b64_decode(b64, raw, sizeof(raw) - 1, &raw_n) != NSBC_OK)
        return NSBC_PROTO;
    if (raw_n < V1_HDR + V1_SALT + V1_IV + V1_TAG + 1)
        return NSBC_PROTO;
    if (raw[0] != 1)
        return NSBC_NOTSUP;
    const uint8_t *salt = raw + V1_HDR;
    const uint8_t *iv = salt + V1_SALT;
    size_t ct_n = raw_n - V1_HDR - V1_SALT - V1_IV - V1_TAG;
    const uint8_t *ct = iv + V1_IV;
    const uint8_t *tag = ct + ct_n;

    const char *mk = cfg_master_key();
    if (!mk)
        return NSBC_AUTH;
    uint8_t dk[32];
    {
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        EVP_DigestInit_ex(m, EVP_sha256(), NULL);
        EVP_DigestUpdate(m, mk, strlen(mk));
        EVP_DigestUpdate(m, salt, V1_SALT);
        unsigned int dl = 0;
        EVP_DigestFinal_ex(m, dk, &dl);
        EVP_MD_CTX_free(m);
    }
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int rc = NSBC_ERR;
    if (!c)
        return NSBC_NOMEM;
    if (ct_n + 1 > outsz) {
        rc = NSBC_NOSPC;
        goto out;
    }
    int outl = 0, fl = 0;
    if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, V1_IV, NULL) != 1 ||
        EVP_DecryptInit_ex(c, NULL, NULL, dk, iv) != 1) {
        goto out;
    }
    if (EVP_DecryptUpdate(c, (unsigned char *)out, &outl, ct,
                          (int)ct_n) != 1 ||
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, V1_TAG,
                            (void *)tag) != 1) {
        goto out;
    }
    if (EVP_DecryptFinal_ex(c, (unsigned char *)out + outl, &fl) == 1) {
        out[outl + fl] = '\0';
        rc = NSBC_OK;
    } else {
        rc = NSBC_AUTH; /* 标签校验失败：密文损坏或主密钥不匹配 */
    }
out:
    EVP_CIPHER_CTX_free(c);
    return rc;
}

#else /* ---------- 未启用 OpenSSL ---------- */

const char *cfg_master_key(void)
{
    const char *k = getenv("NSBC_MASTER_KEY");
    return (k && *k) ? k : NULL;
}
int cfg_secret_decrypt_gcm(const char *b64, char *out, size_t outsz)
{
    (void)b64;
    (void)out;
    (void)outsz;
    return NSBC_NOTSUP; /* P0 生产构建必须带 NSBC_WITH_OPENSSL */
}
#endif
