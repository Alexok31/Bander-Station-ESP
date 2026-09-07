#ifndef __ALAC__DECOMP_H
#define __ALAC__DECOMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alac_file alac_file;

alac_file *alac_create(int samplesize, int numchannels);
void alac_decode_frame(alac_file *alac, unsigned char *inbuffer, void *outbuffer, int *outputsize);
void alac_set_info(alac_file *alac, char *inputbuffer);
void alac_allocate_buffers(alac_file *alac);
void alac_free(alac_file *alac);

struct alac_file {
    unsigned char *input_buffer;
    int input_buffer_bitaccumulator;
    int samplesize;
    int numchannels;
    int bytespersample;
    int32_t *predicterror_buffer_a;
    int32_t *predicterror_buffer_b;
    int32_t *outputsamples_buffer_a;
    int32_t *outputsamples_buffer_b;
    int32_t *uncompressed_bytes_buffer_a;
    int32_t *uncompressed_bytes_buffer_b;
    uint32_t setinfo_max_samples_per_frame;
    uint8_t setinfo_7a;
    uint8_t setinfo_sample_size;
    uint8_t setinfo_rice_historymult;
    uint8_t setinfo_rice_initialhistory;
    uint8_t setinfo_rice_kmodifier;
    uint8_t setinfo_7f;
    uint16_t setinfo_80;
    uint32_t setinfo_82;
    uint32_t setinfo_86;
    uint32_t setinfo_8a_rate;
};

#ifdef __cplusplus
}
#endif

#endif
