# windows specific target definitions
set_target_properties(shell PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SHELL_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:shell_rc_object>
        Windowsapp.lib
        Wtsapi32.lib)
