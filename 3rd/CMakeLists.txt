if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libco/CMakeLists.txt")
  message(FATAL_ERROR "libco submodule is missing. Please run: git submodule update --init --recursive")
endif()

add_subdirectory(libco)

# Some upstream revisions omit co_comm.cpp. Add it to the targets instead of
# mutating the checkout with git apply, which silently fails in Docker contexts
# and source archives without .git metadata.
foreach(target colib_static colib_shared)
  if(TARGET ${target})
    get_target_property(libco_sources ${target} SOURCES)
    set(has_co_comm FALSE)
    foreach(source ${libco_sources})
      if(source MATCHES "(^|/)co_comm[.]cpp$")
        set(has_co_comm TRUE)
      endif()
    endforeach()
    if(NOT has_co_comm)
      target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libco/co_comm.cpp")
    endif()
  endif()
endforeach()
