#ifndef MT_ENCODE_API_H
#define MT_ENCODE_API_H

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace mtapi {

using Status = int32_t;
using InputPtr = void *;
using OutputPtr = void *;

constexpr uint32_t API_VERSION = 0x01020000;
constexpr uint32_t INFINITE_GOP_LENGTH = 0xFFFF;

constexpr Status SUCCESS = 0;
constexpr Status ERR_NEED_MORE_INPUT = 9;

constexpr uint32_t CODEC_H264 = 0x1;
constexpr uint32_t PROFILE_MAIN_H264 = 0x3;
constexpr uint32_t PRESET_DEFAULT = 0x1;
constexpr uint32_t RC_CBR = 0x1;

constexpr uint32_t PIC_TYPE_I = 0x0;
constexpr uint32_t PIC_TYPE_IDR = 0x3;
constexpr uint32_t BUFFER_FORMAT_NV12 = 0x1;
constexpr uint32_t RESOURCE_DIRECTX = 0x0;
constexpr uint32_t DEVICE_DIRECTX = 0x0;

constexpr uint32_t LEVEL_AUTOSELECT = 0x0;
constexpr uint32_t VUI_VIDEO_FORMAT_COMPONENT = 0x0;
constexpr uint32_t VUI_COLOR_SMPTE170M = 0x6;

struct Qp {
  uint32_t inter_p;
  uint32_t inter_b;
  uint32_t intra;
};

struct RcParams {
  uint32_t version;
  uint32_t rate_control_mode;
  Qp const_qp;
  uint32_t average_bit_rate;
  uint32_t max_bit_rate;
  uint32_t reserved0;
  uint32_t reserved1[56];
  void *reserved2[64];
};

struct H264VuiParams {
  uint32_t overscan_info_present;
  uint32_t overscan_info;
  uint32_t video_signal_type_present;
  uint32_t video_format;
  uint32_t video_full_range;
  uint32_t colour_description_present;
  uint32_t colour_primaries;
  uint32_t transfer_characteristics;
  uint32_t colour_matrix;
  uint32_t timing_info_present;
  uint32_t num_units_in_ticks;
  uint32_t time_scale;
  uint32_t reserved[16];
};

struct H264Config {
  uint32_t level;
  uint32_t idr_period;
  H264VuiParams vui;
  uint32_t reserved1[226];
  void *reserved2[64];
};

struct CodecConfig {
  H264Config h264;
  uint8_t remaining_codec_data[9216];
};

struct Config {
  uint32_t version;
  uint32_t profile_id;
  uint32_t gop_length;
  int32_t frame_interval_p;
  RcParams rc;
  CodecConfig codec;
  uint32_t b_depth;
  uint32_t refs;
  uint32_t intra_refresh_mode;
  uint32_t intra_refresh_arg;
  uint32_t repeat_headers;
  uint32_t reserved1[247];
  void *reserved2[64];
};

struct InitParams {
  uint32_t version;
  uint32_t encode_id;
  uint32_t preset_id;
  uint32_t encode_width;
  uint32_t encode_height;
  uint32_t frame_rate_num;
  uint32_t frame_rate_den;
  uint32_t enable_encode_async;
  Config *encode_config;
  uint32_t max_encode_width;
  uint32_t max_encode_height;
  char *device_name;
  uint32_t reserved1[246];
  void *reserved2[62];
};

struct ReconfigureParams {
  uint32_t version;
  uint32_t reserved;
  InitParams reinit_params;
};

struct PresetConfig {
  uint32_t version;
  uint32_t reserved;
  Config preset;
  uint32_t reserved1[254];
  void *reserved2[64];
};

struct CreateOutputBuffer {
  uint32_t version;
  uint32_t reserved;
  OutputPtr output_buffer;
  uint32_t reserved1[62];
  void *reserved2[63];
};

struct PicParams {
  uint32_t version;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t input_pitch;
  uint32_t encode_pic_flags;
  uint32_t buffer_format;
  InputPtr input_buffer;
  OutputPtr output_buffer;
  void *completion_event;
  uint32_t input_buffer_type_and_reserved;
  uint32_t roi_number;
  void *roi;
  uint64_t input_timestamp;
  uint64_t input_duration;
  uint32_t reserved1[244];
  void *reserved2[60];
};

struct LockBuffer {
  uint32_t version;
  uint32_t frame_index;
  uint32_t picture_type;
  uint32_t output_size;
  void *output_buffer_ptr;
  void *locked_output_buffer;
  uint32_t timeout_ms;
  uint32_t reserved;
  uint64_t output_timestamp;
  uint64_t output_duration;
  int64_t decode_timestamp;
  uint32_t reserved1[244];
  void *reserved2[62];
};

struct MapResource {
  uint32_t version;
  uint32_t resource_type;
  InputPtr resource_to_map;
  InputPtr mapped_resource;
  uint32_t reserved1[254];
  void *reserved2[62];
};

struct CreateEncoderParams {
  uint32_t version;
  uint32_t device_type;
  void *device;
  uint32_t reserved1[254];
  void *reserved2[63];
};

using CreateEncoderFn = Status(WINAPI *)(CreateEncoderParams *, void **);
using GetPresetConfigFn = Status(WINAPI *)(void *, uint32_t, uint32_t,
                                           PresetConfig *);
using InitEncoderFn = Status(WINAPI *)(void *, InitParams *);
using ReconfigureEncoderFn = Status(WINAPI *)(void *, ReconfigureParams *);
using CreateOutputBufferFn = Status(WINAPI *)(void *, CreateOutputBuffer *);
using ReleaseOutputBufferFn = Status(WINAPI *)(void *, OutputPtr);
using EncodeFrameFn = Status(WINAPI *)(void *, PicParams *);
using LockOutputBufferFn = Status(WINAPI *)(void *, LockBuffer *);
using UnlockOutputBufferFn = Status(WINAPI *)(void *, OutputPtr);
using MapResourceFn = Status(WINAPI *)(void *, MapResource *);
using UnmapResourceFn = Status(WINAPI *)(void *, InputPtr);
using ReleaseEncoderFn = Status(WINAPI *)(void *);

struct FunctionList {
  uint32_t version;
  uint32_t sdk_version;
  CreateEncoderFn create_encoder;
  GetPresetConfigFn get_preset_config;
  InitEncoderFn init_encoder;
  CreateOutputBufferFn create_output_buffer;
  ReleaseOutputBufferFn release_output_buffer;
  EncodeFrameFn encode_frame;
  LockOutputBufferFn lock_output_buffer;
  UnlockOutputBufferFn unlock_output_buffer;
  MapResourceFn map_resource;
  UnmapResourceFn unmap_resource;
  ReleaseEncoderFn release_encoder;
  ReconfigureEncoderFn reconfigure_encoder;
  void *reserved1[244];
};

using GetMaxSupportedVersionFn = Status(WINAPI *)(uint32_t *);
using CreateInstanceFn = Status(WINAPI *)(FunctionList *);

static_assert(sizeof(void *) == 8, "MTEncode backend requires a 64-bit target");
static_assert(sizeof(Status) == 4, "Unexpected MTEncode status ABI layout");
static_assert(sizeof(RcParams) == 768, "Unexpected MTEncode RC ABI layout");
static_assert(sizeof(H264Config) == 1536,
              "Unexpected MTEncode H.264 ABI layout");
static_assert(sizeof(CodecConfig) == 10752,
              "Unexpected MTEncode codec ABI layout");
static_assert(sizeof(Config) == 13056,
              "Unexpected MTEncode config ABI layout");
static_assert(sizeof(InitParams) == 1536,
              "Unexpected MTEncode init ABI layout");
static_assert(sizeof(PresetConfig) == 14592,
              "Unexpected MTEncode preset ABI layout");
static_assert(sizeof(PicParams) == 1536,
              "Unexpected MTEncode picture ABI layout");
static_assert(sizeof(LockBuffer) == 1536,
              "Unexpected MTEncode lock ABI layout");
static_assert(sizeof(MapResource) == 1536,
              "Unexpected MTEncode map ABI layout");
static_assert(sizeof(CreateEncoderParams) == 1536,
              "Unexpected MTEncode create ABI layout");
static_assert(sizeof(FunctionList) == 2056,
              "Unexpected MTEncode function-list ABI layout");
static_assert(offsetof(Config, rc) == 16,
              "Unexpected MTEncode rate-control ABI offset");
static_assert(offsetof(Config, codec) == 784,
              "Unexpected MTEncode codec ABI offset");
static_assert(offsetof(InitParams, encode_config) == 32,
              "Unexpected MTEncode config-pointer ABI offset");

} // namespace mtapi

#endif // MT_ENCODE_API_H
