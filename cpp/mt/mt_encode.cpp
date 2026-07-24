#include "mt_ffi.h"

#include "mt_encode_api.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "common.h"
#include "system.h"
#include "util.h"

#define LOG_MODULE "MTENC"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t BUFFER_COUNT = 3;

void log_status(const char *operation, mtapi::Status status) {
  LOG_ERROR(std::string(operation) + " failed, status=" +
            std::to_string(status));
}

bool load_api(HMODULE &module, mtapi::FunctionList &api, bool log_errors) {
  module = LoadLibraryW(L"mtencodeapi64.dll");
  if (!module) {
    if (log_errors) {
      LOG_ERROR(std::string("LoadLibrary(mtencodeapi64.dll) failed, error=") +
                std::to_string(GetLastError()));
    }
    return false;
  }

  auto get_max_supported_version =
      reinterpret_cast<mtapi::GetMaxSupportedVersionFn>(
          GetProcAddress(module, "MTEncodeAPIGetMaxSupportedVersion"));
  auto create_instance = reinterpret_cast<mtapi::CreateInstanceFn>(
      GetProcAddress(module, "MTEncodeAPICreateInstance"));
  if (!get_max_supported_version || !create_instance) {
    if (log_errors) {
      LOG_ERROR(std::string("Required MTEncode API exports are missing"));
    }
    FreeLibrary(module);
    module = nullptr;
    return false;
  }

  uint32_t max_supported_version = 0;
  mtapi::Status status = get_max_supported_version(&max_supported_version);
  if (status != mtapi::SUCCESS ||
      max_supported_version < mtapi::API_VERSION) {
    if (log_errors) {
      LOG_ERROR(std::string("MTEncode API version 0x01020000 is not supported; ") +
                "driver maximum=" + std::to_string(max_supported_version) +
                ", status=" + std::to_string(status));
    }
    FreeLibrary(module);
    module = nullptr;
    return false;
  }

  api = {};
  api.version = mtapi::API_VERSION;
  status = create_instance(&api);
  if (status != mtapi::SUCCESS || !api.create_encoder ||
      !api.get_preset_config || !api.init_encoder ||
      !api.create_output_buffer || !api.release_output_buffer ||
      !api.encode_frame || !api.lock_output_buffer ||
      !api.unlock_output_buffer || !api.map_resource ||
      !api.unmap_resource || !api.release_encoder ||
      !api.reconfigure_encoder) {
    if (log_errors) {
      LOG_ERROR(std::string("MTEncode function table is incomplete, status=") +
                std::to_string(status));
    }
    api = {};
    FreeLibrary(module);
    module = nullptr;
    return false;
  }

  return true;
}

class MtEncoder {
public:
  MtEncoder(void *device, int64_t luid, int32_t width, int32_t height,
            int32_t kbs, int32_t framerate, int32_t gop)
      : device_(static_cast<ID3D11Device *>(device)), luid_(luid),
        width_(width), height_(height), kbs_(kbs), framerate_(framerate),
        gop_(gop) {}

  ~MtEncoder() { destroy(); }

