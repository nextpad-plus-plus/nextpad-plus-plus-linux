#
# Nextpad++ Linux port — GTK4 + Scintilla (GTK4 backend) + Lexilla
#
# Migrated from the GTK3 port. Scintilla is the vendored GTK4 backend
# (bugaevc/scintilla, gtk4 branch) under scintilla/; Lexilla is unchanged
# (toolkit-agnostic). See docs/00_EXECUTIVE_SUMMARY.md and the M-phase plan.
#
cmake_minimum_required(VERSION 3.20)
project(NextpadPlusPlusLinux C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Default build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
    message(STATUS "No build type specified — defaulting to Release.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(ADW  REQUIRED libadwaita-1)

set(SCI_ROOT "${CMAKE_SOURCE_DIR}/scintilla")
set(LEX_ROOT "${CMAKE_SOURCE_DIR}/lexilla")
set(SRC      "${CMAKE_SOURCE_DIR}/src")
set(RES      "${CMAKE_SOURCE_DIR}/resources")

# -----------------------------------------------------------------------------
# Scintilla — GTK4 backend.
#
# The GTK4 port ships its own makefile under scintilla/gtk4/ which also drives
# the GObject enum/marshaller codegen. Rather than re-implement that codegen in
# CMake, we invoke the makefile and link the resulting shared library.
# -----------------------------------------------------------------------------
set(SCI_GTK4 "${SCI_ROOT}/gtk4")
set(SCI_SO   "${SCI_ROOT}/bin/libscintilla.so")

add_custom_command(
    OUTPUT  "${SCI_SO}"
    COMMAND make shared
    WORKING_DIRECTORY "${SCI_GTK4}"
    COMMENT "Building GTK4 Scintilla backend (bugaevc/scintilla)"
    VERBATIM)
add_custom_target(scintilla_build DEPENDS "${SCI_SO}")

# -----------------------------------------------------------------------------
# Lexilla — platform-agnostic syntax-highlighting library (unchanged from GTK3).
# -----------------------------------------------------------------------------
file(GLOB LEX_SRCS
    "${LEX_ROOT}/src/*.cxx"
    "${LEX_ROOT}/lexers/*.cxx"
    "${LEX_ROOT}/lexlib/*.cxx"
)
# Lexilla's vendored LexUser.cxx pulls in <windows.h>; swap in our cross-platform
# variant (windows.h removed, _itoa → sprintf) so clones build standalone.
list(FILTER LEX_SRCS EXCLUDE REGEX "LexUser\\.cxx$")
list(APPEND LEX_SRCS "${CMAKE_SOURCE_DIR}/vendor/LexUser.cxx")

add_library(lexilla STATIC ${LEX_SRCS})
target_include_directories(lexilla PUBLIC
    "${LEX_ROOT}/include" "${LEX_ROOT}/src" "${LEX_ROOT}/lexlib" "${LEX_ROOT}/access"
    "${SCI_ROOT}/include" "${SCI_ROOT}/src"
)
target_compile_definitions(lexilla PUBLIC SCI_LEXER)
# GCC 14 no longer pulls intptr_t in via <stddef.h>; Sci_Position.h needs it.
target_compile_options(lexilla PRIVATE "-include" "cstdint")

# LexSearchResult.cxx uses a Notepad++ Scintilla.h patch (SearchResultMarkings)
# absent from the stock vendored Scintilla — force-include the vendored shim.
set_source_files_properties("${LEX_ROOT}/lexers/LexSearchResult.cxx" PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/vendor/SearchResultMarkings.h")

# -----------------------------------------------------------------------------
# Application — nextpad-plus-plus
# -----------------------------------------------------------------------------
set(APP_SRCS
    # Glue
    "${SRC}/main.c"
    "${SRC}/lexilla_bridge.cpp"
    "${SRC}/recent.c"
    "${SRC}/stubs.c"
    "${SRC}/paths.c"
    # GTK4 migration helpers
    "${SRC}/npp_menu.c"
    # Editor core (G3.1)
    "${SRC}/editor.c"
    "${SRC}/encoding.c"
    "${SRC}/i18n.c"
    # G2 — themes + lexers
    "${SRC}/stylestore.c"
    "${SRC}/lexer.c"
    "${SRC}/udl.c"
    "${SRC}/udl_editor.c"
    "${SRC}/find_window.c"
    # Dark / Light / Auto appearance manager
    "${SRC}/theme.c"
    # G3 — file features
    "${SRC}/session.c"
    "${SRC}/backup.c"
    "${SRC}/doclist.c"
    # G4 — toolbar
    "${SRC}/toolbar.c"
    # G5 — find / replace
    "${SRC}/findreplace.c"
    "${SRC}/findinfiles.c"
    # G6 — autocomplete
    "${SRC}/autocomplete.c"
    "${SRC}/acapi.c"
    "${SRC}/udladmin.c"
    # G7 — status bar, git gutter, change history, macro, spell
    "${SRC}/statusbar.c"
    "${SRC}/gitgutter.c"
    "${SRC}/changehistory.c"
    "${SRC}/macro.c"
    "${SRC}/macrobatch.c"
    "${SRC}/spell.c"
    # G8 — side panels
    "${SRC}/docmap.c"
    "${SRC}/funclist.c"
    "${SRC}/workspace.c"
    "${SRC}/searchresults.c"
    "${SRC}/charpanel.c"
    "${SRC}/cliphistory.c"
    # G9 — preferences, shortcut mapper, style editor, misc dialogs
    "${SRC}/prefs.c"
    "${SRC}/shortcutmap.c"
    "${SRC}/styleeditor.c"
    "${SRC}/columneditor.c"
    "${SRC}/run.c"
    "${SRC}/project.c"
    # G10 — plugin system
    "${SRC}/plugin.c"
    "${SRC}/pluginsadmin.c"
    # G18 — full Git panel
    "${SRC}/gitpanel.c"
    # G21 — floating side panels
    "${SRC}/floating.c"
    # G34 — uniform side-panel chrome wrapper
    "${SRC}/panel_frame.c"
    # G29 — Markdown preview panel
    "${SRC}/mdpreview.c"
    # G14 — Split view
    "${SRC}/split.c"
    # P4 — langs.xml parser
    "${SRC}/langsmgr.c"
    # P5 — XML-driven context menus
    "${SRC}/ctxmenu.c"
    # P6 — toolbarButtonsConf.xml parser
    "${SRC}/toolbarconf.c"
)

add_executable(nextpad_pp ${APP_SRCS})
set_target_properties(nextpad_pp PROPERTIES OUTPUT_NAME "Nextpad++")
add_dependencies(nextpad_pp scintilla_build)

target_include_directories(nextpad_pp PRIVATE
    "${SRC}"
    "${SCI_ROOT}/include" "${SCI_ROOT}/gtk4"
    "${LEX_ROOT}/include"
    ${GTK4_INCLUDE_DIRS} ${ADW_INCLUDE_DIRS}
)
# RESOURCES_DIR is baked in at compile time. For a dev build it points at
# the in-tree resources/. For a packaged build, pass -DNPP_RES_DIR=<path>
# (e.g. /usr/share/nextpad-plus-plus) so the installed binary finds the
# resources copied there by the install rules below.
if(NOT DEFINED NPP_RES_DIR)
    set(NPP_RES_DIR "${RES}")
endif()
target_compile_definitions(nextpad_pp PRIVATE
    GTK
    RESOURCES_DIR="${NPP_RES_DIR}"
)
target_compile_options(nextpad_pp PRIVATE ${GTK4_CFLAGS_OTHER} ${ADW_CFLAGS_OTHER})
set_source_files_properties("${SRC}/lexilla_bridge.cpp" PROPERTIES
    COMPILE_OPTIONS "-include;cstdint")

# libscintilla.so lives next to the build tree; embed an rpath so the binary
# finds it without an install step.
set_target_properties(nextpad_pp PROPERTIES BUILD_RPATH "${SCI_ROOT}/bin")

target_link_libraries(nextpad_pp PRIVATE
    "${SCI_SO}"
    lexilla
    ${GTK4_LIBRARIES} ${ADW_LIBRARIES}
    m
)
target_link_directories(nextpad_pp PRIVATE ${GTK4_LIBRARY_DIRS} ${ADW_LIBRARY_DIRS})

# -----------------------------------------------------------------------------
# Install / packaging targets
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS nextpad_pp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${SCI_SO}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Install the contents of resources/ directly under .../nextpad-plus-plus
# so the tree matches RESOURCES_DIR (= NPP_RES_DIR) baked into the binary.
install(DIRECTORY resources/ DESTINATION ${CMAKE_INSTALL_DATADIR}/nextpad-plus-plus
        PATTERN ".git" EXCLUDE
        # The hicolor tree is installed separately under share/icons/hicolor
        # for the system icon theme; don't duplicate it here.
        PATTERN "icons/hicolor" EXCLUDE)

install(FILES packaging/nextpad-plus-plus.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME org.nextpad.NextpadPP.desktop)
install(FILES packaging/org.nextpad.NextpadPP.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/nextpad-plus-plus.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Application icon — the freedesktop hicolor tree under
# resources/icons/hicolor/<size>/apps/nextpad-plus-plus.png is shipped as
# the system icon theme entry. The .desktop file references the bare
# name 'nextpad-plus-plus'; the dock / app picker resolves it through
# hicolor at the right size.
install(DIRECTORY resources/icons/hicolor/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor
        FILES_MATCHING PATTERN "nextpad-plus-plus.*")
