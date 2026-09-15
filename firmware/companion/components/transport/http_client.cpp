#include "http_client.hpp"

namespace t3::companion {

// HTTP is deliberately supplied by the ESP-IDF composition layer.  Keeping
// this translation unit makes the component linkable in the host fake build
// without pulling a network stack into protocol/state tests.

}  // namespace t3::companion
