set(CYANE_WARNINGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Woverloaded-virtual
  -Wconversion
  -Wsign-conversion
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wctad-maybe-unsupported
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND CYANE_WARNINGS -Wdeprecated-copy-with-user-provided-dtor)
endif()

set(CYANE_DEFINES)

# 开发/测试期临时关闭 -O3，显著缩短编译时间；恢复发布构建时把下面注释掉的 -O3 打开即可
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND CYANE_OPT_FLAGS -O1 -g3 -fno-omit-frame-pointer)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  # list(APPEND CYANE_OPT_FLAGS -O3 -g -fno-omit-frame-pointer)
  list(APPEND CYANE_OPT_FLAGS -O0 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  # list(APPEND CYANE_OPT_FLAGS -O3)
  list(APPEND CYANE_OPT_FLAGS -O0)
endif()

list(APPEND CYANE_DEFINES
  $<$<CONFIG:Debug>:CYANE_DEBUG=1>
  $<$<NOT:$<CONFIG:Debug>>:CYANE_DEBUG=0>
)

if(CYANE_WERROR)
  list(APPEND CYANE_WARNINGS -Werror)
endif()

add_link_options(-fuse-ld=lld)
