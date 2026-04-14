cmake_minimum_required(VERSION 3.10)
project(AmxxCurl CXX)

set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "")

# 32-bit target
set(COMPILE_FLAGS "-m32 -U_FORTIFY_SOURCE")
set(LINK_FLAGS "-m32")

# Warnings
set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wall -Wno-unknown-pragmas")

# Remove dead code
set(COMPILE_FLAGS "${COMPILE_FLAGS} -ffunction-sections -fdata-sections")

if (DEBUG)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g3 -O3 -ggdb")
else()
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g0 -O3 -flto -fno-math-errno -fno-stack-protector")
endif()

# Architecture
set(COMPILE_FLAGS "${COMPILE_FLAGS} -march=native -mtune=native -std=c++14 -fPIC")

# Defines
add_definitions(
	-DNO_MSVC8_AUTO_COMPAT
	-DNDEBUG
	-DHAVE_STDINT_H
	-DCURL_STATICLIB
	-DASIO_STANDALONE
)

# Sources
set(SOURCES
	src/asio_poller.cc
	src/callbacks.cc
	src/curl_multi_class.cc
	src/curl_natives.cc
	src/curl_utils_class.cc
	src/sdk/amxxmodule.cpp
)

# Include directories
include_directories(
	${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/deps/halflife/dlls
	${PROJECT_SOURCE_DIR}/deps/halflife/engine
	${PROJECT_SOURCE_DIR}/deps/halflife/common
	${PROJECT_SOURCE_DIR}/deps/halflife/public
	${PROJECT_SOURCE_DIR}/deps/metamod
	${PROJECT_SOURCE_DIR}/deps/asio
	${PROJECT_SOURCE_DIR}/deps/curl/include
)

# Build shared library
add_library(amxxcurl SHARED ${SOURCES})

set_target_properties(amxxcurl PROPERTIES
	OUTPUT_NAME "amxxcurl_ktp_i386"
	COMPILE_FLAGS "${COMPILE_FLAGS}"
	LINK_FLAGS "${LINK_FLAGS}"
)

# Link static deps
target_link_libraries(amxxcurl
	-Wl,--start-group
	"${PROJECT_SOURCE_DIR}/deps/curl/lib/libcurl.a"
	"${PROJECT_SOURCE_DIR}/deps/openssl/lib/libssl.a"
	"${PROJECT_SOURCE_DIR}/deps/openssl/lib/libcrypto.a"
	"${PROJECT_SOURCE_DIR}/deps/zlib/lib/libz.a"
	"${PROJECT_SOURCE_DIR}/deps/cares/lib/libcares.a"
	-Wl,--end-group
	pthread
	rt
)

# Link flags
set_target_properties(amxxcurl PROPERTIES
	LINK_FLAGS "${LINK_FLAGS} -flto -static-libgcc -static-libstdc++ -Wl,--no-as-needed -Wl,-gc-sections -s"
)
