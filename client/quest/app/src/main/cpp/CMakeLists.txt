cmake_minimum_required(VERSION 3.22)
project(metashare_quest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the Meta OpenXR Mobile SDK (override with -DMETA_OPENXR_SDK=...).
set(META_OPENXR_SDK "${CMAKE_SOURCE_DIR}/../../../../../third_party/ovr_openxr"
    CACHE PATH "Meta OpenXR Mobile SDK root")

# Shared wire protocol lives in the repo's src/common.
set(METASHARE_COMMON "${CMAKE_SOURCE_DIR}/../../../../../../src/common")

add_library(metashare_client SHARED
  app.cpp
  net_stream.cpp
  decoder.cpp)

target_include_directories(metashare_client PRIVATE
  ${METASHARE_COMMON}
  ${META_OPENXR_SDK}/OpenXR/Include
  ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_directories(metashare_client PRIVATE
  ${META_OPENXR_SDK}/OpenXR/Libs/Android/arm64-v8a/Release)

# native_app_glue (NativeActivity loop).
add_library(native_app_glue STATIC
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
  ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_libraries(metashare_client
  native_app_glue
  openxr_loader   # from the Meta SDK
  mediandk        # AMediaCodec
  android
  EGL
  GLESv3
  log)

# Export ANativeActivity_onCreate from native_app_glue.
set_target_properties(metashare_client PROPERTIES
  LINK_FLAGS "-u ANativeActivity_onCreate")
