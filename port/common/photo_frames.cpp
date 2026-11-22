// Upstream's native_frames.cpp (its frame rates above 60, which this port leaves out) also tells Photo Mode when the
// title renders its text queues: each one starts a new frame, in which Photo Mode queues its controls again. That call
// on its own, on the same function.
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "photo_mode.h"

X86_EXTERN_FUNC(__imp__sub_001EE6B0);
X86_FUNC(sub_001EE6B0) {
    // text queues draw after the 3D
    openshao::photo_mode::BeforeText();
    __imp__sub_001EE6B0(ctx, base);
}
