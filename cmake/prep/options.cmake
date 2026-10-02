# Publisher Metadata
set(SHELL_PUBLISHER_NAME "Juno Park"
        CACHE STRING "The name of the publisher (or fork developer) of the application.")
set(SHELL_PUBLISHER_WEBSITE "https://github.com/junopark00/hermit-shell"
        CACHE STRING "The URL of the publisher's website.")
set(SHELL_PUBLISHER_ISSUE_URL "https://github.com/junopark00/hermit-shell/issues"
        CACHE STRING "The URL of the publisher's support site or issue tracker.
        If you provide a modified version of Shell, we kindly request that you use your own url.")

option(BUILD_TESTS "Build tests" OFF)
option(NPM_OFFLINE "Use offline npm packages. You must ensure packages are in your npm cache." OFF)

option(BUILD_WERROR "Enable -Werror flag." OFF)

# if this option is set, the build will exit after configuring special package configuration files
option(SHELL_CONFIGURE_ONLY "Configure special files only, then exit." OFF)

option(SHELL_ENABLE_TRAY "Enable system tray icon." ON)

option(SHELL_SYSTEM_WAYLAND_PROTOCOLS "Use system installation of wayland-protocols rather than the submodule." OFF)

if(APPLE)
    option(BOOST_USE_STATIC "Use static boost libraries." OFF)
else()
    option(BOOST_USE_STATIC "Use static boost libraries." ON)
endif()

option(CUDA_FAIL_ON_MISSING "Fail the build if CUDA is not found." ON)
option(CUDA_INHERIT_COMPILE_OPTIONS
        "When building CUDA code, inherit compile options from the the main project. You may want to disable this if
        your IDE throws errors about unknown flags after running cmake." ON)

if(UNIX)
    option(SHELL_BUILD_HOMEBREW
            "Enable a Homebrew build." OFF)
    option(SHELL_CONFIGURE_HOMEBREW
            "Configure Homebrew formula. Recommended to use with SHELL_CONFIGURE_ONLY" OFF)
endif()

if(APPLE)
    option(SHELL_CONFIGURE_PORTFILE
            "Configure macOS Portfile. Recommended to use with SHELL_CONFIGURE_ONLY" OFF)
    option(SHELL_PACKAGE_MACOS
            "Should only be used when creating a macOS package/dmg." OFF)
elseif(UNIX)  # Linux
    option(SHELL_BUILD_APPIMAGE
            "Enable an AppImage build." OFF)
    option(SHELL_BUILD_FLATPAK
            "Enable a Flatpak build." OFF)
    option(SHELL_CONFIGURE_PKGBUILD
            "Configure files required for AUR. Recommended to use with SHELL_CONFIGURE_ONLY" OFF)
    option(SHELL_CONFIGURE_FLATPAK_MAN
            "Configure manifest file required for Flatpak build. Recommended to use with SHELL_CONFIGURE_ONLY" OFF)

    # Linux capture methods
    option(SHELL_ENABLE_CUDA
            "Enable cuda specific code." ON)
    option(SHELL_ENABLE_DRM
            "Enable KMS grab if available." ON)
    option(SHELL_ENABLE_VAAPI
            "Enable building vaapi specific code." ON)
    option(SHELL_ENABLE_WAYLAND
            "Enable building wayland specific code." ON)
    option(SHELL_ENABLE_X11
            "Enable X11 grab if available." ON)
endif()
