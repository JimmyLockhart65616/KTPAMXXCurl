#!/bin/bash
set -e

echo "========================================"
echo "KTPAmxxCurl Docker Build for Ubuntu 22.04"
echo "========================================"

cp -r /build/amxxcurl /tmp/amxxcurl
cd /tmp/amxxcurl

mkdir -p bin/ReleaseDLL obj

echo "Building AmxxCurl with static libraries..."

# Use static libs we built
STATIC_LIB_DIR="/usr/local/lib32"

CXXFLAGS="-m32 -O3 -fPIC -std=c++14 -fno-stack-protector -DCURL_STATICLIB"
DEFINES="-DNO_MSVC8_AUTO_COMPAT -DNDEBUG -DHAVE_STDINT_H -DASIO_STANDALONE"
INCLUDES="-Ideps/halflife/dlls -Ideps/halflife/engine -Ideps/halflife/common -Ideps/halflife/public -Ideps/metamod -Ideps/asio -I${STATIC_LIB_DIR}/include"

# Static linking with our built libraries
LDFLAGS="-m32 -shared -Wl,-soname=amxxcurl_ktp_i386.so -s"
STATIC_LIBS="${STATIC_LIB_DIR}/lib/libcurl.a ${STATIC_LIB_DIR}/lib/libssl.a ${STATIC_LIB_DIR}/lib/libcrypto.a ${STATIC_LIB_DIR}/lib/libz.a ${STATIC_LIB_DIR}/lib/libcares.a"
LIBS="-lpthread -lrt -ldl -static-libgcc -static-libstdc++"

echo "Compiling amx_curl_callback_class.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/amx_curl_callback_class.cc -o obj/amx_curl_callback_class.o

echo "Compiling asio_poller.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/asio_poller.cc -o obj/asio_poller.o

echo "Compiling callbacks.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/callbacks.cc -o obj/callbacks.o

echo "Compiling curl_multi_class.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/curl_multi_class.cc -o obj/curl_multi_class.o

echo "Compiling curl_natives.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/curl_natives.cc -o obj/curl_natives.o

echo "Compiling curl_utils_class.cc..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/curl_utils_class.cc -o obj/curl_utils_class.o

echo "Compiling amxxmodule.cpp..."
g++ ${CXXFLAGS} ${DEFINES} ${INCLUDES} -c src/sdk/amxxmodule.cpp -o obj/amxxmodule.o

echo "Linking with static libraries..."
g++ ${LDFLAGS} -o bin/ReleaseDLL/amxxcurl_ktp_i386.so \
    obj/amx_curl_callback_class.o \
    obj/asio_poller.o \
    obj/callbacks.o \
    obj/curl_multi_class.o \
    obj/curl_natives.o \
    obj/curl_utils_class.o \
    obj/amxxmodule.o \
    -Wl,--start-group ${STATIC_LIBS} -Wl,--end-group \
    ${LIBS}

if [ -f "bin/ReleaseDLL/amxxcurl_ktp_i386.so" ]; then
    echo ""
    echo "========================================"
    echo "BUILD SUCCESS!"
    echo "========================================"
    cp bin/ReleaseDLL/amxxcurl_ktp_i386.so /build/output/
    ls -lh /build/output/amxxcurl_ktp_i386.so
    echo ""
    echo "Checking dependencies..."
    ldd /build/output/amxxcurl_ktp_i386.so || true
else
    echo "BUILD FAILED!"
    exit 1
fi
