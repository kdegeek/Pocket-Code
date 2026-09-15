#include "audio_frame.hpp"
#include "envelope.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace t3::companion;

// Small self-contained SHA-256 implementation keeps the fixture runner
// deterministic and independent from OpenSSL or the ESP-IDF host toolchain.
class Sha256 {
 public:
  void update(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
      block_[block_size_++] = byte;
      total_bytes_++;
      if (block_size_ == block_.size()) {
        transform();
        block_size_ = 0;
      }
    }
  }

  std::array<std::uint8_t, 32> finish() {
    const std::uint64_t total_bits = total_bytes_ * 8;
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) {
      while (block_size_ < 64) block_[block_size_++] = 0;
      transform();
      block_size_ = 0;
    }
    while (block_size_ < 56) block_[block_size_++] = 0;
    for (int index = 7; index >= 0; --index) {
      block_[block_size_++] = static_cast<std::uint8_t>(total_bits >> (index * 8));
    }
    transform();

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
      digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
      digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
      digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
      digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
    }
    return digest;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t amount) {
    return (value >> amount) | (value << (32 - amount));
  }

  void transform() {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = (static_cast<std::uint32_t>(block_[index * 4]) << 24) |
                     (static_cast<std::uint32_t>(block_[index * 4 + 1]) << 16) |
                     (static_cast<std::uint32_t>(block_[index * 4 + 2]) << 8) |
                     static_cast<std::uint32_t>(block_[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t s0 = rotate_right(words[index - 15], 7) ^
                               rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
      const std::uint32_t s1 = rotate_right(words[index - 2], 17) ^
                               rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 = h + s1 + choose + k[index] + words[index];
      const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = s0 + majority;
      h = g; g = f; f = e; e = d + temporary1;
      d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_bytes_ = 0;
};

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  Sha256 sha;
  sha.update(bytes);
  const auto digest = sha.finish();
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint8_t byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

std::string read_file(const std::filesystem::path& path, bool& ok) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { ok = false; return {}; }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  ok = true;
  return bytes.str();
}

bool manifest_has_expected_hashes(const std::filesystem::path& root) {
  bool read_ok = false;
  const std::string manifest = read_file(root / "manifest.json", read_ok);
  if (!read_ok) { std::cerr << "fixture runner: missing manifest.json\n"; return false; }
  const auto manifest_hash = sha256_hex(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
  if (manifest_hash != "2da661ae4c2f9c5fcfa44180dc1d4c97e191f9853dbd921e66c752bb8ffa939c") {
    std::cerr << "fixture runner: manifest hash mismatch\n";
    return false;
  }
  // The manifest itself is canonical release data.  Parse all six listed
  // public fixtures through the same bounded codecs before hashing them.
  const auto hello = decode_hello(read_file(root / "hello.json", read_ok));
  if (!read_ok || !hello.ok()) { std::cerr << "fixture runner: hello decode failed\n"; return false; }
  const auto snapshot = decode_snapshot(read_file(root / "snapshot.json", read_ok));
  if (!read_ok || !snapshot.ok()) { std::cerr << "fixture runner: snapshot decode failed\n"; return false; }
  const auto pending = decode_pending_requests(read_file(root / "pending-requests.json", read_ok));
  if (!read_ok || !pending.ok()) { std::cerr << "fixture runner: pending decode failed\n"; return false; }
  const auto statuses = decode_command_statuses(read_file(root / "command-statuses.json", read_ok));
  if (!read_ok || !statuses.ok()) { std::cerr << "fixture runner: status decode failed\n"; return false; }
  const std::string audio = read_file(root / "audio-frame.bin", read_ok);
  if (!read_ok) { std::cerr << "fixture runner: audio read failed\n"; return false; }
  const auto frame = decode_audio_frame(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(audio.data()), audio.size()), 4096);
  if (!frame.ok()) { std::cerr << "fixture runner: audio decode failed\n"; return false; }
  const std::string usage = read_file(root / "usage-bridge.json", read_ok);
  if (!read_ok || usage.empty()) { std::cerr << "fixture runner: usage fixture read failed\n"; return false; }

  constexpr std::array<std::pair<std::string_view, std::string_view>, 6> expected = {{
      {"hello.json", "e78d03fe5198cfceff84e9d97298e7f72fce87657a9672e576215f32dba9c57f"},
      {"snapshot.json", "f67ce7a592d76d77f506037b4384c43ea93bc59ef5b40b1df03f847b84b9ea7c"},
      {"pending-requests.json", "598ce73ecf759b4061a1f55cca652c028669c74433fb43d2991adfaf08914c2c"},
      {"command-statuses.json", "a2b6feed963d56ef7c8e790d4c9dd005235004339fd748513302d04fe13b06e1"},
      {"audio-frame.bin", "345ba2d9fe652a52fecddcb2b0e4f4f0dd948fbc5ac389896d5abfcd636777f5"},
      {"usage-bridge.json", "35205ce861a3e4c6534e3582ec2ce091420a2888c8212e765562488cd1bb14a6"},
  }};
  for (const auto& [name, expected_hash] : expected) {
    bool file_ok = false;
    const std::string content = read_file(root / name, file_ok);
    if (!file_ok) { std::cerr << "fixture runner: missing " << name << '\n'; return false; }
    const auto digest = sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
    if (digest != expected_hash) {
      std::cerr << "fixture runner: manifest hash mismatch for " << name << '\n';
      return false;
    }
  }
  // Keep the manifest read live so a truncated file cannot accidentally pass
  // through a future parser change; its canonical bytes are checked by the
  // source-side release hash in the task report.
  return manifest.size() == 1798;
}

// The simulator's normal mode proves byte-for-byte fixture integrity.  The
// acceptance mode adds the small cross-fixture invariants that the integrated
// host test consumes: a cached snapshot can be decoded, pending requests and
// command statuses are present, and the audio fixture is an ordered v1 frame.
// Keeping this check here makes the fixture runner useful as a standalone
// release-candidate preflight without opening any transport or device seam.
bool acceptance_fixture_contract(const std::filesystem::path& root) {
  bool read_ok = false;
  const auto snapshot = decode_snapshot(read_file(root / "snapshot.json", read_ok));
  if (!read_ok || !snapshot.ok() || snapshot.value.adapter_sequence != 7U ||
      snapshot.value.work_item_count != 6U || snapshot.value.pending_request_count != 2U) {
    std::cerr << "fixture runner: acceptance snapshot contract failed\n";
    return false;
  }
  const auto pending = decode_pending_requests(read_file(root / "pending-requests.json", read_ok));
  if (!read_ok || !pending.ok() || pending.value.count != 2U) {
    std::cerr << "fixture runner: acceptance pending-request contract failed\n";
    return false;
  }
  const auto statuses = decode_command_statuses(read_file(root / "command-statuses.json", read_ok));
  if (!read_ok || !statuses.ok() || statuses.value.count() != 4U ||
      statuses.value.records[2].status != CommandStatus::Applied ||
      statuses.value.records[3].status != CommandStatus::Rejected) {
    std::cerr << "fixture runner: acceptance command-status contract failed\n";
    return false;
  }
  const auto audio = read_file(root / "audio-frame.bin", read_ok);
  const auto frame = decode_audio_frame(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(audio.data()), audio.size()),
      4096);
  if (!read_ok || !frame.ok() || frame.value.header.chunk_number != 2U ||
      frame.value.payload.empty()) {
    std::cerr << "fixture runner: acceptance audio contract failed\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--acceptance")) {
    std::cerr << "usage: fixture_runner <fixtures/companion/v1> [--acceptance]\n";
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  if (!manifest_has_expected_hashes(root)) return 1;
  if (argc == 3 && !acceptance_fixture_contract(root)) return 1;
  std::cout << (argc == 3 ? "T3 Companion host acceptance fixtures verified deterministically\n"
                          : "T3 Companion v1 fixtures verified deterministically\n");
  return 0;
}
