#
# Copyright (C) 2025 The pgexporter community
#
# Test suite for pgexporter
#

# Find Check framework
find_package(PkgConfig REQUIRED)
pkg_check_modules(CHECK check)

if (CHECK_FOUND)
  set(SOURCES
    pgexporter_test.c
  )

  add_compile_options(-O0)
  add_compile_options(-DDEBUG)
  add_compile_options(-Wno-unused-parameter)
  
  if (CMAKE_BUILD_TYPE MATCHES Debug)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
      if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "OpenBSD")
        if (NOT ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
          add_compile_options(-fsanitize=address)
          add_compile_options(-fsanitize=undefined)
          add_compile_options(-fno-sanitize-recover=all)
          add_compile_options(-fsanitize=float-divide-by-zero)
          add_compile_options(-fsanitize=float-cast-overflow)
          add_compile_options(-fno-sanitize=null)
          add_compile_options(-fno-sanitize=alignment)

          set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment")
        endif()
      endif()
    endif()
  endif()

  add_executable(pgexporter_test ${SOURCES})
  target_include_directories(pgexporter_test PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

  # Link libraries based on platform
  if(EXISTS "/etc/debian_version")
    target_link_libraries(pgexporter_test ${CHECK_LIBRARIES} subunit pthread rt m pgexporter)
  elseif(APPLE)
    target_link_libraries(pgexporter_test ${CHECK_LIBRARIES} m pgexporter)
  else()
    target_link_libraries(pgexporter_test ${CHECK_LIBRARIES} pthread rt m pgexporter)
  endif()

  target_include_directories(pgexporter_test PRIVATE ${CHECK_INCLUDE_DIRS})
  target_compile_options(pgexporter_test PRIVATE ${CHECK_CFLAGS_OTHER})

endif()

# Copy testsuite script to build directory
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/testsuite.sh"
  "${CMAKE_BINARY_DIR}/testsuite.sh"
  COPYONLY
)