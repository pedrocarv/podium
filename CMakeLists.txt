cmake_minimum_required(VERSION 3.25)
project(podium VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Concurrent Multimedia Test)
find_package(PkgConfig REQUIRED)
find_package(pugixml REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip>=1.10)

add_compile_options(-Wall -Wextra -Wpedantic)

file(GLOB_RECURSE TEMPLATE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS resources/template/*)

add_library(podium_core STATIC
    src/document.cpp
    src/geometry.cpp
    src/model.cpp
    src/ops.cpp
    src/package.cpp
    src/pptx_read.cpp
    src/pptx_write.cpp
    src/render.cpp
    src/richtext.cpp
)
target_include_directories(podium_core PUBLIC src)
target_link_libraries(podium_core PUBLIC Qt6::Widgets Qt6::Svg Qt6::Concurrent PkgConfig::LIBZIP pugixml::pugixml)
qt_add_resources(podium_core template PREFIX / BASE resources FILES ${TEMPLATE_FILES})

add_library(podium_ui STATIC
    src/canvas.cpp
    src/inspector.cpp
    src/mainwindow.cpp
    src/show.cpp
    src/slidepanel.cpp
)
target_link_libraries(podium_ui PUBLIC podium_core Qt6::Multimedia)
qt_add_resources(podium_ui icon PREFIX / BASE resources FILES resources/podium.svg)

qt_add_executable(podium src/main.cpp)
target_link_libraries(podium PRIVATE podium_ui)
target_compile_definitions(podium PRIVATE PODIUM_VERSION="${PROJECT_VERSION}")

install(TARGETS podium RUNTIME DESTINATION bin)
install(FILES resources/podium.desktop DESTINATION share/applications)
install(FILES resources/podium.svg DESTINATION share/icons/hicolor/scalable/apps)

enable_testing()
foreach(test pptx ui)
    qt_add_executable(tst_${test} tests/tst_${test}.cpp)
    target_link_libraries(tst_${test} PRIVATE podium_ui Qt6::Test)
    add_test(NAME ${test} COMMAND tst_${test})
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
add_test(NAME ui-two-screens COMMAND tst_ui dualScreenShow)
set_tests_properties(ui-two-screens PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_CURRENT_SOURCE_DIR}/tests/screens.json")
