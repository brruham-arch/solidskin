LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := solidskin

LOCAL_SRC_FILES := \
    ../mod/main.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../include \
    $(LOCAL_PATH)/../include/AML

LOCAL_CPPFLAGS := \
    -std=c++17 \
    -O2 \
    -fPIC \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections

LOCAL_LDLIBS := \
    -llog \
    -lm \
    -ldl \
    -lGLESv2

LOCAL_LDFLAGS := \
    -static-libstdc++ \
    -Wl,--gc-sections

include $(BUILD_SHARED_LIBRARY)
