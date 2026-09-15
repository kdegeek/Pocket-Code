#pragma once

#include "manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace t3::companion {

enum class OtaTransferErrorCode {
  None,
  InvalidUrl,
  InvalidManifest,
  NotStarted,
  InvalidState,
  Interrupted,
  Http,
  OffsetMismatch,
  EmptyChunk,
  ChunkTooLarge,
  WriteFailed,
  SizeMismatch,
  HashMismatch,
};

struct OtaTransferError {
  OtaTransferErrorCode code = OtaTransferErrorCode::None;
  std::string message;
};

struct OtaTransferResult {
  OtaTransferError error;
  [[nodiscard]] bool ok() const {
    return error.code == OtaTransferErrorCode::None;
  }
  static OtaTransferResult success() { return {}; }
  static OtaTransferResult failure(OtaTransferErrorCode code,
                                   std::string message) {
    return {{code, std::move(message)}};
  }
};

/** Response boundary for an explicit HTTPS range request. */
struct OtaHttpResponse {
  int status = 0;
  std::size_t offset = 0;
  std::vector<std::uint8_t> body;
  bool complete = false;
  std::string error;
};

/** ESP-IDF or a host fake implements this seam. No default implementation
 * opens a socket or makes a request. */
class OtaHttpClient {
 public:
  virtual ~OtaHttpClient() = default;
  [[nodiscard]] virtual OtaHttpResponse get(std::string_view https_url,
                                             std::size_t offset) = 0;
};

/** OTA writes are isolated from flash APIs so interrupted host tests never
 * access a device or persist credentials. */
class OtaImageWriter {
 public:
  virtual ~OtaImageWriter() = default;
  [[nodiscard]] virtual bool begin(std::size_t image_size) = 0;
  [[nodiscard]] virtual bool write(std::size_t offset,
                                   std::span<const std::uint8_t> bytes) = 0;
  [[nodiscard]] virtual bool finish() = 0;
};

enum class OtaTransferStatus { Idle, Receiving, Interrupted, Ready, Failed };

/**
 * Resumable, HTTPS-only transfer state machine. `start` validates metadata and
 * initializes the sink but never calls OtaHttpClient. A caller explicitly
 * invokes `pull` when it is ready to perform a request.
 */
class HttpsOtaSession {
 public:
  explicit HttpsOtaSession(OtaHttpClient& client) : client_(client) {}

  [[nodiscard]] OtaTransferResult start(std::string_view https_url,
                                         const OtaManifest& manifest,
                                         OtaImageWriter& writer);
  [[nodiscard]] OtaTransferResult pull();
  [[nodiscard]] OtaTransferResult interrupt();
  [[nodiscard]] OtaTransferResult resume();
  [[nodiscard]] OtaTransferResult finish();

  [[nodiscard]] OtaTransferStatus status() const { return status_; }
  [[nodiscard]] std::size_t received_bytes() const { return received_bytes_; }
  [[nodiscard]] std::size_t next_offset() const { return received_bytes_; }
  [[nodiscard]] bool response_was_complete() const { return response_complete_; }
  [[nodiscard]] std::string_view url() const { return url_; }

 private:
  OtaHttpClient& client_;
  OtaImageWriter* writer_ = nullptr;
  OtaManifest manifest_;
  Sha256Hasher hasher_;
  std::string url_;
  std::size_t received_bytes_ = 0;
  bool response_complete_ = false;
  OtaTransferStatus status_ = OtaTransferStatus::Idle;
};

[[nodiscard]] bool is_https_url(std::string_view url);

}  // namespace t3::companion
