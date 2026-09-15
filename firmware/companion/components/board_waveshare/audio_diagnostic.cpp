#include "audio_diagnostic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "es7210_adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace t3::board {
namespace {

constexpr char kTag[] = "task2_audio_diag";
constexpr std::size_t kFrames = 256;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kSamples = kFrames * kChannels;
constexpr std::int16_t kSentinel = static_cast<std::int16_t>(0x5A5A);
constexpr std::uint32_t kReadPeriodMs = 2000;
constexpr std::size_t kToneFrames = 256;
constexpr std::size_t kToneChannels = 2;
constexpr std::size_t kToneSamples = kToneFrames * kToneChannels;
constexpr std::uint32_t kToneSampleRate = 16000;
constexpr std::uint32_t kToneFrequencyHz = 1000;
constexpr std::int16_t kToneAmplitude = 1600;
constexpr std::uint32_t kToneDurationMs = 2000;

const char *err_name(esp_err_t value) {
  return esp_err_to_name(value);
}

void log_i2s_config() {
  // These are the exact public-BSP std-mode parameters supplied to
  // bsp_audio_init().  The maintained esp_codec_dev/I2S_IF logs immediately
  // after open report the effective mode and clock values as well.
  ESP_LOGI(kTag,
           "TASK2_BASELINE_I2S mode=STD sample_rate=16000 bits=16 channels=2 "
           "mclk_gpio=16 bclk_gpio=9 ws_gpio=45 tx_gpio=8 rx_gpio=10 "
           "expected_mclk_hz=4096000 expected_bclk_hz=512000 slots=stereo");
}

void log_gpio10_sample() {
  gpio_io_config_t io_config{};
  const auto direction_rc = gpio_get_io_config(BSP_I2S_DSIN, &io_config);
  std::uint16_t high = 0;
  std::uint16_t low = 0;
  int first = -1;
  int last = -1;
  constexpr int kSamplesToTake = 256;
  for (int index = 0; index < kSamplesToTake; ++index) {
    const int level = gpio_get_level(BSP_I2S_DSIN);
    if (index == 0) {
      first = level;
    }
    last = level;
    if (level == 0) {
      ++low;
    } else {
      ++high;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  ESP_LOGI(kTag,
           "TASK2_BASELINE_GPIO10 direction_rc=%s input_enabled=%d output_enabled=%d "
           "periph_oe=%d high=%u low=%u first=%d last=%d "
           "samples=%d",
           err_name(direction_rc), io_config.ie ? 1 : 0, io_config.oe ? 1 : 0,
           io_config.oe_ctrl_by_periph ? 1 : 0, static_cast<unsigned>(high),
           static_cast<unsigned>(low), first, last, kSamplesToTake);
}

void log_es7210_registers(esp_codec_dev_handle_t record) {
  constexpr std::array<std::uint8_t, 28> kRegisters = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x11, 0x12, 0x14, 0x15,
      0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
      0x4B, 0x4C,
  };
  for (const auto reg : kRegisters) {
    int value = 0;
    const int read_rc = esp_codec_dev_read_reg(record, reg, &value);
    ESP_LOGI(kTag, "TASK2_BASELINE_ES7210_REG reg=0x%02X read_rc=%d value=0x%02X", reg,
             read_rc, value & 0xFF);
  }
}

void log_capture_sample(esp_codec_dev_handle_t record, std::uint32_t sequence) {
  // Static storage keeps the diagnostic task's stack bounded.  A sentinel
  // makes the esp_codec_dev_read overwrite extent observable without logging
  // raw audio payload bytes.
  static std::array<std::int16_t, kSamples> buffer{};
  buffer.fill(kSentinel);
  const int read_rc = esp_codec_dev_read(record, buffer.data(), sizeof(buffer));

  std::size_t overwritten_words = 0;
  std::size_t nonzero_words = 0;
  std::int32_t min_sample = std::numeric_limits<std::int32_t>::max();
  std::int32_t max_sample = std::numeric_limits<std::int32_t>::min();
  double sum_squares = 0.0;
  for (const auto sample : buffer) {
    if (sample == kSentinel) {
      continue;
    }
    ++overwritten_words;
    const auto value = static_cast<std::int32_t>(sample);
    if (value != 0) {
      ++nonzero_words;
    }
    min_sample = std::min(min_sample, value);
    max_sample = std::max(max_sample, value);
    sum_squares += static_cast<double>(value) * value;
  }
  const int have_samples = overwritten_words != 0 ? 1 : 0;
  const int rms10 = have_samples
                        ? static_cast<int>(std::sqrt(sum_squares / overwritten_words) * 10.0)
                        : 0;
  if (!have_samples) {
    min_sample = 0;
    max_sample = 0;
  }
  ESP_LOGI(kTag,
           "TASK2_BASELINE_CAPTURE seq=%u read_rc=%d requested_bytes=%u overwritten_words=%u "
           "nonzero_words=%u min=%d max=%d rms10=%d sentinel=0x%04X",
           static_cast<unsigned>(sequence), read_rc, static_cast<unsigned>(sizeof(buffer)),
           static_cast<unsigned>(overwritten_words), static_cast<unsigned>(nonzero_words),
           static_cast<int>(min_sample), static_cast<int>(max_sample), rms10,
           static_cast<unsigned>(static_cast<std::uint16_t>(kSentinel)));
}

struct StereoChannelMetrics {
  std::size_t overwritten_words = 0;
  std::size_t nonzero_words = 0;
  std::int32_t min_sample = 0;
  std::int32_t max_sample = 0;
  std::int32_t peak = 0;
  double sum_squares = 0.0;
};

struct StereoAggregate {
  std::uint32_t reads = 0;
  std::uint32_t successful_reads = 0;
  std::uint32_t failed_reads = 0;
  int last_read_rc = ESP_CODEC_DEV_OK;
  std::size_t overwritten_words = 0;
  std::array<StereoChannelMetrics, kToneChannels> channels{};
};

void capture_stereo_frame(esp_codec_dev_handle_t record, StereoAggregate &aggregate) {
  static std::array<std::int16_t, kToneSamples> buffer{};
  buffer.fill(kSentinel);
  const int read_rc = esp_codec_dev_read(record, buffer.data(), sizeof(buffer));
  ++aggregate.reads;
  aggregate.last_read_rc = read_rc;
  if (read_rc == ESP_CODEC_DEV_OK) {
    ++aggregate.successful_reads;
  } else {
    ++aggregate.failed_reads;
  }
  for (std::size_t index = 0; index < buffer.size(); ++index) {
    const auto sample = buffer[index];
    if (sample == kSentinel) {
      continue;
    }
    ++aggregate.overwritten_words;
    auto &channel = aggregate.channels[index % kToneChannels];
    ++channel.overwritten_words;
    const auto value = static_cast<std::int32_t>(sample);
    if (value != 0) {
      ++channel.nonzero_words;
    }
    if (channel.overwritten_words == 1) {
      channel.min_sample = value;
      channel.max_sample = value;
    } else {
      channel.min_sample = std::min(channel.min_sample, value);
      channel.max_sample = std::max(channel.max_sample, value);
    }
    const auto magnitude = value < 0 ? -value : value;
    channel.peak = std::max(channel.peak, magnitude);
    channel.sum_squares += static_cast<double>(value) * value;
  }
}

void log_stereo_aggregate(const char *phase, const StereoAggregate &aggregate) {
  ESP_LOGI(kTag,
           "TASK2_STEREO_CAPTURE phase=%s reads=%u successful=%u failed=%u last_read_rc=%d "
           "requested_bytes=%u overwritten_words=%u requested_words=%u sentinel=0x%04X",
           phase, static_cast<unsigned>(aggregate.reads),
           static_cast<unsigned>(aggregate.successful_reads),
           static_cast<unsigned>(aggregate.failed_reads), aggregate.last_read_rc,
           static_cast<unsigned>(sizeof(std::array<std::int16_t, kToneSamples>)),
           static_cast<unsigned>(aggregate.overwritten_words),
           static_cast<unsigned>(aggregate.reads * kToneSamples),
           static_cast<unsigned>(static_cast<std::uint16_t>(kSentinel)));
  for (std::size_t channel_index = 0; channel_index < aggregate.channels.size(); ++channel_index) {
    const auto &channel = aggregate.channels[channel_index];
    const int rms10 = channel.overwritten_words == 0
                          ? 0
                          : static_cast<int>(std::sqrt(channel.sum_squares /
                                                       channel.overwritten_words) * 10.0);
    ESP_LOGI(kTag,
             "TASK2_STEREO_CHANNEL phase=%s channel=%u nonzero=%u overwritten=%u min=%d max=%d "
             "rms10=%d peak=%d",
             phase, static_cast<unsigned>(channel_index),
             static_cast<unsigned>(channel.nonzero_words),
             static_cast<unsigned>(channel.overwritten_words), static_cast<int>(channel.min_sample),
             static_cast<int>(channel.max_sample), rms10, static_cast<int>(channel.peak));
  }
}

std::array<std::int16_t, kToneSamples> &stereo_tone_buffer() {
  static std::array<std::int16_t, kToneSamples> buffer{};
  constexpr double kTwoPi = 6.28318530717958647692;
  for (std::size_t frame = 0; frame < kToneFrames; ++frame) {
    const double phase = kTwoPi * static_cast<double>(kToneFrequencyHz * frame) /
                         static_cast<double>(kToneSampleRate);
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * kToneAmplitude));
    buffer[frame * kToneChannels] = sample;
    buffer[frame * kToneChannels + 1] = sample;
  }
  return buffer;
}

