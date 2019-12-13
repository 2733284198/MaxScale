#
# Builds libmemcached: https://libmemcached.org/libMemcached.html
#
# Sources taken from https://launchpad.net/libmemcached/+download
#
# The following variables are set:
# LIBMEMCACHED_VERSION   - The libcached version used.
# LIBMEMCACHED_URL       - The download URL.
# LIBMEMCACHED_INCLUDE   - The include directories
# LIBMEMCACHED_LIBRARIES - The libraries to link
#

set(LIBMEMCACHED_VERSION "1.0.18")

message(STATUS "Using libmemcached version ${LIBMEMCACHED_VERSION}")

set(LIBMEMCACHED_URL "https://launchpad.net/libmemcached/1.0/${LIBMEMCACHED_VERSION}/+download/libmemcached-${LIBMEMCACHED_VERSION}.tar.gz")

ExternalProject_Add(libmemcached
  URL ${LIBMEMCACHED_URL}
    SOURCE_DIR ${CMAKE_BINARY_DIR}/libmemcached/
  CONFIGURE_COMMAND ${CMAKE_BINARY_DIR}/libmemcached//configure --prefix=${CMAKE_BINARY_DIR}/libmemcached/ --enable-shared --with-pic --libdir=${CMAKE_BINARY_DIR}/libmemcached/lib/
  BINARY_DIR ${CMAKE_BINARY_DIR}/libmemcached/
  BUILD_COMMAND make
  INSTALL_COMMAND make install)

set(LIBMEMCACHED_INCLUDE_DIR ${CMAKE_BINARY_DIR}/libmemcached/include CACHE INTERNAL "")
set(LIBMEMCACHED_LIBRARIES ${CMAKE_BINARY_DIR}/libmemcached/lib/libmemcached.a)
