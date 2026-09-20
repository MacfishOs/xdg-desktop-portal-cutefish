cmake_minimum_required(VERSION 3.18)
project(xdg-desktop-portal-cutefish VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(Gbm REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(Libdrm REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(WaylandProtocols REQUIRED wayland-protocols>=1.39)
pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

# The permission store is shared with Settings through the sibling framework
# checkout, as in the other desktop components.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../cutefish-framework/privacy
                 ${CMAKE_CURRENT_BINARY_DIR}/cutefish-framework-privacy-build)

# Client-side marshalling for the capture protocols; generated into the build tree.
set(PROTOCOL_DIR ${CMAKE_BINARY_DIR}/wayland-protocols)
file(MAKE_DIRECTORY ${PROTOCOL_DIR})
function(portal_wayland_protocol target name directory)
    set(xml ${WAYLAND_PROTOCOLS_DATADIR}/${directory}/${name}.xml)
    add_custom_command(OUTPUT ${PROTOCOL_DIR}/${name}-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${PROTOCOL_DIR}/${name}-protocol.c
        DEPENDS ${xml} VERBATIM)
    add_custom_command(OUTPUT ${PROTOCOL_DIR}/${name}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${PROTOCOL_DIR}/${name}-client-protocol.h
        DEPENDS ${xml} VERBATIM)
    target_sources(${target} PRIVATE
        ${PROTOCOL_DIR}/${name}-protocol.c ${PROTOCOL_DIR}/${name}-client-protocol.h)
endfunction()

add_executable(xdg-desktop-portal-cutefish
    src/main.cpp
    src/inhibitportal.cpp
    src/inhibitportal.h
    src/settingsportal.cpp
    src/settingsportal.h
    src/filechooserportal.cpp
    src/filechooserportal.h
    src/filechoosertypes.h
    src/pipewirecore.cpp
    src/pipewirecore.h
    src/screencastportal.cpp
    src/screencastportal.h
    src/screencaststream.cpp
    src/screencaststream.h
    src/screencasttypes.h
    src/waylandcapture.cpp
    src/waylandcapture.h
)
portal_wayland_protocol(xdg-desktop-portal-cutefish ext-image-capture-source-v1 staging/ext-image-capture-source)
portal_wayland_protocol(xdg-desktop-portal-cutefish ext-image-copy-capture-v1 staging/ext-image-copy-capture)
portal_wayland_protocol(xdg-desktop-portal-cutefish ext-foreign-toplevel-list-v1 staging/ext-foreign-toplevel-list)
portal_wayland_protocol(xdg-desktop-portal-cutefish xdg-output-unstable-v1 unstable/xdg-output)
portal_wayland_protocol(xdg-desktop-portal-cutefish linux-dmabuf-v1 stable/linux-dmabuf)
target_include_directories(xdg-desktop-portal-cutefish PRIVATE ${PROTOCOL_DIR})
target_link_libraries(xdg-desktop-portal-cutefish PRIVATE Qt6::Core Qt6::DBus
    PkgConfig::PipeWire PkgConfig::WaylandClient PkgConfig::Gbm PkgConfig::Libdrm
    Cutefish::Privacy)
target_compile_definitions(xdg-desktop-portal-cutefish PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
    FILE_CHOOSER_HELPER="${CMAKE_INSTALL_FULL_LIBEXECDIR}/cutefish-filechooser"
)

configure_file(data/org.freedesktop.impl.portal.desktop.cutefish.service.in
    org.freedesktop.impl.portal.desktop.cutefish.service @ONLY)
configure_file(data/xdg-desktop-portal-cutefish.service.in
    xdg-desktop-portal-cutefish.service @ONLY)

install(TARGETS xdg-desktop-portal-cutefish RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES data/cutefish.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES data/cutefish-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.cutefish.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
set(SYSTEMD_USER_UNIT_DIR "lib/systemd/user" CACHE STRING
    "Installation directory for systemd user units")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xdg-desktop-portal-cutefish.service
    DESTINATION ${SYSTEMD_USER_UNIT_DIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "from gi.repository import Gio, GLib"
        RESULT_VARIABLE PYTHON_GIO_RESULT ERROR_QUIET)
    if(NOT PYTHON_GIO_RESULT EQUAL 0)
        message(FATAL_ERROR "Tests require Python GObject/GIO bindings (python3-gi on Debian). Use -DPython3_EXECUTABLE=/usr/bin/python3 or -DBUILD_TESTING=OFF.")
    endif()
    foreach(test settings inhibit screencast)
        add_test(NAME ${test}-portal
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_session.py
                ${DBUS_RUN_SESSION} ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${test}.py
                $<TARGET_FILE:xdg-desktop-portal-cutefish>)
        set_tests_properties(${test}-portal PROPERTIES TIMEOUT 60)
    endforeach()
endif()
