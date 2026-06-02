#pragma once

namespace popy::render {

// Apply resource limits for the current process.
bool set_limits();

// Enter the platform sandbox in the current process. Intended to run in the
// forked child after fd setup and before parsing untrusted bytes.
bool enter_sandbox();

}  // namespace popy::render
