#include "AirPlayPriv.h"

#include <WiFi.h>
#include <cstring>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>

// Добре відомий RSA-ключ AirPort Express. Його використовують усі відкриті
// AirPlay 1 приймачі (shairport, squeezelite). Це не секрет користувача.
// Ключ з abrasive/shairport (MIT), common.c — стандартний RAOP AirPort Express.
static const char kAirportPem[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----";

static mbedtls_pk_context s_pk;
static bool s_pk_ok = false;

static int rng_cb(void*, unsigned char* out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}

bool airplay_crypto_begin() {
    if (s_pk_ok) {
        return true;
    }
    mbedtls_pk_init(&s_pk);
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    const int err = mbedtls_pk_parse_key(
        &s_pk, reinterpret_cast<const unsigned char*>(kAirportPem), strlen(kAirportPem) + 1,
        nullptr, 0, rng_cb, nullptr);
#else
    const int err = mbedtls_pk_parse_key(
        &s_pk, reinterpret_cast<const unsigned char*>(kAirportPem), strlen(kAirportPem) + 1,
        nullptr, 0);
#endif
    if (err != 0) {
        Serial.printf("[AirPlay] RSA key parse %d\n", err);
        return false;
    }
    s_pk_ok = true;
    return true;
}

size_t airplay_b64_decode(const char* s, uint8_t* out, size_t outmax) {
    char tmp[800];
    size_t n = 0;
    for (; s && *s && n + 1 < sizeof(tmp); ++s) {
        if (*s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') {
            tmp[n++] = *s;
        }
    }
    while ((n & 3) && n + 1 < sizeof(tmp)) {
        tmp[n++] = '=';
    }
    tmp[n] = 0;
    size_t olen = 0;
    if (mbedtls_base64_decode(out, outmax, &olen, reinterpret_cast<const unsigned char*>(tmp), n) !=
        0) {
        return 0;
    }
    return olen;
}

bool airplay_rsa_oaep_decrypt(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen) {
    if (!s_pk_ok || inlen != 256) {
        return false;
    }
    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(s_pk);
    if (!rsa) {
        return false;
    }
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    size_t olen = 0;
    const int err = mbedtls_rsa_pkcs1_decrypt(rsa, rng_cb, nullptr, &olen, in, out, 256);
    if (err != 0 || olen < 16) {
        Serial.printf("[AirPlay] OAEP decrypt %d olen=%u\n", err, (unsigned)olen);
        return false;
    }
    if (outlen) {
        *outlen = olen;
    }
    return true;
}

bool airplay_apple_response(const uint8_t* challenge, size_t clen, char* b64, size_t b64max) {
    if (!s_pk_ok || !challenge || clen == 0 || clen > 32) {
        return false;
    }
    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(s_pk);
    if (!rsa) {
        return false;
    }
    uint8_t plain[32];
    memset(plain, 0, sizeof(plain));
    memcpy(plain, challenge, clen);
    IPAddress ip = WiFi.localIP();
    const uint8_t ipb[4] = {ip[0], ip[1], ip[2], ip[3]};
    if (clen + 4 + 6 <= 32) {
        memcpy(plain + clen, ipb, 4);
        memcpy(plain + clen + 4, g_ap_mac, 6);
    }

    uint8_t padded[256];
    memset(padded, 0xff, sizeof(padded));
    padded[0] = 0x00;
    padded[1] = 0x01;
    const size_t data_off = 256 - 32;
    padded[data_off - 1] = 0x00;
    memcpy(padded + data_off, plain, 32);

    uint8_t cipher[256];
    const int err = mbedtls_rsa_private(rsa, rng_cb, nullptr, padded, cipher);
    if (err != 0) {
        Serial.printf("[AirPlay] Apple-Response RSA %d\n", err);
        return false;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), b64max, &olen, cipher, 256) !=
        0) {
        return false;
    }
    b64[olen] = 0;
    return true;
}

static mbedtls_aes_context s_aes;
static bool s_aes_ready = false;

void airplay_aes_prepare() {
    if (s_aes_ready) {
        mbedtls_aes_free(&s_aes);
        s_aes_ready = false;
    }
    mbedtls_aes_init(&s_aes);
    if (mbedtls_aes_setkey_dec(&s_aes, g_ap.aes_key, 128) != 0) {
        mbedtls_aes_free(&s_aes);
        return;
    }
    s_aes_ready = true;
}

void airplay_aes_decrypt(const uint8_t* in, size_t n, uint8_t* out) {
    const size_t aligned = n & ~((size_t)15);
    if (aligned >= 16) {
        if (!s_aes_ready) {
            airplay_aes_prepare();
        }
        if (s_aes_ready) {
            uint8_t iv[16];
            memcpy(iv, g_ap.aes_iv, 16);
            mbedtls_aes_crypt_cbc(&s_aes, MBEDTLS_AES_DECRYPT, aligned, iv, in, out);
        } else {
            memcpy(out, in, aligned);
        }
    }
    if (n > aligned) {
        memcpy(out + aligned, in + aligned, n - aligned);
    }
}
