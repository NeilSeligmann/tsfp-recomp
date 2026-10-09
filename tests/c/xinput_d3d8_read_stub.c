/* Test-only adapter: XInput oracle runners do not exercise D3D surfaces. */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool d3d8_gpu_read_guest(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    (void)address;
    if (out && bytes) memset(out, 0, bytes);
    return false;
}
