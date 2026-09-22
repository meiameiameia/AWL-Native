#pragma once

namespace awl {

// Scans the mounted logical filesystem to identify and classify game asset candidates.
// This is a dev-only startup sequence and should not be used as permanent game boot behavior.
void asset_inventory_run(const char* logical_root);

} // namespace awl
