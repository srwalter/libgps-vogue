LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := libgps

LOCAL_SRC_FILES += \
    vogue_gps.c

include $(BUILD_SHARED_LIBRARY)
