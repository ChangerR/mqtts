if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libco/CMakeLists.txt")
  message(FATAL_ERROR "libco submodule is missing. Please run: git submodule update --init --recursive")
endif()

# libco upstream CMakeLists misses co_comm.cpp in SOURCE_FILES on some revisions.
# Apply local patch before add_subdirectory to guarantee consistent build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libco.patch")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}/libco apply --check ${CMAKE_CURRENT_SOURCE_DIR}/libco.patch
    RESULT_VARIABLE LIBCO_PATCH_CHECK_RET
    OUTPUT_QUIET
    ERROR_QUIET
  )

  if(LIBCO_PATCH_CHECK_RET EQUAL 0)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}/libco apply ${CMAKE_CURRENT_SOURCE_DIR}/libco.patch
      RESULT_VARIABLE LIBCO_PATCH_RET
    )
    if(NOT LIBCO_PATCH_RET EQUAL 0)
      message(FATAL_ERROR "Failed to apply libco.patch")
    endif()
  endif()
endif()

add_subdirectory(libco)