  bool init() {
    if (width_ <= 0 || height_ <= 0 || width_ % 2 != 0 || height_ % 2 != 0 ||
        kbs_ <= 0 || framerate_ <= 0) {
      LOG_ERROR(std::string("Invalid MTEncode dimensions or rate settings"));
      return false;
    }

    if (!load_api(module_, api_, true)) {
      return false;
    }

    native_ = std::make_unique<NativeDevice>();
    if (!native_->Init(luid_, device_)) {
      LOG_ERROR(std::string("Failed to create the MT D3D11 device context"));
      return false;
    }
    if (native_->GetVendor() != ADAPTER_VENDOR_MT) {
      LOG_ERROR(std::string("The selected D3D11 device is not a Moore Threads adapter"));
      return false;
    }

    mtapi::CreateEncoderParams create_params = {};
    create_params.version = mtapi::API_VERSION;
    create_params.device_type = mtapi::DEVICE_DIRECTX;
    create_params.device = native_->device_.Get();
    mtapi::Status status = api_.create_encoder(&create_params, &encoder_);
    if (status != mtapi::SUCCESS || !encoder_) {
      log_status("mtEncCreateEncoder", status);
      return false;
    }

    mtapi::PresetConfig preset = {};
    preset.version = mtapi::API_VERSION;
    preset.preset.version = mtapi::API_VERSION;
    status = api_.get_preset_config(encoder_, mtapi::CODEC_H264,
                                    mtapi::PRESET_DEFAULT, &preset);
    if (status != mtapi::SUCCESS) {
      log_status("mtEncGetPresetConfig", status);
      return false;
    }

    config_ = preset.preset;
    configure_h264();

    init_params_ = {};
    init_params_.version = mtapi::API_VERSION;
    init_params_.encode_id = mtapi::CODEC_H264;
    init_params_.preset_id = mtapi::PRESET_DEFAULT;
    init_params_.encode_width = static_cast<uint32_t>(width_);
    init_params_.encode_height = static_cast<uint32_t>(height_);
    init_params_.frame_rate_num = static_cast<uint32_t>(framerate_);
    init_params_.frame_rate_den = 1;
    init_params_.enable_encode_async = 1;
    init_params_.encode_config = &config_;
    init_params_.max_encode_width = static_cast<uint32_t>(width_);
    init_params_.max_encode_height = static_cast<uint32_t>(height_);

    status = api_.init_encoder(encoder_, &init_params_);
    if (status != mtapi::SUCCESS) {
      log_status("mtEncInitEncoder", status);
      return false;
    }

    if (!allocate_buffers()) {
      return false;
    }

    initialized_ = true;
    return true;
  }

  int encode(void *texture, EncodeCallback callback, void *obj, int64_t ms) {
    if (!initialized_ || !texture || pending_.size() >= BUFFER_COUNT) {
      return -1;
    }

    size_t slot = next_slot_;
    if (mapped_resources_[slot]) {
      LOG_ERROR(std::string("MTEncode input slot is still mapped"));
      return -1;
    }

    auto source = static_cast<ID3D11Texture2D *>(texture);
    if (!native_->BgraToNv12(source, input_textures_[slot].Get(), width_,
                             height_, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                             DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601)) {
      LOG_ERROR(std::string("Failed to convert the captured texture to NV12"));
      return -1;
    }

    mtapi::MapResource map = {};
    map.version = mtapi::API_VERSION;
    map.resource_type = mtapi::RESOURCE_DIRECTX;
    map.resource_to_map = input_textures_[slot].Get();
    mtapi::Status status = api_.map_resource(encoder_, &map);
    if (status != mtapi::SUCCESS || !map.mapped_resource) {
      log_status("mtEncMapResource", status);
      return -1;
    }
    mapped_resources_[slot] = map.mapped_resource;
    timestamps_[slot] = ms;

    ResetEvent(events_[slot]);
    mtapi::PicParams picture = {};
    picture.version = mtapi::API_VERSION;
    picture.input_width = static_cast<uint32_t>(width_);
    picture.input_height = static_cast<uint32_t>(height_);
    picture.input_pitch = static_cast<uint32_t>(width_);
    picture.buffer_format = mtapi::BUFFER_FORMAT_NV12;
    picture.input_buffer = map.mapped_resource;
    picture.output_buffer = output_buffers_[slot];
    picture.completion_event = events_[slot];
    picture.input_timestamp = static_cast<uint64_t>(ms);
    picture.input_duration = static_cast<uint64_t>(1000 / framerate_);

    status = api_.encode_frame(encoder_, &picture);
    if (status != mtapi::SUCCESS && status != mtapi::ERR_NEED_MORE_INPUT) {
      log_status("mtEncEncodeFrame", status);
      unmap_slot(slot);
      return -1;
    }

    pending_.push_back(slot);
    next_slot_ = (next_slot_ + 1) % BUFFER_COUNT;
    if (status == mtapi::ERR_NEED_MORE_INPUT) {
      return 0;
    }

    return collect_one(callback, obj);
  }

