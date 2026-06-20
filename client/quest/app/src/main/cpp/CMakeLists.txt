cmake_minimum_required(VERSION 3.22)
project(metashare_quest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenXR: Khronos loader — headers + a prebuilt Android arm64 shared lib under
# lib/<abi>/libopenxr_loader.so (fetched by client/quest/fetch-openxr.sh).
# Override with -DOPENXR_ROOT=...; the Meta OpenXR Mobile SDK layout also works
# if you point this at its root and pass -DOPENXR_LIB=<abs path to .so/.a>.
set(OPENXR_ROOT "${CMAKE_SOURCE_DIR}/../../../../../../third_party/openxr"
    CACHE PATH "OpenXR SDK root (include/openxr/*.h + lib/<abi>/libopenxr_loader.so)")

# Shared wire protocol lives in the repo's src/common.
set(METASHARE_COMMON "${CMAKE_SOURCE_DIR}/../../../../../../src/common")

add_library(metashare_client SHARED
  app.cpp
  egl.cpp
  net_stream.cpp
  decoder.cpp
  renderer.cpp)

target_include_directories(metashare_client PRIVATE
  ${METASHARE_COMMON}
  ${OPENXR_ROOT}/include
  ${ANDROID_NDK}/sources/android/native_app_glue)

# native_app_glue (NativeActivity loop).
add_library(native_app_glue STATIC
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
  ${ANDROID_NDK}/sources/android/native_app_glue)

# The prebuilt OpenXR loader. Packaged into the APK via jniLibs (build.gradle).
if(EXISTS "${OPENXR_ROOT}/lib/${ANDROID_ABI}/libopenxr_loader.so")
  set(OPENXR_LIB "${OPENXR_ROOT}/lib/${ANDROID_ABI}/libopenxr_loader.so")
elseif(EXISTS "${OPENXR_ROOT}/lib/${ANDROID_ABI}/Release/libopenxr_loader.a")
  set(OPENXR_LIB "${OPENXR_ROOT}/lib/${ANDROID_ABI}/Release/libopenxr_loader.a")
else()
  set(OPENXR_LIB "openxr_loader")
  message(WARNING "OpenXR loader not found under ${OPENXR_ROOT}/lib; "
                  "falling back to -lopenxr_loader (set -DOPENXR_ROOT/-DOPENXR_LIB).")
endif()

target_link_libraries(metashare_client
  native_app_glue
  ${OPENXR_LIB}
  mediandk        # AMediaCodec
  android
  EGL
  GLESv3
  log)

# Export ANativeActivity_onCreate from native_app_glue.
set_target_properties(metashare_client PROPERTIES
  LINK_FLAGS "-u ANativeActivity_onCreate")
