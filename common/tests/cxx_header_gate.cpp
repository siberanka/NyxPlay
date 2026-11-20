// The NyxPlay Host includes the library headers from C++20. This file must compile as C++17 and
// C++20 (no <stdatomic.h>) and the shared constants must be usable from C++ without a C compiler.

// The host includes the headers inside extern "C" blocks (see host/src/stream.cpp)
extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/Limelight-internal.h>
#include <moonlight-common-c/src/Rtsp.h>
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Platform.h>
#include <moonlight-common-c/src/Video.h>
}
#include <moonlight-common-c/src/NyxExtensions.h>

static_assert(LI_PACKET_TYPE_SET_TEXT_INPUT_STATE == 0x4E01, "text input packet type");
static_assert(LI_TEXT_INPUT_PAYLOAD_SIZE == 4, "text input payload size");
static_assert(sizeof(NV_VIDEO_PACKET) == 16, "video packet layout");
static_assert(sizeof(RTP_PACKET) == 12, "rtp packet layout");

// The atomics used by the headers must exist in C++
atomic_bool gateBool;
atomic_int gateInt;
atomic_uint gateUint;

int main() {
    CONNECTION_LISTENER_CALLBACKS callbacks;
    LiInitializeConnectionCallbacks(&callbacks);
    gateBool = true;
    gateInt = 1;
    gateUint = 2u;
    return (callbacks.setTextInputState == nullptr) ? 0 : 1;
}