  int set_bitrate(int32_t kbs) {
    if (!initialized_ || kbs <= 0 || !pending_.empty()) {
      return -1;
    }
    uint32_t old_average = config_.rc.average_bit_rate;
    uint32_t old_max = config_.rc.max_bit_rate;
    set_rate_control(kbs);
    if (!reconfigure()) {
      config_.rc.average_bit_rate = old_average;
      config_.rc.max_bit_rate = old_max;
      return -1;
    }
    kbs_ = kbs;
    return 0;
  }

  int set_framerate(int32_t framerate) {
    if (!initialized_ || framerate <= 0 || !pending_.empty()) {
      return -1;
    }
    uint32_t old_framerate = init_params_.frame_rate_num;
    init_params_.frame_rate_num = static_cast<uint32_t>(framerate);
    if (!reconfigure()) {
      init_params_.frame_rate_num = old_framerate;
      return -1;
    }
    framerate_ = framerate;
    return 0;
  }

  void *test_texture() {
    if (!native_ || !native_->EnsureTexture(width_, height_)) {
      return nullptr;
    }
    return native_->GetCurrentTexture();
  }

private:
  void configure_h264() {
    config_.version = mtapi::API_VERSION;
    config_.profile_id = mtapi::PROFILE_MAIN_H264;
    config_.frame_interval_p = 1;
    config_.gop_length =
        gop_ > 0 && gop_ < static_cast<int32_t>(mtapi::INFINITE_GOP_LENGTH)
            ? static_cast<uint32_t>(gop_)
            : mtapi::INFINITE_GOP_LENGTH;
    config_.repeat_headers = 1;
    config_.rc.version = mtapi::API_VERSION;
    config_.rc.rate_control_mode = mtapi::RC_CBR;
    set_rate_control(kbs_);

    mtapi::H264Config &h264 = config_.codec.h264;
    h264.level = mtapi::LEVEL_AUTOSELECT;
    h264.idr_period = config_.gop_length;
    h264.vui.video_signal_type_present = 1;
    h264.vui.video_format = mtapi::VUI_VIDEO_FORMAT_COMPONENT;
    h264.vui.video_full_range = 0;
    h264.vui.colour_description_present = 1;
    h264.vui.colour_primaries = mtapi::VUI_COLOR_SMPTE170M;
    h264.vui.transfer_characteristics = mtapi::VUI_COLOR_SMPTE170M;
    h264.vui.colour_matrix = mtapi::VUI_COLOR_SMPTE170M;
  }

  void set_rate_control(int32_t kbs) {
    uint64_t bitrate = static_cast<uint64_t>(kbs) * 1000;
    config_.rc.average_bit_rate = static_cast<uint32_t>(std::min<uint64_t>(
        bitrate, std::numeric_limits<uint32_t>::max()));
    config_.rc.max_bit_rate = config_.rc.average_bit_rate;
  }

