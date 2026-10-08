cmake_minimum_required(VERSION 3.20)

project(shell_tools)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SHELL_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp utils.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SHELL_COMPILE_OPTIONS})

add_executable(shellsvc shellsvc.cpp)
set_target_properties(shellsvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(shellsvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(shellsvc PRIVATE ${SHELL_COMPILE_OPTIONS})

# Shell: the clipboard helper shell.exe runs as the signed-in user (no console window)
add_executable(shellclip WIN32
        shellclip.cpp
        "${CMAKE_SOURCE_DIR}/src/platform/windows/clipboard_access.cpp")
set_target_properties(shellclip PROPERTIES CXX_STANDARD 23)
target_link_libraries(shellclip
        ${CMAKE_THREAD_LIBS_INIT}
        ole32
        shell32
        user32
        uuid
        windowscodecs
        ${PLATFORM_LIBRARIES})
target_compile_options(shellclip PRIVATE ${SHELL_COMPILE_OPTIONS})