struct Gpio10Window {
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  int first = -1;
  int last = -1;
};

Gpio10Window sample_gpio10_tone_window(std::size_t samples, bool delay_between_samples) {
  Gpio10Window result{};
  for (std::size_t index = 0; index < samples; ++index) {
    const int level = gpio_get_level(BSP_I2S_DSIN);
    if (index == 0) {
      result.first = level;
    }
    result.last = level;
    if (level == 0) {
      ++result.low;
    } else {
      ++result.high;
    }
    if (delay_between_samples) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return result;
}

void log_gpio10_tone_window(const char *phase, const Gpio10Window &window) {
  ESP_LOGI(kTag,
           "TASK2_STEREO_GPIO10 phase=%s high=%u low=%u first=%d last=%d samples=%u",
           phase, static_cast<unsigned>(window.high), static_cast<unsigned>(window.low),
           window.first, window.last,
           static_cast<unsigned>(window.high + window.low));
}

}  // namespace

void run_audio_diagnostic() {
  ESP_LOGI(kTag,
           "TASK2_AUDIO_BASELINE_START runtime=audio_only variant=1.75C mclk_gpio=16 "
           "bsp=public_unchanged "
           "no_display_touch_buttons_network_nvs_ota");
  log_i2s_config();

  const auto i2c_rc = bsp_i2c_init();
  ESP_LOGI(kTag, "TASK2_BASELINE_I2C init_rc=%s", err_name(i2c_rc));
  if (i2c_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO_BASELINE_BLOCKED i2c_init");
    return;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          // The official 1.75C pin map routes ES7210/ES8311 MCLK to GPIO16.
          .mclk = GPIO_NUM_16,
          .bclk = BSP_I2S_SCLK,
          .ws = BSP_I2S_LCLK,
          .dout = BSP_I2S_DOUT,
          .din = BSP_I2S_DSIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  const auto audio_rc = bsp_audio_init(&std_cfg);
  ESP_LOGI(kTag, "TASK2_BASELINE_BSP_AUDIO_INIT rc=%s", err_name(audio_rc));
  if (audio_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO_BASELINE_BLOCKED bsp_audio_init");
    return;
  }

  // These are the maintained public constructors, unchanged.  They create
  // ES8311 playback and ES7210 recording devices over the BSP-owned I2S data
  // interface; no private handles or direct I2S reads are used here.
  const auto speaker = bsp_audio_codec_speaker_init();
  const auto record = bsp_audio_codec_microphone_init();
  ESP_LOGI(kTag, "TASK2_BASELINE_BSP_CODECS speaker=%d record=%d", speaker != nullptr,
           record != nullptr);
  if (speaker == nullptr || record == nullptr) {
    ESP_LOGE(kTag, "TASK2_AUDIO_BASELINE_BLOCKED codec_constructor");
    return;
  }

  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 2,
      .channel_mask = 0,
      .sample_rate = 16000,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  const int speaker_open_rc = esp_codec_dev_open(speaker, &fs);
  const int speaker_mute_rc = esp_codec_dev_set_out_mute(speaker, true);
  const int speaker_volume_rc = esp_codec_dev_set_out_vol(speaker, 0);
  ESP_LOGI(kTag,
           "TASK2_BASELINE_SPEAKER open_rc=%d mute_rc=%d volume_rc=%d fs=16000/16/2",
           speaker_open_rc, speaker_mute_rc, speaker_volume_rc);
  const int record_open_rc = esp_codec_dev_open(record, &fs);
  const int gain_rc = esp_codec_dev_set_in_gain(record, 24.0F);
  ESP_LOGI(kTag,
           "TASK2_BASELINE_RECORD open_rc=%d gain_rc=%d mic_selection=MIC1|MIC2 "
           "gain_db=24 fs=16000/16/2 read_api=esp_codec_dev_read",
           record_open_rc, gain_rc);
  if (record_open_rc != ESP_CODEC_DEV_OK || gain_rc != ESP_CODEC_DEV_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO_BASELINE_BLOCKED record_open_or_gain");
    return;
  }

  log_es7210_registers(record);
  log_gpio10_sample();
  for (std::uint32_t sequence = 0;; ++sequence) {
    log_capture_sample(record, sequence);
    if (sequence == 2) {
      ESP_LOGI(kTag,
               "TASK2_BASELINE_IDLE_READY stable_samples=3 "
               "awaiting_parent_speech_instruction=true");
    }
    vTaskDelay(pdMS_TO_TICKS(kReadPeriodMs));
  }
}

void run_stereo_tone_diagnostic() {
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_START runtime=audio_only variant=1.75C mclk_gpio=16 "
           "topology=STD_TX+STD_RX no_display_touch_buttons_network_nvs_ota");
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_SPEC frequency_hz=%u amplitude_peak=%d duration_ms=%u "
           "sample_rate_hz=%u bits=16 channels=2 volume_db=0 output=ES8311 "
           "capture=ES7210_MIC1_MIC2 read_api=esp_codec_dev_read",
           static_cast<unsigned>(kToneFrequencyHz), static_cast<int>(kToneAmplitude),
           static_cast<unsigned>(kToneDurationMs), static_cast<unsigned>(kToneSampleRate));

  const auto i2c_rc = bsp_i2c_init();
  ESP_LOGI(kTag, "TASK2_STEREO_TONE_I2C init_rc=%s", err_name(i2c_rc));
  if (i2c_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_STEREO_TONE_BLOCKED i2c_init");
    return;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kToneSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = GPIO_NUM_16,
          .bclk = BSP_I2S_SCLK,
          .ws = BSP_I2S_LCLK,
          .dout = BSP_I2S_DOUT,
          .din = BSP_I2S_DSIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  const auto audio_rc = bsp_audio_init(&std_cfg);
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_BSP_AUDIO_INIT rc=%s effective_mclk_gpio=16 "
           "expected_mclk_hz=4096000 expected_bclk_hz=512000",
           err_name(audio_rc));
  if (audio_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_STEREO_TONE_BLOCKED bsp_audio_init");
    return;
  }
  const auto speaker = bsp_audio_codec_speaker_init();
  const auto record = bsp_audio_codec_microphone_init();
  if (speaker == nullptr || record == nullptr) {
    ESP_LOGE(kTag, "TASK2_STEREO_TONE_BLOCKED codec_constructor speaker=%d record=%d",
             speaker != nullptr, record != nullptr);
    return;
  }
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 2,
      .channel_mask = 0,
      .sample_rate = kToneSampleRate,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  const int speaker_open_rc = esp_codec_dev_open(speaker, &fs);
  const int speaker_mute_rc = esp_codec_dev_set_out_mute(speaker, false);
  const int speaker_volume_rc = esp_codec_dev_set_out_vol(speaker, 0);
  const int record_open_rc = esp_codec_dev_open(record, &fs);
  const int gain_rc = esp_codec_dev_set_in_gain(record, 24.0F);
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_OPEN speaker_open_rc=%d mute_rc=%d volume_rc=%d "
           "record_open_rc=%d gain_rc=%d fs=16000/16/2 gain_db=24",
           speaker_open_rc, speaker_mute_rc, speaker_volume_rc, record_open_rc, gain_rc);
  if (record_open_rc != ESP_CODEC_DEV_OK || gain_rc != ESP_CODEC_DEV_OK) {
    ESP_LOGE(kTag, "TASK2_STEREO_TONE_BLOCKED record_open_or_gain");
    return;
  }
  log_es7210_registers(record);

  StereoAggregate before{};
  const auto gpio_before = sample_gpio10_tone_window(128, true);
  for (int index = 0; index < 3; ++index) {
    capture_stereo_frame(record, before);
  }
  log_stereo_aggregate("before", before);
  log_gpio10_tone_window("before", gpio_before);

  StereoAggregate during{};
  const auto &tone = stereo_tone_buffer();
  const auto tone_start_us = esp_timer_get_time();
  std::uint32_t writes = 0;
  Gpio10Window gpio_during{};
  bool tone_error = false;
  int first_write_rc = ESP_CODEC_DEV_OK;
  while (esp_timer_get_time() - tone_start_us <
         static_cast<int64_t>(kToneDurationMs) * 1000) {
    const int level = gpio_get_level(BSP_I2S_DSIN);
    if (gpio_during.high + gpio_during.low == 0) {
      gpio_during.first = level;
    }
    gpio_during.last = level;
    if (level == 0) {
      ++gpio_during.low;
    } else {
      ++gpio_during.high;
    }
    const int write_rc = esp_codec_dev_write(speaker, const_cast<std::int16_t *>(tone.data()),
                                             sizeof(tone));
    if (writes == 0) {
      first_write_rc = write_rc;
    }
    if (write_rc != ESP_CODEC_DEV_OK) {
      ESP_LOGE(kTag, "TASK2_STEREO_TONE_ABORT write_rc=%d writes=%u", write_rc,
               static_cast<unsigned>(writes));
      tone_error = true;
      break;
    }
    ++writes;
    capture_stereo_frame(record, during);
    if (during.failed_reads != 0) {
      ESP_LOGE(kTag, "TASK2_STEREO_TONE_ABORT read_rc=%d failures=%u", during.last_read_rc,
               static_cast<unsigned>(during.failed_reads));
      tone_error = true;
      break;
    }
  }
  const auto elapsed_us = esp_timer_get_time() - tone_start_us;
  const int mute_after_rc = esp_codec_dev_set_out_mute(speaker, true);
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_RESULT writes=%u bytes_each=%u first_write_rc=%d elapsed_us=%lld "
           "elapsed_ms=%lld mute_after_rc=%d error=%d",
           static_cast<unsigned>(writes), static_cast<unsigned>(sizeof(tone)), first_write_rc,
           static_cast<long long>(elapsed_us), static_cast<long long>(elapsed_us / 1000),
           mute_after_rc, tone_error ? 1 : 0);
  log_stereo_aggregate("during", during);
  log_gpio10_tone_window("during", gpio_during);

  StereoAggregate after{};
  const auto gpio_after = sample_gpio10_tone_window(128, true);
  for (int index = 0; index < 3; ++index) {
    capture_stereo_frame(record, after);
  }
  log_stereo_aggregate("after", after);
  log_gpio10_tone_window("after", gpio_after);
  ESP_LOGI(kTag,
           "TASK2_STEREO_TONE_DONE stable_runtime=%d mapping_claimed=0 "
           "correlation_requires_before_during_after=1",
           tone_error ? 0 : 1);
}

}  // namespace t3::board
