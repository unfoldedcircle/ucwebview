# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# The uc_webview plugin of flutter-pi. flutter-pi's CMakeLists.txt includes this file when BUILD_UC_WEBVIEW_PLUGIN is
# ON (patches/0001-Add-the-uc_webview-plugin-option.patch), with this directory copied to src/plugins/uc_webview.
# The sources go into flutterpi_module like those of flutter-pi's own plugins; FLUTTERPI_PLUGIN registers the plugin.
# The library is dlopen()ed at run time: the build needs only its header, uc_webview.h of the same ucwebview release.

if(NOT HAVE_EGL_GLES2)
  message(SEND_ERROR "The uc_webview plugin needs EGL and OpenGL ES (ENABLE_OPENGL).")
  return()
endif()

pkg_check_modules(UC_WEBVIEW_GLIB REQUIRED IMPORTED_TARGET glib-2.0)

set(UC_WEBVIEW_PLUGIN_DIR ${CMAKE_CURRENT_LIST_DIR})

# uc_webview.h: copied next to the core sources (firmware builds), or the one of the ucwebview checkout this
# directory belongs to (builds from the repository)
if(EXISTS ${UC_WEBVIEW_PLUGIN_DIR}/core/uc_webview.h)
  set(UC_WEBVIEW_HEADER_DIR ${UC_WEBVIEW_PLUGIN_DIR}/core)
elseif(EXISTS ${UC_WEBVIEW_PLUGIN_DIR}/../lib/uc_webview.h)
  set(UC_WEBVIEW_HEADER_DIR ${UC_WEBVIEW_PLUGIN_DIR}/../lib)
else()
  message(SEND_ERROR "uc_webview.h not found: copy lib/uc_webview.h of the same ucwebview release to ${UC_WEBVIEW_PLUGIN_DIR}/core/.")
  return()
endif()

target_sources(flutterpi_module PRIVATE
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_codec.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_dump.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_host.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_loader.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_pacing.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_protocol.c
  ${UC_WEBVIEW_PLUGIN_DIR}/core/ucfl_views.c
  ${UC_WEBVIEW_PLUGIN_DIR}/flutterpi/ucfp_glib_loop.c
  ${UC_WEBVIEW_PLUGIN_DIR}/flutterpi/uc_webview_plugin.c
)
target_include_directories(flutterpi_module PRIVATE
  ${UC_WEBVIEW_PLUGIN_DIR}/core
  ${UC_WEBVIEW_HEADER_DIR}
)
target_link_libraries(flutterpi_module PUBLIC PkgConfig::UC_WEBVIEW_GLIB)