  bool allocate_buffers() {
    input_textures_.resize(BUFFER_COUNT);
    output_buffers_.assign(BUFFER_COUNT, nullptr);
    mapped_resources_.assign(BUFFER_COUNT, nullptr);
    events_.assign(BUFFER_COUNT, nullptr);
    timestamps_.assign(BUFFER_COUNT, 0);

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = static_cast<UINT>(width_);
    texture_desc.Height = static_cast<UINT>(height_);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_NV12;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                             D3D11_BIND_SHADER_RESOURCE |
                             D3D11_BIND_VIDEO_ENCODER;

    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
      HRESULT hr = native_->device_->CreateTexture2D(
          &texture_desc, nullptr, input_textures_[i].ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        LOG_ERROR(std::string("CreateTexture2D(NV12) failed, hr=") +
                  std::to_string(hr));
        return false;
      }

      mtapi::CreateOutputBuffer output = {};
      output.version = mtapi::API_VERSION;
      mtapi::Status status = api_.create_output_buffer(encoder_, &output);
      if (status != mtapi::SUCCESS || !output.output_buffer) {
        log_status("mtEncCreateOutputBuffer", status);
        return false;
      }
      output_buffers_[i] = output.output_buffer;

      events_[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!events_[i]) {
        LOG_ERROR(std::string("CreateEvent failed, error=") +
                  std::to_string(GetLastError()));
        return false;
      }
    }
    return true;
  }

  int collect_one(EncodeCallback callback, void *obj) {
    if (pending_.empty()) {
      return -1;
    }

    size_t slot = pending_.front();
    DWORD wait_result = WaitForSingleObject(events_[slot], ENCODE_TIMEOUT_MS);
    if (wait_result != WAIT_OBJECT_0) {
      LOG_ERROR(std::string("Waiting for MTEncode completion failed, result=") +
                std::to_string(wait_result));
      return -1;
    }

    mtapi::LockBuffer lock = {};
    lock.version = mtapi::API_VERSION;
    lock.timeout_ms = ENCODE_TIMEOUT_MS;
    mtapi::Status status = api_.lock_output_buffer(encoder_, &lock);
    if (status != mtapi::SUCCESS) {
      log_status("mtEncLockOutputBuffer", status);
      return -1;
    }

    int result = -1;
    if (lock.output_buffer_ptr && lock.output_size > 0 &&
        lock.output_size <= static_cast<uint32_t>(
                                std::numeric_limits<int32_t>::max())) {
      int32_t key = lock.picture_type == mtapi::PIC_TYPE_IDR ||
                            lock.picture_type == mtapi::PIC_TYPE_I
                        ? 1
                        : 0;
      if (callback) {
        callback(static_cast<const uint8_t *>(lock.output_buffer_ptr),
                 static_cast<int32_t>(lock.output_size), key, obj,
                 timestamps_[slot]);
      }
      result = 0;
    }

    if (lock.locked_output_buffer) {
      status = api_.unlock_output_buffer(encoder_, lock.locked_output_buffer);
      if (status != mtapi::SUCCESS) {
        log_status("mtEncUnlockOutputBuffer", status);
        result = -1;
      }
    }
    unmap_slot(slot);
    pending_.pop_front();
    return result;
  }

  void unmap_slot(size_t slot) {
    if (slot < mapped_resources_.size() && mapped_resources_[slot]) {
      mtapi::Status status =
          api_.unmap_resource(encoder_, mapped_resources_[slot]);
      if (status != mtapi::SUCCESS) {
        log_status("mtEncUnmapResource", status);
      }
      mapped_resources_[slot] = nullptr;
    }
  }

  bool reconfigure() {
    mtapi::ReconfigureParams params = {};
    params.version = mtapi::API_VERSION;
    params.reinit_params = init_params_;
    params.reinit_params.encode_config = &config_;
    mtapi::Status status = api_.reconfigure_encoder(encoder_, &params);
    if (status != mtapi::SUCCESS) {
      log_status("mtEncReconfigureEncoder", status);
      return false;
    }
    return true;
  }

  void destroy() {
    while (!pending_.empty()) {
      size_t before = pending_.size();
      collect_one(nullptr, nullptr);
      if (pending_.size() == before) {
        break;
      }
    }

    if (encoder_ && api_.unmap_resource) {
      for (size_t i = 0; i < mapped_resources_.size(); ++i) {
        unmap_slot(i);
      }
    }

    if (encoder_ && api_.release_output_buffer) {
      for (void *output : output_buffers_) {
        if (output) {
          api_.release_output_buffer(encoder_, output);
        }
      }
    }
    output_buffers_.clear();

    for (HANDLE event : events_) {
      if (event) {
        CloseHandle(event);
      }
    }
    events_.clear();
    input_textures_.clear();

    if (encoder_ && api_.release_encoder) {
      api_.release_encoder(encoder_);
      encoder_ = nullptr;
    }
    native_.reset();

    if (module_) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
    api_ = {};
    initialized_ = false;
  }

private:
  ID3D11Device *device_ = nullptr;
  int64_t luid_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t kbs_ = 0;
  int32_t framerate_ = 0;
  int32_t gop_ = 0;

  HMODULE module_ = nullptr;
  mtapi::FunctionList api_ = {};
  void *encoder_ = nullptr;
  std::unique_ptr<NativeDevice> native_;
  mtapi::Config config_ = {};
  mtapi::InitParams init_params_ = {};

  std::vector<ComPtr<ID3D11Texture2D>> input_textures_;
  std::vector<void *> output_buffers_;
  std::vector<void *> mapped_resources_;
  std::vector<HANDLE> events_;
  std::vector<int64_t> timestamps_;
  std::deque<size_t> pending_;
  size_t next_slot_ = 0;
  bool initialized_ = false;
};

} // namespace

