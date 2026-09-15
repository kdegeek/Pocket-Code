#include "https_ota.hpp"

#include <algorithm>

namespace t3::companion {

bool is_https_url(std::string_view url) {
  if (url.size() < 9 || url.substr(0, 8) != "https://") return false;
  const auto authority_start = 8U;
  const auto authority_end = url.find_first_of("/?#", authority_start);
  const auto authority_length =
      (authority_end == std::string_view::npos ? url.size() : authority_end) -
      authority_start;
  if (authority_length == 0) return false;
  for (const unsigned char value : url) {
    if (value <= 0x20U || value == 0x7fU) return false;
  }
  return true;
}

OtaTransferResult HttpsOtaSession::start(std::string_view https_url,
                                        const OtaManifest& manifest,
                                        OtaImageWriter& writer) {
  if (!is_https_url(https_url)) {
    return OtaTransferResult::failure(OtaTransferErrorCode::InvalidUrl,
                                      "OTA requires an HTTPS URL");
  }
  if (manifest.image_size_bytes == 0 || manifest.slot_size_bytes == 0 ||
      manifest.image_size_bytes > manifest.slot_size_bytes ||
      !is_sha256_hex(manifest.image_sha256)) {
    return OtaTransferResult::failure(
        OtaTransferErrorCode::InvalidManifest,
        "OTA manifest has invalid image size, slot size, or hash");
  }
  if (!writer.begin(manifest.image_size_bytes)) {
    return OtaTransferResult::failure(OtaTransferErrorCode::WriteFailed,
                                      "OTA image sink rejected initialization");
  }
  manifest_ = manifest;
  url_.assign(https_url.data(), https_url.size());
  writer_ = &writer;
  hasher_.reset();
  received_bytes_ = 0;
  response_complete_ = false;
  status_ = OtaTransferStatus::Receiving;
  return OtaTransferResult::success();
}

OtaTransferResult HttpsOtaSession::pull() {
  if (status_ == OtaTransferStatus::Interrupted) {
    return OtaTransferResult::failure(OtaTransferErrorCode::Interrupted,
                                      "resume is required before pulling");
  }
  if (status_ != OtaTransferStatus::Receiving || writer_ == nullptr) {
    return OtaTransferResult::failure(OtaTransferErrorCode::NotStarted,
                                      "OTA transfer is not receiving");
  }
  const auto response = client_.get(url_, received_bytes_);
  if (response.status != 200 && response.status != 206) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(
        OtaTransferErrorCode::Http,
        response.error.empty() ? "OTA HTTPS request failed" : response.error);
  }
  if (response.offset != received_bytes_) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(
        OtaTransferErrorCode::OffsetMismatch,
        "OTA response did not begin at the resumable offset");
  }
  if (response.body.empty()) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(OtaTransferErrorCode::EmptyChunk,
                                      "OTA response contained no bytes");
  }
  const std::size_t remaining = manifest_.image_size_bytes - received_bytes_;
  if (response.body.size() > remaining) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(OtaTransferErrorCode::ChunkTooLarge,
                                      "OTA response exceeds manifest image size");
  }
  if (!writer_->write(received_bytes_, response.body)) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(OtaTransferErrorCode::WriteFailed,
                                      "OTA image sink rejected a chunk");
  }
  hasher_.update(response.body);
  received_bytes_ += response.body.size();
  response_complete_ = response.complete;
  return OtaTransferResult::success();
}

OtaTransferResult HttpsOtaSession::interrupt() {
  if (status_ != OtaTransferStatus::Receiving) {
    return OtaTransferResult::failure(OtaTransferErrorCode::InvalidState,
                                      "OTA transfer is not receiving");
  }
  status_ = OtaTransferStatus::Interrupted;
  return OtaTransferResult::success();
}

OtaTransferResult HttpsOtaSession::resume() {
  if (status_ != OtaTransferStatus::Interrupted) {
    return OtaTransferResult::failure(OtaTransferErrorCode::InvalidState,
                                      "OTA transfer is not interrupted");
  }
  status_ = OtaTransferStatus::Receiving;
  return OtaTransferResult::success();
}

OtaTransferResult HttpsOtaSession::finish() {
  if (status_ != OtaTransferStatus::Receiving || writer_ == nullptr) {
    return OtaTransferResult::failure(OtaTransferErrorCode::InvalidState,
                                      "OTA transfer is not ready to finish");
  }
  if (received_bytes_ != manifest_.image_size_bytes) {
    return OtaTransferResult::failure(OtaTransferErrorCode::SizeMismatch,
                                      "OTA image is incomplete");
  }
  if (hasher_.finalize_hex() != manifest_.image_sha256) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(OtaTransferErrorCode::HashMismatch,
                                      "OTA image hash does not match manifest");
  }
  if (!writer_->finish()) {
    status_ = OtaTransferStatus::Failed;
    return OtaTransferResult::failure(OtaTransferErrorCode::WriteFailed,
                                      "OTA image sink rejected finalization");
  }
  status_ = OtaTransferStatus::Ready;
  return OtaTransferResult::success();
}

}  // namespace t3::companion
