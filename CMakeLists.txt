cmake_minimum_required(VERSION 3.24)
project(aes67-vsc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
endif()

if(APPLE)
# libaspl probes the sysroot itself and warns when cmake has not set one -
# resolve it once via xcrun so the probe is never needed
if(NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE _macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_macos_sdk)
        set(CMAKE_OSX_SYSROOT "${_macos_sdk}" CACHE PATH "macOS SDK" FORCE)
    endif()
endif()
# libaspl (pinned commit) calls add_custom_command without PRE/POST_BUILD -
# default the new CMP0175 check to the old behaviour for the subproject
set(CMAKE_POLICY_DEFAULT_CMP0175 OLD)
include(FetchContent)
FetchContent_Declare(libASPL
    GIT_REPOSITORY https://github.com/gavv/libASPL.git
    GIT_TAG 633e0f70203edd87d320fc5a3cae901e1363aac5
)
FetchContent_MakeAvailable(libASPL)
endif()

# ---- common: ported pi-audio-node protocol stack + macos compat ----
add_library(aes67-common STATIC
    common/config.cpp
    common/log.cpp
    common/compat/net_compat.cpp
    common/compat/time_compat.cpp
    common/ptp/ptpclient.cpp
    common/ptp/bmca.cpp
    common/rtp/sdp.cpp
    common/rtp/rtpsender.cpp
    common/rtp/rtpreceiver.cpp
    common/audio/sources.cpp
    common/audio/meters.cpp
    common/nmos/node.cpp
    common/nmos/connection.cpp
    common/nmos/is12.cpp
    common/nmos/dnssd.cpp
    common/web/server.cpp
    third_party/civetweb.c
)
target_include_directories(aes67-common PUBLIC common third_party)
target_compile_definitions(aes67-common PUBLIC
    NO_SSL NO_CGI NO_CACHING USE_WEBSOCKET
    APP_VERSION="${PROJECT_VERSION}"
)
set_source_files_properties(third_party/civetweb.c PROPERTIES COMPILE_FLAGS "-w")
target_compile_options(aes67-common PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
)
find_package(Threads REQUIRED)
target_link_libraries(aes67-common PUBLIC Threads::Threads resolv)

# ---- daemon (macos) / gateway (linux, headless docker build) ----
if(APPLE)
enable_language(OBJCXX)
add_executable(aes67vsc-daemon
    daemon/src/main.cpp
    daemon/src/coreaudio/hal_device.cpp
    daemon/src/coreaudio/monitor_out.cpp
    daemon/src/menubar.mm
)
target_include_directories(aes67vsc-daemon PRIVATE daemon/src)
target_compile_options(aes67vsc-daemon PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(daemon/src/menubar.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_link_libraries(aes67vsc-daemon PRIVATE
    aes67-common
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework Cocoa"
    "-framework AVFoundation"
)

# ---- hal driver ----
add_library(aes67vsc-driver MODULE
    driver/src/EntryPoint.cpp
    driver/src/PtpClockedDevice.cpp
)
target_include_directories(aes67vsc-driver PRIVATE driver/src common)
target_compile_options(aes67vsc-driver PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(aes67vsc-driver PRIVATE
    libASPL
    "-framework CoreAudio"
    "-framework CoreFoundation"
)
set_target_properties(aes67vsc-driver PROPERTIES
    OUTPUT_NAME "AES67VSC"
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/driver/Info.plist.in"
)
else()
# ---- linux gateway: same main.cpp, stub hal/monitor/menubar, no driver ----
add_executable(aes67vsc-gateway
    daemon/src/main.cpp
    daemon/src/linux/platform.cpp
)
target_include_directories(aes67vsc-gateway PRIVATE daemon/src)
target_compile_options(aes67vsc-gateway PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(aes67vsc-gateway PRIVATE aes67-common rt)
endif()
