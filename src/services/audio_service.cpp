#include "services/audio_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "utils/asset_manager.h"
#include "utils/logger.h"

#if !USE_DESKTOP && APP_USE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace service {
namespace {

constexpr const char* kShutterAsset        = "audio/shutter.wav";
constexpr const char* kPressAsset          = "audio/click.wav";
constexpr const char* kRecordStartAsset    = "";
constexpr const char* kRecordFinishedAsset = "";
constexpr unsigned int kRecordRate         = 48000;
constexpr unsigned int kRecordChannels     = 1;
constexpr unsigned int kBitsPerSample      = 16;
constexpr size_t kPeriodFrames             = 1024;

struct WavData {
  unsigned int channels{0};
  unsigned int sample_rate{0};
  unsigned int bits_per_sample{0};
  std::vector<uint8_t> pcm;
};

uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

void write_le16(std::ostream& out, uint16_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xFF),
      static_cast<char>((value >> 8) & 0xFF),
  };
  out.write(bytes, sizeof(bytes));
}

void write_le32(std::ostream& out, uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xFF),
      static_cast<char>((value >> 8) & 0xFF),
      static_cast<char>((value >> 16) & 0xFF),
      static_cast<char>((value >> 24) & 0xFF),
  };
  out.write(bytes, sizeof(bytes));
}

bool load_wav_asset(const char* asset_name, WavData& wav) {
  asset::BinaryAsset asset;
  if (!asset::AssetManager::read_binary(asset_name, asset)) {
    LOG_WARN("Audio asset not found: {}", asset_name);
    return false;
  }

  const auto& data = asset.data;
  if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
      std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
    LOG_WARN("Invalid WAV asset: {}", asset.path);
    return false;
  }

  bool found_fmt        = false;
  bool found_data       = false;
  uint16_t audio_format = 0;
  size_t offset         = 12;
  while (offset + 8 <= data.size()) {
    const uint8_t* chunk      = data.data() + offset;
    const uint32_t chunk_size = read_le32(chunk + 4);
    const size_t payload      = offset + 8;
    if (payload + chunk_size > data.size()) {
      break;
    }

    if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
      audio_format        = read_le16(data.data() + payload);
      wav.channels        = read_le16(data.data() + payload + 2);
      wav.sample_rate     = read_le32(data.data() + payload + 4);
      wav.bits_per_sample = read_le16(data.data() + payload + 14);
      found_fmt           = true;
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      wav.pcm.assign(data.begin() + static_cast<std::ptrdiff_t>(payload),
                     data.begin() + static_cast<std::ptrdiff_t>(payload + chunk_size));
      found_data = true;
    }

    offset = payload + chunk_size + (chunk_size & 1u);
  }

  if (!found_fmt || !found_data || audio_format != 1 || wav.channels == 0 || wav.sample_rate == 0 ||
      wav.bits_per_sample != 16 || wav.pcm.empty()) {
    LOG_WARN("Unsupported WAV asset format: {}", asset.path);
    return false;
  }

  LOG_DEBUG("Loaded WAV asset: {}", asset.path);
  return true;
}

