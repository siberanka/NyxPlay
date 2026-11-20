# Android.mk for moonlight-core and binding
MY_LOCAL_PATH := $(call my-dir)

include $(call all-subdir-makefiles)

LOCAL_PATH := $(MY_LOCAL_PATH)

# The one copy of the library shared with the host and the PC client
MCC := ../../../../../../common/moonlight-common-c

include $(CLEAR_VARS)
LOCAL_MODULE    := moonlight-core

LOCAL_SRC_FILES := $(MCC)/src/AudioStream.c \
                   $(MCC)/src/ByteBuffer.c \
                   $(MCC)/src/Connection.c \
                   $(MCC)/src/ConnectionTester.c \
                   $(MCC)/src/ControlStream.c \
                   $(MCC)/src/FakeCallbacks.c \
                   $(MCC)/src/InputStream.c \
                   $(MCC)/src/LinkedBlockingQueue.c \
                   $(MCC)/src/Misc.c \
                   $(MCC)/src/Platform.c \
                   $(MCC)/src/PlatformCrypto.c \
                   $(MCC)/src/PlatformSockets.c \
                   $(MCC)/src/RtpAudioQueue.c \
                   $(MCC)/src/RtpVideoQueue.c \
                   $(MCC)/src/RtspConnection.c \
                   $(MCC)/src/RtspParser.c \
                   $(MCC)/src/SdpGenerator.c \
                   $(MCC)/src/SimpleStun.c \
                   $(MCC)/src/VideoDepacketizer.c \
                   $(MCC)/src/VideoStream.c \
                   $(MCC)/nanors/rs.c \
                   $(MCC)/nanors/deps/obl/oblas_common.c \
                   $(MCC)/nanors/deps/obl/oblas_lite.c \
                   $(MCC)/enet/callbacks.c \
                   $(MCC)/enet/compress.c \
                   $(MCC)/enet/host.c \
                   $(MCC)/enet/list.c \
                   $(MCC)/enet/packet.c \
                   $(MCC)/enet/peer.c \
                   $(MCC)/enet/protocol.c \
                   $(MCC)/enet/unix.c \
                   $(MCC)/enet/win32.c \
                   simplejni.c \
                   callbacks.c \
                   minisdl.c \


LOCAL_C_INCLUDES := $(LOCAL_PATH)/$(MCC)/enet/include \
                    $(LOCAL_PATH)/$(MCC)/nanors \
                    $(LOCAL_PATH)/$(MCC)/nanors/deps \
                    $(LOCAL_PATH)/$(MCC)/nanors/deps/obl \
                    $(LOCAL_PATH)/$(MCC)/src \

LOCAL_CFLAGS := -DHAS_SOCKLEN_T=1 -DLC_ANDROID -DHAVE_CLOCK_GETTIME=1

ifeq ($(NDK_DEBUG),1)
LOCAL_CFLAGS += -DLC_DEBUG
endif

LOCAL_LDLIBS := -llog

LOCAL_STATIC_LIBRARIES := libopus libssl libcrypto cpufeatures
LOCAL_LDFLAGS += -Wl,--exclude-libs,ALL

LOCAL_BRANCH_PROTECTION := standard

include $(BUILD_SHARED_LIBRARY)

$(call import-module,android/cpufeatures)