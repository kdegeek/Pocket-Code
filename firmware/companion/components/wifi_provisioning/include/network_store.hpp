#pragma once

#include "credential_store.hpp"

namespace t3::companion {

// The policy lives in persistence so the same bounded list is used by the
// captive portal and station roaming. These aliases keep the Wi-Fi-facing
// seam discoverable without duplicating credential state.
using NetworkCredential = RememberedNetwork;
using NetworkStore = CredentialStore;

namespace wifi_provisioning {
using ::t3::companion::CredentialMutation;
using ::t3::companion::CredentialMutationResult;
using ::t3::companion::NetworkCredential;
using ::t3::companion::NetworkStore;
}  // namespace wifi_provisioning

}  // namespace t3::companion
