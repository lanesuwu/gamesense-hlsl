#pragma once
#include <Windows.h>

namespace native_hlsl::detail {
using ProtocolAction = void (*)(void *);

// Keep this in a separate frame with no C++ objects requiring unwinding.
// /EHsc alone does not run a C++ lock_guard destructor for a Windows SEH unwind.
// Release the gate without catching or suppressing the original exception.
__declspec(noinline) inline bool try_protocol(SRWLOCK &gate, ProtocolAction action, void *context,
                                              volatile LONG *unwinds = nullptr) {
    if (!TryAcquireSRWLockExclusive(&gate))
        return false;
    __try {
        action(context);
    } __finally {
        if (unwinds && AbnormalTermination())
            InterlockedIncrement(unwinds);
        ReleaseSRWLockExclusive(&gate);
    }
    return true;
}

template <typename Action>
bool try_protocol(SRWLOCK &gate, Action &action, volatile LONG *unwinds = nullptr) {
    return try_protocol(gate, [](void *context) { (*static_cast<Action *>(context))(); }, &action, unwinds);
}
} // namespace native_hlsl::detail
