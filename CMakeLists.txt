cmake_minimum_required(VERSION 3.16)

project(Transnalysis VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Charts Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Charts Test)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        plotwindow.cpp
        plotwindow.h
        plotwindow.ui
        transportmath.h
        transportmath.cpp
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Transnalysis
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}

        plotwindow.h plotwindow.cpp plotwindow.ui
        numerictablemodel.h numerictablemodel.cpp
        saveexportdialog.h saveexportdialog.cpp saveexportdialog.ui

    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET Transnalysis APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(Transnalysis SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(Transnalysis
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(Transnalysis PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Charts
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.Transnalysis)
endif()
set_target_properties(Transnalysis PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS Transnalysis
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Transnalysis)
endif()

# --- Unit tests -------------------------------------------------------
# TransportMath has no QWidget/model dependency, so it can be exercised
# directly with Qt Test without constructing the GUI.
enable_testing()

add_executable(transnalysis_tests
    transportmath.h
    transportmath.cpp
    tests/test_transportmath.cpp
)

target_include_directories(transnalysis_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(transnalysis_tests PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Test
)

add_test(NAME TransportMathTests COMMAND transnalysis_tests)