extern "C" {

int mt_encode_driver_support() {
  HMODULE module = nullptr;
  mtapi::FunctionList api = {};
  if (!load_api(module, api, false)) {
    return -1;
  }
  FreeLibrary(module);
  return 0;
}

void *mt_new_encoder(void *device, int64_t luid, int32_t data_format,
                     int32_t width, int32_t height, int32_t kbs,
                     int32_t framerate, int32_t gop) {
  if (data_format != H264) {
    return nullptr;
  }
  try {
    auto encoder =
        std::make_unique<MtEncoder>(device, luid, width, height, kbs,
                                    framerate, gop);
    if (!encoder->init()) {
      return nullptr;
    }
    return encoder.release();
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Creating MTEncode encoder failed: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Creating MTEncode encoder failed"));
  }
  return nullptr;
}

int mt_encode(void *encoder, void *texture, EncodeCallback callback, void *obj,
              int64_t ms) {
  if (!encoder) {
    return -1;
  }
  try {
    return static_cast<MtEncoder *>(encoder)->encode(texture, callback, obj,
                                                      ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("MTEncode frame failed: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("MTEncode frame failed"));
  }
  return -1;
}

int mt_destroy_encoder(void *encoder) {
  try {
    delete static_cast<MtEncoder *>(encoder);
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Destroying MTEncode encoder failed: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Destroying MTEncode encoder failed"));
  }
  return -1;
}

int mt_test_encode(int64_t *out_luids, int32_t *out_vendors,
                   int32_t max_desc_num, int32_t *out_desc_num,
                   int32_t data_format, int32_t width, int32_t height,
                   int32_t kbs, int32_t framerate, int32_t gop,
                   const int64_t *excluded_luids,
                   const int32_t *exclude_formats, int32_t exclude_count) {
  if (!out_luids || !out_vendors || !out_desc_num || max_desc_num <= 0 ||
      data_format != H264) {
    return -1;
  }
  *out_desc_num = 0;

  try {
    Adapters adapters;
    if (!adapters.Init(ADAPTER_VENDOR_MT)) {
      return 0;
    }

    int32_t count = 0;
    for (auto &adapter : adapters.adapters_) {
      int64_t luid = LUID(adapter->desc1_);
      if (util::skip_test(excluded_luids, exclude_formats, exclude_count,
                          luid, data_format)) {
        continue;
      }

      std::unique_ptr<MtEncoder> encoder(static_cast<MtEncoder *>(
          mt_new_encoder(adapter->device_.Get(), luid, data_format, width,
                         height, kbs, framerate, gop)));
      if (!encoder) {
        continue;
      }

      void *texture = encoder->test_texture();
      if (!texture) {
        continue;
      }

      int32_t key = 0;
      auto start = util::now();
      for (int i = 0; i < static_cast<int>(BUFFER_COUNT) && key != 1; ++i) {
        if (encoder->encode(texture, util_encode::vram_encode_test_callback,
                            &key, i) != 0) {
          break;
        }
      }
      int64_t elapsed = util::elapsed_ms(start);
      if (key == 1 && elapsed < TEST_TIMEOUT_MS) {
        out_luids[count] = luid;
        out_vendors[count] = VENDOR_MT;
        ++count;
      }
      if (count >= max_desc_num) {
        break;
      }
    }

    *out_desc_num = count;
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Testing MTEncode failed: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Testing MTEncode failed"));
  }
  return -1;
}

int mt_set_bitrate(void *encoder, int32_t kbs) {
  if (!encoder) {
    return -1;
  }
  return static_cast<MtEncoder *>(encoder)->set_bitrate(kbs);
}

int mt_set_framerate(void *encoder, int32_t framerate) {
  if (!encoder) {
    return -1;
  }
  return static_cast<MtEncoder *>(encoder)->set_framerate(framerate);
}

} // extern "C"
