set(GG_MINGW_TRIPLET "x86_64-w64-mingw32" CACHE STRING "MinGW-w64 target triplet")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(CMAKE_C_COMPILER NAMES "${GG_MINGW_TRIPLET}-gcc" REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES "${GG_MINGW_TRIPLET}-g++" REQUIRED)

get_filename_component(_gg_mingw_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
set(_gg_mingw_root_candidates
    "/usr/${GG_MINGW_TRIPLET}"
    "/usr/local/${GG_MINGW_TRIPLET}"
    "${_gg_mingw_compiler_dir}/../${GG_MINGW_TRIPLET}"
)
foreach(_gg_mingw_root IN LISTS _gg_mingw_root_candidates)
  if(EXISTS "${_gg_mingw_root}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${_gg_mingw_root}")
  endif()
endforeach()
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
