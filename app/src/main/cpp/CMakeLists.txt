cmake_minimum_required(VERSION 3.22)
project(wlbridge C)
add_library(wlbridge SHARED wlbridge.c ahb.c)
# Pixel copies and compositing run per frame; debug builds would otherwise use -O0.
target_compile_options(wlbridge PRIVATE -O3 -Wall)
# nativewindow: AHardwareBuffer; android: ASurfaceControl.
target_link_libraries(wlbridge android nativewindow log)
# android.airpods' L2CAP socket, see aap.c.
add_library(aap SHARED aap.c)
target_compile_options(aap PRIVATE -Wall)
target_link_libraries(aap log)
# Debug builds only (app/build.gradle.kts): the zero-copy P0 test, app/src/debug/cpp.
if(OMA_ZC_P0)
  add_library(zcp0 SHARED ../../debug/cpp/zcp0.c)
  target_compile_options(zcp0 PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(zcp0 android nativewindow log m)
endif()
