#ifndef MT_FFI_H
#define MT_FFI_H

#include "../common/callback.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mt_encode_driver_support();

void *mt_new_encoder(void *device, int64_t luid, int32_t data_format,
                     int32_t width, int32_t height, int32_t bitrate,
                     int32_t framerate, int32_t gop);

int mt_encode(void *encoder, void *texture, EncodeCallback callback, void *obj,
              int64_t ms);

int mt_destroy_encoder(void *encoder);

int mt_test_encode(int64_t *out_luids, int32_t *out_vendors,
                   int32_t max_desc_num, int32_t *out_desc_num,
                   int32_t data_format, int32_t width, int32_t height,
                   int32_t kbs, int32_t framerate, int32_t gop,
                   const int64_t *excluded_luids,
                   const int32_t *exclude_formats, int32_t exclude_count);

int mt_set_bitrate(void *encoder, int32_t kbs);

int mt_set_framerate(void *encoder, int32_t framerate);

#ifdef __cplusplus
}
#endif

#endif // MT_FFI_H
