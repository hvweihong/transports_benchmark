# Findfoonathan_memory.cmake
# This module finds foonathan_memory when it's built via add_subdirectory

if(TARGET foonathan_memory)
    set(foonathan_memory_FOUND TRUE)
    # Create an alias if it doesn't exist
    if(NOT TARGET foonathan_memory::foonathan_memory)
        add_library(foonathan_memory::foonathan_memory ALIAS foonathan_memory)
    endif()
else()
    set(foonathan_memory_FOUND FALSE)
endif()
