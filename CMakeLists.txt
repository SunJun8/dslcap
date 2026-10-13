# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(dslcap C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DSVIEW_DIR "$ENV{HOME}/Downloads/DSView-1.3.2"
    CACHE PATH "DSView source tree providing libsigrok4DSL and firmware")
if(NOT EXISTS "${DSVIEW_DIR}/libsigrok4DSL/lib_main.c")
  message(FATAL_ERROR "DSVIEW_DIR=${DSVIEW_DIR} is not a DSView source tree")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET glib-2.0 libusb-1.0 zlib)
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Same source list as DSView's own CMakeLists.txt.
set(SIGROK4DSL_SOURCES
    libsigrok4DSL/version.c
    libsigrok4DSL/strutil.c
    libsigrok4DSL/std.c
    libsigrok4DSL/session_driver.c
    libsigrok4DSL/session.c
    libsigrok4DSL/log.c
    libsigrok4DSL/hwdriver.c
    libsigrok4DSL/error.c
    libsigrok4DSL/backend.c
    libsigrok4DSL/output/output.c
    libsigrok4DSL/input/input.c
    libsigrok4DSL/hardware/demo/demo.c
    libsigrok4DSL/input/in_binary.c
    libsigrok4DSL/input/in_vcd.c
    libsigrok4DSL/input/in_wav.c
    libsigrok4DSL/output/csv.c
    libsigrok4DSL/output/gnuplot.c
    libsigrok4DSL/output/srzip.c
    libsigrok4DSL/output/vcd.c
    libsigrok4DSL/hardware/DSL/dslogic.c
    libsigrok4DSL/hardware/common/usb.c
    libsigrok4DSL/hardware/common/ezusb.c
    libsigrok4DSL/trigger.c
    libsigrok4DSL/dsdevice.c
    libsigrok4DSL/hardware/DSL/dscope.c
    libsigrok4DSL/hardware/DSL/command.c
    libsigrok4DSL/hardware/DSL/dsl.c
    libsigrok4DSL/lib_main.c
    common/minizip/zip.c
    common/minizip/unzip.c
    common/minizip/ioapi.c
    common/log/xlog.c)
list(TRANSFORM SIGROK4DSL_SOURCES PREPEND "${DSVIEW_DIR}/")

add_library(sigrok4dsl STATIC ${SIGROK4DSL_SOURCES})
target_include_directories(sigrok4dsl PUBLIC "${DSVIEW_DIR}"
                                             "${DSVIEW_DIR}/common")
target_include_directories(sigrok4dsl PRIVATE "${DSVIEW_DIR}/libsigrok4DSL")
target_compile_options(sigrok4dsl PRIVATE -w)
target_link_libraries(sigrok4dsl PUBLIC PkgConfig::DEPS Threads::Threads m)

add_library(dslcap_convert STATIC convert.c)
target_compile_options(dslcap_convert PRIVATE -Wall -Wextra)

add_executable(dslcap dslcap.c)
target_compile_options(dslcap PRIVATE -Wall -Wextra)
target_compile_definitions(dslcap
                           PRIVATE DSLCAP_FW_DIR="${DSVIEW_DIR}/DSView/res")
target_link_libraries(dslcap PRIVATE sigrok4dsl dslcap_convert)

enable_testing()
add_executable(test_convert test_convert.c)
target_compile_options(test_convert PRIVATE -Wall -Wextra)
target_link_libraries(test_convert PRIVATE dslcap_convert)
add_test(NAME convert COMMAND test_convert)
