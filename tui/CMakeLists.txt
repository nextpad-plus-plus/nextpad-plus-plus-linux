# Nextpad++ terminal editor (`npp`) — Linux port of the macOS tui/ tree.
# Reached only through the root's -DNPP_BUILD_TUI=ON (option OFF by default,
# so a plain configure produces exactly the GUI's targets). Plan and file
# provenance: docs/12_macos_parity_delta_2026-09-03.md §8; the macOS
# original is notepad-plus-plus-macos/tui/CMakeLists.txt.
#
# Phase 1 (this file): scintilla_curses, the verbatim chrome/input/scene/term
# libraries, the shared-source npp_core through the backend seam, the unit
# tests under ctest, and a Scintilla+Lexilla+ncursesw link proof.
# Phase 2 adds main.cpp + shim/*.cpp and turns on the nextpad-tui block below.

# GLib/GIO (npp_core, fuzzy) and uchardet (encoding.c). ncursesw was found by
# the root (NCURSESW_*: Debian needs -ltinfo, pkg-config supplies it).
pkg_check_modules(TUI_GLIB REQUIRED glib-2.0 gio-2.0)

set(TUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# T0.3 diagnostic (`make curses-probe`): verifies the system curses is a
# wide-character build with usable colour depth (docs/tui/phase0-results.md).
# -----------------------------------------------------------------------------
add_executable(curses-probe tools/curses-probe.c)
target_compile_definitions(curses-probe PRIVATE _XOPEN_SOURCE_EXTENDED=1)
target_include_directories(curses-probe PRIVATE ${NCURSESW_INCLUDE_DIRS})
target_link_libraries(curses-probe PRIVATE ${NCURSESW_LIBRARIES})

# -----------------------------------------------------------------------------
# T1.1 — Scintilla with the Scinterm (curses) platform layer.
#
# The same platform-agnostic core (scintilla/src) the GUI's libscintilla.so
# is built from — including scintilla-patches/ and the patch-0005 SCI_OWNREGEX
# backends from regex/ — recompiled with -DCURSES into a separate static
# library. Independent of the scintilla_build makefile target; the GUI
# library and the app are untouched. Build proof: docs/12 §8.2.
# -----------------------------------------------------------------------------
file(GLOB SCI_CORE_SRCS "${SCI_ROOT}/src/*.cxx")

# Exactly the four regex/ sources scintilla/gtk4/makefile compiles into
# libscintilla.so (RegexBackendSelect picks NppRegexSearch by default,
# BoostRegexSearch behind gNppUseBoostRegex).
set(NPP_REGEX_ROOT "${CMAKE_SOURCE_DIR}/regex")
set(NPP_REGEX_SRCS
    "${NPP_REGEX_ROOT}/NppRegexSearch.cxx"
    "${NPP_REGEX_ROOT}/BoostRegExSearch.cxx"
    "${NPP_REGEX_ROOT}/UTF8DocumentIterator.cxx"
    "${NPP_REGEX_ROOT}/RegexBackendSelect.cxx"
)

add_library(scintilla_curses STATIC
    ${SCI_CORE_SRCS}
    ${NPP_REGEX_SRCS}
    scinterm/PlatCurses.cxx
    scinterm/ScintillaCurses.cxx
    scinterm/wcwidth.c
)
target_include_directories(scintilla_curses PUBLIC
    "${SCI_ROOT}/include"
    "${SCI_ROOT}/src"
    "${TUI_DIR}/scinterm"
    ${NCURSESW_INCLUDE_DIRS}
)
target_include_directories(scintilla_curses PRIVATE "${NPP_REGEX_ROOT}")
target_compile_definitions(scintilla_curses PRIVATE
    CURSES
    SCI_LEXER
    SCI_OWNREGEX
    BOOST_REGEX_STANDALONE
)
# GCC 14 no longer pulls intptr_t in via <stddef.h>; Sci_Position.h needs it
# (same treatment as the lexilla target in the root).
target_compile_options(scintilla_curses PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-include>" "$<$<COMPILE_LANGUAGE:CXX>:cstdint>")
# Wide-char curses declarations are needed only by the scinterm sources; the
# define is scoped to them because _XOPEN_SOURCE macros narrow the libc
# namespace and must not leak into the Scintilla core compilation.
set_source_files_properties(
    scinterm/PlatCurses.cxx scinterm/ScintillaCurses.cxx scinterm/wcwidth.c
    PROPERTIES COMPILE_DEFINITIONS _XOPEN_SOURCE_EXTENDED=1
)
target_link_libraries(scintilla_curses PUBLIC ${NCURSESW_LIBRARIES})

# -----------------------------------------------------------------------------
# fuzzy/ — the command palette's matcher (same npp::fuzzy interface as macOS;
# FuzzyText.cxx uses GLib for its UTF-8 folding where the .mm used CF).
# -----------------------------------------------------------------------------
add_library(npp_fuzzy STATIC
    "${CMAKE_SOURCE_DIR}/fuzzy/FuzzyMatcher.cxx"
    "${CMAKE_SOURCE_DIR}/fuzzy/FuzzyText.cxx"
)
target_include_directories(npp_fuzzy PUBLIC
    "${CMAKE_SOURCE_DIR}/fuzzy"
    "${CMAKE_SOURCE_DIR}/fuzzy/rapidfuzz"
    ${TUI_GLIB_INCLUDE_DIRS}
)
target_link_libraries(npp_fuzzy PUBLIC ${TUI_GLIB_LIBRARIES})

# -----------------------------------------------------------------------------
# The verbatim pure-C++ layer: Fresh-derived ports (chrome/, input/, term/ —
# see NOTICE and the per-file headers) plus our scene/ projections and the
# screen model. No curses, no GLib: the renderers write cell surfaces.
# -----------------------------------------------------------------------------
add_library(npp_tui_chrome STATIC
    chrome/dimming.cpp
    chrome/menubar.cpp
    chrome/palette.cpp
    chrome/panel.cpp
    chrome/popup.cpp
    chrome/scrollbar.cpp
    chrome/statusbar.cpp
    chrome/tabbar.cpp
    input/vtparser.cpp
    scene/scene.cpp
    term/colorcaps.cpp
    term/modes.cpp
    term/screen.cpp
)
target_include_directories(npp_tui_chrome PUBLIC "${TUI_DIR}")
target_link_libraries(npp_tui_chrome PUBLIC npp_fuzzy)   # palette.cpp

# -----------------------------------------------------------------------------
# npp_core — shared src/ modules compiled headless through the backend seam
# (-DNPP_TUI: sci_backend.h SCI_BACKEND_CURSES + gtk_compat.h GtkWidget=void).
# Gives the Linux TUI what the macOS one lacks: UDLs, Global-override
# handling, per-language tab overrides (docs/12 §8.3). shim/prefs_stub.c
# supplies the g_prefs data prefs.c cannot be linked for.
# -----------------------------------------------------------------------------
add_library(npp_core STATIC
    # tier 1 — reported headless-linkable
    "${SRC}/langsmgr.c"
    "${SRC}/encoding.c"
    "${SRC}/paths.c"
    "${SRC}/funclist_xml.c"
    "${SRC}/langcaptions.c"
    "${SRC}/lexilla_bridge.cpp"
    # tier 2 — through the seam
    "${SRC}/stylestore.c"
    "${SRC}/udl.c"
    "${SRC}/lexer.c"
    shim/prefs_stub.c
)
target_include_directories(npp_core PUBLIC
    "${SRC}"
    "${SCI_ROOT}/include"
    "${LEX_ROOT}/include"
    "${NPP_REGEX_ROOT}"
    ${TUI_GLIB_INCLUDE_DIRS} ${UCHARDET_INCLUDE_DIRS}
)
target_compile_definitions(npp_core PUBLIC
    NPP_TUI
    RESOURCES_DIR="${NPP_RES_DIR}"
)
set_source_files_properties("${SRC}/lexilla_bridge.cpp" PROPERTIES
    COMPILE_OPTIONS "-include;cstdint")
target_link_libraries(npp_core PUBLIC
    lexilla
    ${TUI_GLIB_LIBRARIES} ${UCHARDET_LIBRARIES}
)

# -----------------------------------------------------------------------------
# npp_tui_shim — PHASE 2a: the five Linux shim bodies behind shim/*.h
# (docs/12 §8.3): theme (stylestore.c getters, Option B), session
# (tui-session.xml via GMarkup), workspace (GMarkup + GDir), interop
# (config.xml SAX + writer + FileWatch), funclist (flxml_scan adapter +
# the hardcoded-regex fallback). Links npp_core; stylestore.o's Scintilla
# apply paths resolve against scintilla_curses. No curses calls of its own.
# -----------------------------------------------------------------------------
add_library(npp_tui_shim STATIC
    shim/funclist.cpp      # adapter over src/funclist_xml.c flxml_scan (+ fallback)
    shim/interop.cpp       # config.xml SAX + SetGuiConfigAttr + FileWatch (st_mtim)
    shim/session.cpp       # tui-session.xml via GMarkup, same keys as the plist
    shim/workspace.cpp     # GMarkup .workspace + GDir listing
    shim/theme.cpp         # stylestore.c getters, theme.mm palette math verbatim
)
target_include_directories(npp_tui_shim PUBLIC "${TUI_DIR}")
target_link_libraries(npp_tui_shim PUBLIC npp_core scintilla_curses)

# nextpad-tui (`npp`) — phase 2. main.cpp is the transcription of the macOS
# tui/main.mm against the Linux replacements of docs/12 §8.3; the five
# shim/*.cpp bodies (phase 2a) implement shim/*.h.
# -----------------------------------------------------------------------------
add_executable(nextpad-tui main.cpp)
set_target_properties(nextpad-tui PROPERTIES OUTPUT_NAME npp)
# Wide-char curses (cchar_t / mvwin_wch for the dim-behind pass), same as the
# scinterm sources; GCC 14 needs <cstdint> ahead of Sci_Position.h.
target_compile_definitions(nextpad-tui PRIVATE _XOPEN_SOURCE_EXTENDED=1
    NPP_TUI_VERSION="0.9.0")   # the TUI's own version, pre-1.0 (GAP-189 —
                               # host version comes from branding.h APP_VERSION)
target_compile_options(nextpad-tui PRIVATE "-include" "cstdint" ${NCURSESW_CFLAGS_OTHER})
target_link_libraries(nextpad-tui PRIVATE
    npp_tui_shim        # shim/ funclist interop session workspace theme
    npp_tui_chrome      # chrome/ input/ scene/ term/ (+ npp_fuzzy)
    npp_core            # langsmgr encoding paths funclist_xml stylestore udl lexer
    scintilla_curses    # + ncursesw/tinfo
    lexilla
    ${TUI_GLIB_LIBRARIES} ${UCHARDET_LIBRARIES}
)
target_link_directories(nextpad-tui PRIVATE ${TUI_GLIB_LIBRARY_DIRS} ${UCHARDET_LIBRARY_DIRS})
install(TARGETS nextpad-tui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# (packaging/npp.1 is installed from the root CMakeLists NPP_BUILD_TUI block.)

# -----------------------------------------------------------------------------
# Unit tests — one executable per tests/*_test.cpp, exit 0 = pass. Same
# suites as macOS's portable set (theme/session/interop/funclist/workspace
# tests are phase 2 with the shim bodies).
# -----------------------------------------------------------------------------
foreach(_t colorcaps modes palette panel popup scene screen vtparser)
    add_executable(${_t}-test tests/${_t}_test.cpp)
    target_link_libraries(${_t}-test PRIVATE npp_tui_chrome)
    add_test(NAME tui.${_t} COMMAND ${_t}-test)
endforeach()

# Phase 2a — the shim suites (ports of the macOS theme/session/workspace/
# interop/funclist tests, assertions preserved) over npp_tui_shim. The
# theme and funclist suites also sweep this repo's resources/ (RESOURCES_DIR).
foreach(_t theme session workspace interop funclist)
    add_executable(${_t}-test tests/${_t}_test.cpp)
    target_link_libraries(${_t}-test PRIVATE npp_tui_shim)
    add_test(NAME tui.${_t} COMMAND ${_t}-test)
endforeach()

# The Linux shim header contract must stay compilable on its own (phase 2
# transliterates main.mm's call sites against it).
add_executable(shim_headers-test tests/shim_headers_test.cpp)
target_include_directories(shim_headers-test PRIVATE "${TUI_DIR}")
add_test(NAME tui.shim_headers COMMAND shim_headers-test)

# Link proof (docs/12 §8.2 reproduced under ctest): scintilla_curses +
# lexilla + ncursesw on a private pty — CreateLexer("cpp") styles a keyword,
# the SCI_OWNREGEX backend matches, CJK text inserts.
add_executable(tui_linktest tests/linktest.cxx)
target_compile_definitions(tui_linktest PRIVATE _XOPEN_SOURCE_EXTENDED=1)
target_compile_options(tui_linktest PRIVATE "-include" "cstdint")
target_link_libraries(tui_linktest PRIVATE scintilla_curses lexilla util)
add_test(NAME tui.linktest COMMAND tui_linktest)

# -----------------------------------------------------------------------------
# Phase 4.0 E2E harness (drives the real `npp` on a pty against the in-memory
# screen model). Each battery is a ctest entry fed the freshly built binary:
#   ctest --test-dir build-tui -R tui.e2e
# -----------------------------------------------------------------------------
add_library(npp_tui_e2e STATIC tests/e2e/driver.cpp)
target_include_directories(npp_tui_e2e PUBLIC "${TUI_DIR}")
target_link_libraries(npp_tui_e2e PUBLIC npp_tui_chrome util)
foreach(_e smoke edit features brace panel)
    add_executable(e2e-${_e} tests/e2e/e2e_${_e}.cpp)
    target_link_libraries(e2e-${_e} PRIVATE npp_tui_e2e)
    add_test(NAME tui.e2e.${_e} COMMAND e2e-${_e} $<TARGET_FILE:nextpad-tui>)
    set_tests_properties(tui.e2e.${_e} PROPERTIES
        ENVIRONMENT "NPP_E2E_BIN=$<TARGET_FILE:nextpad-tui>"
        TIMEOUT 600)
endforeach()
add_custom_target(tui-e2e DEPENDS nextpad-tui e2e-smoke e2e-edit e2e-features e2e-brace e2e-panel)
