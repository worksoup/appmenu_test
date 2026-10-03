cmake_minimum_required(VERSION 3.16)

project(appmenu_test
        DESCRIPTION "Minimal visual reproducer for the Krita 6 / Qt 6 global menu bug"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# appmenu_test.cpp contains no Q_OBJECT, so no moc is required. Keeping AUTOMOC
# off also allows the Qt5 and Qt6 variants to be built from one build tree.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

option(APPMENU_TEST_QT6 "Build the Qt 6 variant" ON)
option(APPMENU_TEST_QT5 "Build the Qt 5 variant" ON)

set(APPMENU_TEST_TARGETS "")

if (APPMENU_TEST_QT6)
    find_package(Qt6 QUIET COMPONENTS Widgets DBus)
    if (Qt6_FOUND)
        add_executable(appmenu_test_qt6 appmenu_test.cpp)
        set_target_properties(appmenu_test_qt6 PROPERTIES OUTPUT_NAME appmenu_test-qt6)
        target_link_libraries(appmenu_test_qt6 PRIVATE Qt6::Widgets Qt6::DBus)
        list(APPEND APPMENU_TEST_TARGETS appmenu_test_qt6)
        message(STATUS "Qt 6 ${Qt6_VERSION}: target appmenu_test-qt6")
    else ()
        message(STATUS "Qt 6 not found - skipping appmenu_test-qt6")
    endif ()
endif ()

if (APPMENU_TEST_QT5)
    find_package(Qt5 QUIET COMPONENTS Widgets DBus)
    if (Qt5_FOUND)
        add_executable(appmenu_test_qt5 appmenu_test.cpp)
        set_target_properties(appmenu_test_qt5 PROPERTIES OUTPUT_NAME appmenu_test-qt5)
        target_link_libraries(appmenu_test_qt5 PRIVATE Qt5::Widgets Qt5::DBus)
        list(APPEND APPMENU_TEST_TARGETS appmenu_test_qt5)
        message(STATUS "Qt 5 ${Qt5_VERSION}: target appmenu_test-qt5")
    else ()
        message(STATUS "Qt 5 not found - skipping appmenu_test-qt5")
    endif ()
endif ()

if (NOT APPMENU_TEST_TARGETS)
    message(FATAL_ERROR
            "Neither Qt 6 nor Qt 5 with Widgets was found. Install qt6-base / qt5-base "
            "(including their CMake package files).")
endif ()

# Smoke tests: --help prints usage and exits before any QApplication is created,
# so these do not open a window and are safe to run from CTest.
enable_testing()
foreach (_tgt IN LISTS APPMENU_TEST_TARGETS)
    add_test(NAME ${_tgt}-help COMMAND ${_tgt} --help)
endforeach ()
