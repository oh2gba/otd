cmake_minimum_required(VERSION 3.21)
project(otd VERSION 1.6.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OTD_BUILD_TESTS "Build unit tests" ON)
# Desktop entry / icon name. Flatpak builds pass the reverse-DNS id.
set(OTD_DESKTOP_ID "otd" CACHE STRING "Name of the installed .desktop file and icon")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql WebSockets Multimedia)

# ---- bundled third-party: miniz (MIT) for zip extraction -----------------
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- core library: no GUI, unit-testable -------------------------------
add_library(otd_core STATIC
  src/core/Station.h
  src/core/EibiParser.h      src/core/EibiParser.cpp
  src/core/HfccParser.h      src/core/HfccParser.cpp
  src/core/AokiParser.h      src/core/AokiParser.cpp
  src/core/ZipReader.h       src/core/ZipReader.cpp
  src/core/Schedule.h        src/core/Schedule.cpp
  src/core/StationDb.h       src/core/StationDb.cpp
  src/core/AppSettings.h  src/core/AppSettings.cpp
  src/core/Session.h      src/core/Session.cpp
  src/core/BandLine.h     src/core/BandLine.cpp
  src/core/DialMarks.h    src/core/DialMarks.cpp
  src/core/Format.h       src/core/Format.cpp
  src/core/RigClient.h       src/core/RigClient.cpp
  src/core/RigctldLauncher.h src/core/RigctldLauncher.cpp
  src/core/ScheduleSource.h  src/core/ScheduleSource.cpp
  src/core/EibiSource.h      src/core/EibiSource.cpp
  src/core/HfccSource.h      src/core/HfccSource.cpp
  src/core/AokiSource.h      src/core/AokiSource.cpp
  src/core/Updater.h         src/core/Updater.cpp
  src/core/SigidWiki.h       src/core/SigidWiki.cpp
  src/core/UserList.h        src/core/UserList.cpp
  src/core/BandPlan.h        src/core/BandPlan.cpp
  src/core/UpdateCheck.h     src/core/UpdateCheck.cpp
  src/core/KiwiClient.h      src/core/KiwiClient.cpp
  src/core/KiwiDirectory.h   src/core/KiwiDirectory.cpp
  src/core/TraficomSource.h  src/core/TraficomSource.cpp
  src/core/Ciraf.h           src/core/Ciraf.cpp
  src/core/PcmResampler.h    src/core/PcmResampler.cpp
  src/core/PcmQueue.h        src/core/PcmQueue.cpp
  src/core/Logging.h         src/core/Logging.cpp
  src/core/core.qrc
)
target_include_directories(otd_core PUBLIC src)
target_link_libraries(otd_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql Qt6::WebSockets PRIVATE miniz)
target_compile_definitions(otd_core PUBLIC
  OTD_VERSION="${PROJECT_VERSION}"
  QT_NO_CAST_FROM_ASCII_DISABLED)

# ---- application --------------------------------------------------------
# Everything but main() is compiled once, so that the tests can drive the
# real main window. An object library rather than a static one: a linker
# drops the compiled-in resources (the icons) from a static library, as
# nothing refers to them by name.
add_library(otd_app OBJECT
  src/app/MainWindow.h       src/app/MainWindow.cpp
  src/app/StationModel.h     src/app/StationModel.cpp
  src/app/DialScale.h        src/app/DialScale.cpp
  src/app/KiwiPlayer.h       src/app/KiwiPlayer.cpp
  src/app/KiwiAudio.h        src/app/KiwiAudio.cpp
  src/app/BufferBar.h        src/app/BufferBar.cpp
  src/app/SettingsDialog.h   src/app/SettingsDialog.cpp
  src/app/StationEditDialog.h src/app/StationEditDialog.cpp
  src/app/MyStationsDialog.h src/app/MyStationsDialog.cpp
  src/app/resources.qrc
)
target_link_libraries(otd_app PUBLIC otd_core Qt6::Widgets Qt6::Gui Qt6::Multimedia)

add_executable(otd WIN32 MACOSX_BUNDLE src/app/main.cpp)
target_link_libraries(otd PRIVATE otd_app)
target_compile_definitions(otd PRIVATE OTD_DESKTOP_ID="${OTD_DESKTOP_ID}")
if(WIN32)
  # icon and version block for otd.exe
  configure_file(data/otd.rc.in ${CMAKE_BINARY_DIR}/otd.rc @ONLY)
  target_sources(otd PRIVATE ${CMAKE_BINARY_DIR}/otd.rc)
endif()
set_target_properties(otd PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "On The Dial"
  MACOSX_BUNDLE_GUI_IDENTIFIER "eu.oh2gba.otd"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "GPL-3.0-or-later")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(otd_core PRIVATE -Wall -Wextra)
  target_compile_options(otd_app PRIVATE -Wall -Wextra)
  target_compile_options(otd PRIVATE -Wall -Wextra)
endif()

# ---- install ------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS otd
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
  configure_file(data/otd.desktop.in ${CMAKE_BINARY_DIR}/${OTD_DESKTOP_ID}.desktop @ONLY)
  configure_file(data/eu.oh2gba.otd.metainfo.xml.in
    ${CMAKE_BINARY_DIR}/eu.oh2gba.otd.metainfo.xml @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/${OTD_DESKTOP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES data/otd.svg RENAME ${OTD_DESKTOP_ID}.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  foreach(size 64 128 256)
    install(FILES data/otd-${size}.png RENAME ${OTD_DESKTOP_ID}.png
      DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
  endforeach()
  install(FILES ${CMAKE_BINARY_DIR}/eu.oh2gba.otd.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

# ---- tests --------------------------------------------------------------
if(OTD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
