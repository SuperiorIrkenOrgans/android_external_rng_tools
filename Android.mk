LOCAL_PATH:= $(call my-dir)

# ========================================================
# vim
# ========================================================
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	rngd.c stats.c rngd_entsource.c rngd_threads.c rngd_signals.c rngd_linux.c fips.c util.c

#LOCAL_C_INCLUDES += \
#        $(LOCAL_PATH) \
#        $(LOCAL_PATH)/proto \
#        $(LOCAL_PATH)/auto

LOCAL_SHARED_LIBRARIES += \
        libcutils 

#LOCAL_CFLAGS += \
#        -DHAVE_CONFIG_H \
#        -lncurses

LOCAL_MODULE := rngd
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

