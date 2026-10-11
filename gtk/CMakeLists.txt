cmake_minimum_required(VERSION 3.25)
project(SvinLauncher LANGUAGES CXX)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.6)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)
find_package(ZLIB REQUIRED)
find_package(Intl REQUIRED)
find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)

set(SVIN_APP_ID "org.svinlauncher.SvinLauncher" CACHE STRING "Desktop application ID")
set(SVIN_DISPLAY_NAME "SvinLauncher" CACHE STRING "Desktop application name")

add_executable(svinlauncher
    app/main.cpp
    app/notifications.cpp
    app/settings.cpp
    app/window.cpp
    instances/create_dialog.cpp
    instances/instance_list.cpp
    instances/instance_store.cpp
    instances/launch.cpp
    minecraft/http.cpp
    minecraft/installer.cpp
    minecraft/libraries.cpp
    minecraft/version_catalog.cpp
    ui/install_progress.cpp
)
target_link_libraries(svinlauncher PRIVATE PkgConfig::GTK4 PkgConfig::ADW ZLIB::ZLIB Intl::Intl)
target_compile_features(svinlauncher PRIVATE cxx_std_20)
target_compile_definitions(svinlauncher PRIVATE
    SVIN_APP_ID="${SVIN_APP_ID}"
    SVIN_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)
set(Launcher_AppID "${SVIN_APP_ID}")
set(Launcher_DisplayName "${SVIN_DISPLAY_NAME}")
configure_file(svinlauncher.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/svinlauncher.desktop" @ONLY)

install(TARGETS svinlauncher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/svinlauncher.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" RENAME "${SVIN_APP_ID}.desktop")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/${SVIN_APP_ID}.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps" RENAME "${SVIN_APP_ID}.png")

set(SVIN_SUPPORTED_LOCALES ru en es pt_BR fr de zh_CN ja ko pl)
set(SVIN_TRANSLATION_OUTPUTS)
foreach(locale IN LISTS SVIN_SUPPORTED_LOCALES)
    set(po_file "${CMAKE_CURRENT_SOURCE_DIR}/po/${locale}.po")
    set(mo_file "${CMAKE_CURRENT_BINARY_DIR}/locale/${locale}/LC_MESSAGES/svinlauncher.mo")
    add_custom_command(OUTPUT "${mo_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/locale/${locale}/LC_MESSAGES"
        COMMAND "${MSGFMT_EXECUTABLE}" -o "${mo_file}" "${po_file}"
        DEPENDS "${po_file}"
        VERBATIM)
    list(APPEND SVIN_TRANSLATION_OUTPUTS "${mo_file}")
    install(FILES "${mo_file}" DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${locale}/LC_MESSAGES")
endforeach()
add_custom_target(svinlauncher-translations ALL DEPENDS ${SVIN_TRANSLATION_OUTPUTS})
