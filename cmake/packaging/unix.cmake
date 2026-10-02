# unix specific packaging
# put anything here that applies to both linux and macos

# return here if building a macos package
if(SHELL_PACKAGE_MACOS)
    return()
endif()

# Installation destination dir
set(CPACK_SET_DESTDIR true)
if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "/usr/share/shell")
endif()

install(TARGETS shell RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
