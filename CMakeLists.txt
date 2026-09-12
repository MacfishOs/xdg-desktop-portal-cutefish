cmake_minimum_required(VERSION 3.18)
project(xdg-desktop-portal-cutefish VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_executable(xdg-desktop-portal-cutefish
    src/main.cpp
    src/settingsportal.cpp
    src/settingsportal.h
)
target_link_libraries(xdg-desktop-portal-cutefish PRIVATE Qt6::Core Qt6::DBus)
target_compile_definitions(xdg-desktop-portal-cutefish PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
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
    add_test(NAME settings-portal
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_session.py
            ${DBUS_RUN_SESSION} ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_settings.py
            $<TARGET_FILE:xdg-desktop-portal-cutefish>)
    set_tests_properties(settings-portal PROPERTIES TIMEOUT 60)
endif()
