// Input stream: use after free of a queued controller packet when the input queue is full
// (upstream 970b072) and invalid motion types (upstream f0df33c).
//
// This file includes InputStream.c to reach its static state.

#include "testutil.h"

#include "InputStream.c"

static void setup(void) {
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = 0;
    SunshineFeatureFlags = 0;
    CHECK_EQ_INT(initializeInputStream(), 0);
    // No sender thread runs in this test: queued packets stay in the queue
    atomic_store_explicit(&initialized, true, memory_order_release);
}

static void teardown(void) {
    atomic_store_explicit(&initialized, false, memory_order_release);
    destroyInputStream();
}

static void testFullQueueDoesNotLeaveDanglingBatchPointer(void) {
    int firstFailure = -1;

    setup();

    // Different button flags end the batch every time, so each event queues its own packet holder
    for (int i = 0; i < MAX_QUEUED_INPUT_PACKETS + 50; i++) {
        int ret = LiSendMultiControllerEvent(0, 1, 0x0001 + i, 0, 0, 0, 0, 0, 0);
        if (ret != 0 && firstFailure < 0) {
            firstFailure = i;
        }
        if (ret != 0) {
            // The packet that did not fit was freed, so it must not stay registered for batching
            CHECK(currentQueuedControllerPacket[0] == NULL);
            // The next event with the same buttons must allocate a fresh holder, not touch the freed one
            ret = LiSendMultiControllerEvent(0, 1, 0x0001 + i, 10, 0, 0, 0, 0, 0);
            CHECK(ret != 0);
            CHECK(currentQueuedControllerPacket[0] == NULL);
        }
    }
    CHECK_EQ_INT(firstFailure, MAX_QUEUED_INPUT_PACKETS);
    teardown();
}

static void testMotionTypeValidation(void) {
    setup();
    SunshineFeatureFlags = LI_FF_CONTROLLER_TOUCH_EVENTS;
    AppVersionQuad[3] = -1; // Sunshine style host

    // 0 would index the sensor state array at -1 (out of bounds write)
    CHECK_EQ_INT(LiSendControllerMotionEvent(0, 0, 1.0f, 2.0f, 3.0f), -3);
    CHECK_EQ_INT(LiSendControllerMotionEvent(0, MAX_MOTION_EVENTS + 1, 1.0f, 2.0f, 3.0f), -3);
    CHECK_EQ_INT(LiSendControllerMotionEvent(0, 255, 1.0f, 2.0f, 3.0f), -3);
    CHECK_EQ_INT(LiSendControllerMotionEvent(0, LI_MOTION_TYPE_ACCEL, 1.0f, 2.0f, 3.0f), 0);
    CHECK_EQ_INT(LiSendControllerMotionEvent(15, LI_MOTION_TYPE_GYRO, 1.0f, 2.0f, 3.0f), 0);
    teardown();
}

int main(void) {
    RUN(testFullQueueDoesNotLeaveDanglingBatchPointer);
    RUN(testMotionTypeValidation);
    return testFinish("input");
}