void write_wav_header(std::ostream& out,
                      uint32_t data_size,
                      uint32_t sample_rate,
                      uint16_t channels,
                      uint16_t bits_per_sample) {
  const uint32_t byte_rate   = sample_rate * channels * bits_per_sample / 8;
  const uint16_t block_align = channels * bits_per_sample / 8;

  out.seekp(0, std::ios::beg);
  out.write("RIFF", 4);
  write_le32(out, 36 + data_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  write_le32(out, 16);
  write_le16(out, 1);
  write_le16(out, channels);
  write_le32(out, sample_rate);
  write_le32(out, byte_rate);
  write_le16(out, block_align);
  write_le16(out, bits_per_sample);
  out.write("data", 4);
  write_le32(out, data_size);
}

#if !USE_DESKTOP && APP_USE_ALSA
void append_unique_device(std::vector<std::string>& devices, const char* device) {
  if (!device || !device[0]) {
    return;
  }

  if (std::find(devices.begin(), devices.end(), device) == devices.end()) {
    devices.emplace_back(device);
  }
}

std::vector<std::string> playback_device_candidates() {
  std::vector<std::string> devices;
  append_unique_device(devices, std::getenv("CAMERA_APP_ALSA_PLAYBACK_DEVICE"));
  append_unique_device(devices, std::getenv("CAMERA_APP_ALSA_DEVICE"));
  append_unique_device(devices, APP_ALSA_PLAYBACK_DEVICE);
  append_unique_device(devices, "default:CARD=ES8388Audio");
  append_unique_device(devices, "plughw:CARD=ES8388Audio,DEV=0");
  append_unique_device(devices, "dmix:CARD=ES8388Audio,DEV=0");
  append_unique_device(devices, "default");
  append_unique_device(devices, "default:CARD=vc4hdmi");
  append_unique_device(devices, "plughw:CARD=vc4hdmi,DEV=0");
  return devices;
}

std::vector<std::string> capture_device_candidates() {
  std::vector<std::string> devices;
  append_unique_device(devices, std::getenv("CAMERA_APP_ALSA_CAPTURE_DEVICE"));
  append_unique_device(devices, std::getenv("CAMERA_APP_ALSA_DEVICE"));
  append_unique_device(devices, APP_ALSA_CAPTURE_DEVICE);
  append_unique_device(devices, "default:CARD=ES8388Audio");
  append_unique_device(devices, "plughw:CARD=ES8388Audio,DEV=0");
  append_unique_device(devices, "default");
  return devices;
}

bool open_pcm_from_candidates(snd_pcm_t** pcm,
                              snd_pcm_stream_t stream,
                              const std::vector<std::string>& devices,
                              std::string& opened_device) {
  for (const auto& device : devices) {
    const int err = snd_pcm_open(pcm, device.c_str(), stream, 0);
    if (err >= 0) {
      opened_device = device;
      LOG_INFO("ALSA {} opened: {}",
               stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
               device);
      return true;
    }

    LOG_WARN("ALSA {} open failed for {}: {} ({})",
             stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
             device,
             snd_strerror(err),
             err);
  }

  return false;
}

bool configure_pcm(snd_pcm_t* pcm,
                   snd_pcm_stream_t stream,
                   unsigned int sample_rate,
                   unsigned int channels) {
  snd_pcm_hw_params_t* params = nullptr;
  snd_pcm_hw_params_alloca(&params);

  int err = snd_pcm_hw_params_any(pcm, params);
  if (err < 0) {
    LOG_WARN("ALSA hw params init failed: {}", snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0) {
    LOG_WARN("ALSA set access failed: {}", snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
  if (err < 0) {
    LOG_WARN("ALSA set format S16_LE failed: {}", snd_strerror(err));
    return false;
  }

  unsigned int actual_rate = sample_rate;
  err                      = snd_pcm_hw_params_set_rate_near(pcm, params, &actual_rate, nullptr);
  if (err < 0) {
    LOG_WARN("ALSA set rate failed: {}", snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_channels(pcm, params, channels);
  if (err < 0) {
    LOG_WARN("ALSA set channels failed: {}", snd_strerror(err));
    return false;
  }

  snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(kPeriodFrames);
  (void)snd_pcm_hw_params_set_period_size_near(pcm, params, &period, nullptr);

  err = snd_pcm_hw_params(pcm, params);
  if (err < 0) {
    LOG_WARN("ALSA apply hw params failed: {}", snd_strerror(err));
    return false;
  }

  err = snd_pcm_prepare(pcm);
  if (err < 0) {
    LOG_WARN("ALSA prepare failed: {}", snd_strerror(err));
    return false;
  }

  (void)stream;
  return true;
}

bool write_pcm_frames(snd_pcm_t* pcm,
                      const int16_t* samples,
                      size_t frames,
                      unsigned int channels) {
  size_t written = 0;
  while (written < frames) {
    const snd_pcm_sframes_t rc =
        snd_pcm_writei(pcm, samples + written * channels, frames - written);
    if (rc == -EPIPE) {
      snd_pcm_prepare(pcm);
      continue;
    }
    if (rc < 0) {
      const int recovered = snd_pcm_recover(pcm, static_cast<int>(rc), 1);
      if (recovered < 0) {
        LOG_WARN("ALSA playback write failed: {}", snd_strerror(static_cast<int>(rc)));
        return false;
      }
      continue;
    }
    written += static_cast<size_t>(rc);
  }

  return true;
}

bool playback_wav(const WavData& wav) {
  snd_pcm_t* pcm = nullptr;
  std::string opened_device;
  if (!open_pcm_from_candidates(&pcm,
                                SND_PCM_STREAM_PLAYBACK,
                                playback_device_candidates(),
                                opened_device)) {
    LOG_WARN("ALSA playback open failed: no usable device");
    return false;
  }

  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm_guard(pcm, snd_pcm_close);
  if (!configure_pcm(pcm, SND_PCM_STREAM_PLAYBACK, wav.sample_rate, wav.channels)) {
    LOG_WARN("ALSA playback configure failed for {}", opened_device);
    return false;
  }

  const size_t frame_bytes = wav.channels * wav.bits_per_sample / 8;
  const size_t frames      = wav.pcm.size() / frame_bytes;
  const bool ok =
      write_pcm_frames(pcm, reinterpret_cast<const int16_t*>(wav.pcm.data()), frames, wav.channels);
  snd_pcm_drain(pcm);
  return ok;
}

void capture_loop(std::atomic<bool>& recording,
                  std::atomic<bool>& record_ok,
                  const std::string path) {
  snd_pcm_t* pcm = nullptr;
  std::string opened_device;
  if (!open_pcm_from_candidates(&pcm,
                                SND_PCM_STREAM_CAPTURE,
                                capture_device_candidates(),
                                opened_device)) {
    LOG_WARN("ALSA capture open failed: no usable device");
    record_ok = false;
    return;
  }

  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm_guard(pcm, snd_pcm_close);
  if (!configure_pcm(pcm, SND_PCM_STREAM_CAPTURE, kRecordRate, kRecordChannels)) {
    LOG_WARN("ALSA capture configure failed for {}", opened_device);
    record_ok = false;
    return;
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    LOG_WARN("Failed to open audio recording file: {}", path);
    record_ok = false;
    return;
  }

  write_wav_header(out, 0, kRecordRate, kRecordChannels, kBitsPerSample);

  std::vector<int16_t> frames(kPeriodFrames * kRecordChannels);
  uint32_t data_size = 0;
  while (recording.load()) {
    const snd_pcm_sframes_t rc = snd_pcm_readi(pcm, frames.data(), kPeriodFrames);
    if (rc == -EPIPE) {
      snd_pcm_prepare(pcm);
      continue;
    }
    if (rc < 0) {
      const int recovered = snd_pcm_recover(pcm, static_cast<int>(rc), 1);
      if (recovered < 0) {
        LOG_WARN("ALSA capture read failed: {}", snd_strerror(static_cast<int>(rc)));
        record_ok = false;
        break;
      }
      continue;
    }
    if (rc == 0) {
      continue;
    }

    const size_t bytes = static_cast<size_t>(rc) * kRecordChannels * sizeof(int16_t);
    out.write(reinterpret_cast<const char*>(frames.data()), static_cast<std::streamsize>(bytes));
    data_size += static_cast<uint32_t>(bytes);
  }

  snd_pcm_drop(pcm);
  write_wav_header(out, data_size, kRecordRate, kRecordChannels, kBitsPerSample);
  out.close();
  LOG_INFO("Audio recording finalized: {}", path);
}
#endif

}  // namespace

struct AudioService::Impl {
  std::thread playback_thread;
  std::thread record_thread;
  std::atomic<bool> recording{false};
  std::atomic<bool> record_ok{true};

  ~Impl() {
    stop_recording();
    if (playback_thread.joinable()) {
      playback_thread.join();
    }
  }

  bool play_asset(const char* asset_name) {
#if USE_DESKTOP
    LOG_INFO("Audio playback simulator: {}", asset_name);
    return true;
#elif !APP_USE_ALSA
    LOG_WARN("Audio playback unavailable; built without ALSA support: {}", asset_name);
    return false;
#else
    WavData wav;
    if (!load_wav_asset(asset_name, wav)) {
      return false;
    }

    if (playback_thread.joinable()) {
      playback_thread.join();
    }

    playback_thread = std::thread([wav = std::move(wav)]() { (void)playback_wav(wav); });
    return true;
#endif
  }

  bool play_shutter() { return play_asset(kShutterAsset); }

  bool play_click() { return play_asset(kPressAsset); }

  bool start_recording(const std::string& path) {
    if (path.empty()) {
      return false;
    }

    if (recording.load()) {
      return true;
    }

#if USE_DESKTOP
    LOG_INFO("Audio recording simulator started: {}", path);
    recording = true;
    record_ok = true;
    return true;
#elif !APP_USE_ALSA
    LOG_WARN("Audio recording unavailable; built without ALSA support: {}", path);
    record_ok = false;
    return false;
#else
    if (record_thread.joinable()) {
      record_thread.join();
    }

    record_ok     = true;
    recording     = true;
    record_thread = std::thread([this, path]() { capture_loop(recording, record_ok, path); });
    LOG_INFO("Audio recording started: {}", path);
    return true;
#endif
  }

  bool stop_recording() {
    if (!recording.load()) {
      if (record_thread.joinable()) {
        record_thread.join();
      }
      return record_ok.load();
    }

    recording = false;
    if (record_thread.joinable()) {
      record_thread.join();
    }

#if USE_DESKTOP
    LOG_INFO("Audio recording simulator stopped");
#elif !APP_USE_ALSA
    LOG_WARN("Audio recording stopped without ALSA support");
#else
    LOG_INFO("Audio recording stopped");
#endif
    return record_ok.load();
  }
};

AudioService::AudioService() = default;

AudioService::~AudioService() { stop(); }

void AudioService::ensure_impl_() {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }
}

void AudioService::start() {
  ensure_impl_();
  if (state_ == AudioServiceState::Recording) {
    return;
  }

  state_          = AudioServiceState::Ready;
  status_message_ = "Audio ready";
  LOG_INFO("Audio service ready");
}

void AudioService::stop() {
  if (impl_ && state_ == AudioServiceState::Recording) {
    impl_->stop_recording();
  }

  state_          = AudioServiceState::Idle;
  status_message_ = "Audio idle";
  recording_path_.clear();
  LOG_INFO("Audio service stopped");
}

void AudioService::update(uint32_t /*delta_ms*/) {}

bool AudioService::play_shutter() {
  ensure_impl_();
  if (state_ == AudioServiceState::Idle) {
    start();
  }

  const bool ok   = impl_->play_shutter();
  status_message_ = ok ? "Shutter sound played" : "Shutter sound unavailable";
  return ok;
}

bool AudioService::play_click() {
  ensure_impl_();
  if (state_ == AudioServiceState::Idle) {
    start();
  }

  const bool ok   = impl_->play_click();
  status_message_ = ok ? "Click sound played" : "Click sound unavailable";
  return ok;
}

bool AudioService::start_recording(const std::string& path) {
  ensure_impl_();
  if (state_ == AudioServiceState::Idle) {
    start();
  }

  if (state_ == AudioServiceState::Recording) {
    return true;
  }

  if (!impl_->start_recording(path)) {
    state_          = AudioServiceState::Error;
    status_message_ = "Audio recording failed";
    return false;
  }

  recording_path_ = path;
  state_          = AudioServiceState::Recording;
  status_message_ = "Audio recording";
  return true;
}

bool AudioService::stop_recording() {
  if (state_ != AudioServiceState::Recording) {
    return true;
  }

  ensure_impl_();
  const bool ok   = impl_->stop_recording();
  state_          = ok ? AudioServiceState::Ready : AudioServiceState::Error;
  status_message_ = ok ? "Audio ready" : "Audio stop failed";
  return ok;
}

}  // namespace service
