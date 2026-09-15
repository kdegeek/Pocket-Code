// Decode adapter frames with the actual firmware parser, not a Python imitation.
#include "envelope.hpp"
#include <iostream>
#include <string>

int main() {
  std::string line;
  unsigned count = 0;
  while (std::getline(std::cin, line)) {
    const auto decoded = t3::companion::decode_envelope(line);
    if (!decoded.ok()) {
      std::cerr << "Firmware rejected frame: " << decoded.error.message << '\n';
      return 1;
    }
    ++count;
  }
  if (count == 0) return 2;
  std::cout << "Firmware accepted " << count << " adapter frames\n";
}
