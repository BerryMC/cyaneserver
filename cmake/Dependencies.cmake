find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)

macro(cyane_pkgconfig_dep pkg var)
  if(PkgConfig_FOUND)
    pkg_check_modules(${var} QUIET IMPORTED_TARGET ${pkg})
  endif()
endmacro()

# 压缩用 zlib 兼容 API（compress2/uncompress）。zlib-ng 的 compat 构建导出这些符号，
# 而 libz-ng 只提供 zng_* 原生 API，因此这里取 zlib 模块并检测其底层实现。
cyane_pkgconfig_dep(zlib CYANE_ZLIB)
cyane_pkgconfig_dep(openssl CYANE_OPENSSL)
cyane_pkgconfig_dep(liburing CYANE_LIBURING)

set(CYANE_HAVE_ZLIB OFF)
set(CYANE_HAVE_OPENSSL OFF)
set(CYANE_HAVE_LIBURING OFF)
set(CYANE_ZLIB_IMPL "none")

add_library(cyane_deps INTERFACE)
target_link_libraries(cyane_deps INTERFACE Threads::Threads)
add_library(cyane::deps ALIAS cyane_deps)

if(TARGET PkgConfig::CYANE_ZLIB)
  target_link_libraries(cyane_deps INTERFACE PkgConfig::CYANE_ZLIB)
  set(CYANE_HAVE_ZLIB ON)
  target_compile_definitions(cyane_deps INTERFACE CYANE_HAVE_ZLIB=1)
  if(CYANE_ZLIB_VERSION MATCHES "zlib-ng")
    set(CYANE_ZLIB_IMPL "zlib-ng ${CYANE_ZLIB_VERSION}")
  else()
    set(CYANE_ZLIB_IMPL "zlib ${CYANE_ZLIB_VERSION}")
  endif()
endif()

# AES-128-CFB8 登录加密与 RSA 密钥交换
if(TARGET PkgConfig::CYANE_OPENSSL)
  target_link_libraries(cyane_deps INTERFACE PkgConfig::CYANE_OPENSSL)
  set(CYANE_HAVE_OPENSSL ON)
  target_compile_definitions(cyane_deps INTERFACE CYANE_HAVE_OPENSSL=1)
endif()

# io_uring reactor 后端，缺失时运行期退回 epoll
if(TARGET PkgConfig::CYANE_LIBURING)
  target_link_libraries(cyane_deps INTERFACE PkgConfig::CYANE_LIBURING)
  set(CYANE_HAVE_LIBURING ON)
  target_compile_definitions(cyane_deps INTERFACE CYANE_HAVE_LIBURING=1)
endif()
