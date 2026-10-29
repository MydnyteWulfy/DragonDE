// Process-wide settings for the test binary, applied before any test runs.

#include <cstdlib>

namespace {

struct TestEnvironment {
    TestEnvironment()
    {
        // The validation layer re-checks our SPIR-V on every device (and most
        // GPU tests make one), costing seconds each. Every debug compositor run
        // still checks the shaders; here it only slows the suite down.
        // (0: an explicit setting from the caller wins.)
        setenv("VK_KHRONOS_VALIDATION_CHECK_SHADERS", "false", 0);
    }
} const environment;

} // namespace
