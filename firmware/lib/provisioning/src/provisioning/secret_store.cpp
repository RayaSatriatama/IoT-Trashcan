#include "secret_store.h"

namespace sigap::provisioning {

// The concrete NVS SecretStore is intentionally deferred to the provisioning
// slice. This translation unit keeps the boundary visible without creating a
// credentials implementation or plaintext fixture in the foundation slice.

}  // namespace sigap::provisioning
