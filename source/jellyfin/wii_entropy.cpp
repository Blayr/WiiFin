/**
 * wii_entropy.cpp
 * Implements mbedtls_hardware_poll() for the Nintendo Wii using the Broadway
 * CPU timebase register (libogc gettime()), required by MBEDTLS_ENTROPY_HARDWARE_ALT.
 */

#include <mbedtls/build_info.h>

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)

#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <ogc/lwp_watchdog.h>

extern "C" {

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    *olen = 0;
    size_t i = 0;
    uint64_t prev = gettime();
    while (i < len) {
        uint64_t t;
        /* Spin until at least 128 ticks have elapsed to force LSB variation
         * and capture environmental jitter (IPC, cache misses, etc.). */
        do { t = gettime(); } while ((t - prev) < 128);
        uint64_t delta = t - prev;
        prev = t;
        /* XOR the raw timer (high entropy in LSBs) with the inter-call delta
         * (captures timing jitter) for each output byte. */
        for (size_t j = 0; j < sizeof(t) && i < len; j++, i++)
            output[i] = ((const uint8_t*)&t)[j] ^ ((const uint8_t*)&delta)[j];
    }
    *olen = len;
    return 0;
}

} // extern "C"

#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */
