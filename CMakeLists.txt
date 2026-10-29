cmake_minimum_required(VERSION 3.21)
project(zuuned VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MPRIS (src/player/MprisService.cpp) is the Linux D-Bus media-key bridge, so
# DBus is only a component where it exists: the Windows Qt build has no Qt6::DBus.
if(WIN32)
    set(ZUUNED_QT_COMPONENTS Quick Gui Concurrent Network OpenGL QuickControls2
        QuickDialogs2 Core5Compat)
else()
    set(ZUUNED_QT_COMPONENTS Quick Gui Concurrent Network OpenGL DBus
        QuickControls2 QuickDialogs2 Core5Compat)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS ${ZUUNED_QT_COMPONENTS})

# Translations are optional in the configure, never required: a Qt without
# LinguistTools still builds the app in English, and src/main.cpp logs that the
# catalogue is missing rather than pretending zh_CN is available.
find_package(Qt6 6.8 QUIET COMPONENTS LinguistTools)

if(WIN32)
    # ---- Windows dependency plumbing -------------------------------------------
    # pkg-config does not describe this tree, so every dependency is probed here
    # up front (libzune needs FFmpeg and LAME before the transcode section runs).
    # vcpkg carries libusb, SQLite, LAME and libgcrypt; ZUUNED_WIN_DEPS carries
    # what vcpkg does not: the FFmpeg shared build, the mpv dev zip, a PCRE2
    # build and the libmpv import library.  BUILDING.md documents the layout.
    set(ZUUNED_VCPKG_PREFIX "" CACHE PATH "Installed vcpkg tree used by the Windows build")
    set(ZUUNED_WIN_DEPS "" CACHE PATH "Windows dependency tree: FFmpeg shared build, mpv-dev, PCRE2, probe")
    set(ZUUNED_FFMPEG_DIR "" CACHE PATH "FFmpeg shared build: headers plus MSVC import libraries")

    if(NOT ZUUNED_VCPKG_PREFIX)
        message(FATAL_ERROR
            "The Windows build needs ZUUNED_VCPKG_PREFIX.  Run "
            "'vcpkg install libusb sqlite3 libmp3lame libgcrypt:x64-windows' and "
            "configure with -DZUUNED_VCPKG_PREFIX=<vcpkg>/installed/x64-windows.")
    endif()
    if(NOT IS_DIRECTORY "${ZUUNED_VCPKG_PREFIX}/lib")
        message(FATAL_ERROR
            "ZUUNED_VCPKG_PREFIX='${ZUUNED_VCPKG_PREFIX}' has no lib directory.  "
            "It must be an *installed* vcpkg tree carrying libusb, sqlite3, "
            "libmp3lame and libgcrypt, e.g. run "
            "'vcpkg install libusb sqlite3 libmp3lame libgcrypt:x64-windows' "
            "and point the variable at <vcpkg>/installed/x64-windows.")
    endif()
    if(NOT ZUUNED_WIN_DEPS)
        message(FATAL_ERROR
            "The Windows build needs ZUUNED_WIN_DEPS: one directory holding the "
            "FFmpeg win64 shared build, mpv-dev/, pcre2/ and probe/libmpv.lib.  "
            "See BUILDING.md.")
    endif()
    if(NOT ZUUNED_FFMPEG_DIR)
        set(ZUUNED_FFMPEG_DIR "${ZUUNED_WIN_DEPS}/ffmpeg-master-latest-win64-gpl-shared"
            CACHE PATH "FFmpeg shared build: headers plus MSVC import libraries" FORCE)
    endif()
    if(NOT IS_DIRECTORY "${ZUUNED_FFMPEG_DIR}/include")
        message(FATAL_ERROR
            "ZUUNED_FFMPEG_DIR='${ZUUNED_FFMPEG_DIR}' has no include directory.  "
            "Unpack an FFmpeg win64 gpl *shared* build inside ZUUNED_WIN_DEPS.")
    endif()

    find_library(LIBUSB_LIBRARIES NAMES libusb-1.0 usb-1.0
        HINTS "${ZUUNED_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
    find_library(SQLITE3_LIBRARIES NAMES sqlite3
        HINTS "${ZUUNED_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
    find_library(LAME_LIBRARIES NAMES libmp3lame mp3lame
        HINTS "${ZUUNED_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
    find_library(GCRYPT_LIBRARIES NAMES gcrypt
        HINTS "${ZUUNED_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
    find_library(GPG_ERROR_LIBRARIES NAMES gpg-error
        HINTS "${ZUUNED_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
    find_path(LAME_INCLUDE_DIR lame/lame.h
        HINTS "${ZUUNED_VCPKG_PREFIX}/include" REQUIRED NO_DEFAULT_PATH)
    find_path(GCRYPT_INCLUDE_DIR gcrypt.h
        HINTS "${ZUUNED_VCPKG_PREFIX}/include" REQUIRED NO_DEFAULT_PATH)
    find_path(PCRE2_INCLUDE_DIR pcre2.h
        HINTS "${ZUUNED_WIN_DEPS}/pcre2/include" REQUIRED NO_DEFAULT_PATH)
    find_library(PCRE2_POSIX_LIBRARY NAMES pcre2-posix-static pcre2-posix
        HINTS "${ZUUNED_WIN_DEPS}/pcre2/lib" REQUIRED NO_DEFAULT_PATH)
    find_library(PCRE2_8_LIBRARY NAMES pcre2-8-static pcre2-8
        HINTS "${ZUUNED_WIN_DEPS}/pcre2/lib" REQUIRED NO_DEFAULT_PATH)
    # libmpv ships as a DLL; probe/libmpv.lib is the MSVC import library built
    # from its definition file (BUILDING.md), which is what lets us link it.
    find_path(MPV_INCLUDE_DIR mpv/client.h
        HINTS "${ZUUNED_WIN_DEPS}/mpv-dev/include" REQUIRED NO_DEFAULT_PATH)
    find_library(MPV_LIBRARIES NAMES mpv libmpv
        HINTS "${ZUUNED_WIN_DEPS}/probe" REQUIRED NO_DEFAULT_PATH)

    foreach(component avformat avcodec avutil swresample swscale)
        find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
            HINTS "${ZUUNED_FFMPEG_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
        list(APPEND ZUUNED_FFMPEG_LIBRARIES "${FFMPEG_${component}_LIBRARY}")
    endforeach()

    # pkg-config publishes singular cache entries plus plural consumer
    # variables; mirror that split so the rest of this file stays neutral.
    set(FFMPEG_LIBRARIES ${ZUUNED_FFMPEG_LIBRARIES})
    set(FFMPEG_INCLUDE_DIRS "${ZUUNED_FFMPEG_DIR}/include")
    set(LAME_INCLUDE_DIRS ${LAME_INCLUDE_DIR})
    set(GCRYPT_INCLUDE_DIRS ${GCRYPT_INCLUDE_DIR})
    set(MPV_INCLUDE_DIRS ${MPV_INCLUDE_DIR})
    set(SQLITE3_INCLUDE_DIRS "${ZUUNED_VCPKG_PREFIX}/include")
    if(IS_DIRECTORY "${ZUUNED_VCPKG_PREFIX}/include/libusb-1.0")
        set(LIBUSB_INCLUDE_DIRS "${ZUUNED_VCPKG_PREFIX}/include/libusb-1.0")
    else()
        set(LIBUSB_INCLUDE_DIRS "${ZUUNED_VCPKG_PREFIX}/include")
    endif()

    # compat/win is the POSIX surface libzune and the transcode C layer expect.
    # /FI forces the umbrella header into every translation unit so no shared
    # source grows #ifdefs; /utf-8 keeps the UTF-8 literals legible to MSVC.
    # Both are needed for C and C++: moc output is compiled as C++ here.
    set(ZUUNED_COMPAT_DIR ${CMAKE_SOURCE_DIR}/compat/win)
    set(ZUUNED_SHIM_FLAGS /FIposix_shim.h)
    set(ZUUNED_WINCOMPAT zuuned_wincompat)
    set(ZUUNED_SYSTEM_LIBS ${GCRYPT_LIBRARIES} ${GPG_ERROR_LIBRARIES} ws2_32)
    set(ZUUNED_LIBM "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    set(ZUUNED_COMPAT_DIR "")
    set(ZUUNED_SHIM_FLAGS "")
    set(ZUUNED_WINCOMPAT "")
    set(ZUUNED_SYSTEM_LIBS gcrypt)
    set(ZUUNED_LIBM m)
    set(LAME_LIBRARIES mp3lame)
    set(LAME_INCLUDE_DIRS "")
    set(GCRYPT_INCLUDE_DIRS "")
    set(PCRE2_INCLUDE_DIR "")
endif()
qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0004 NEW)
include(packaging/CheckQmlImports.cmake)

if(WIN32)
    # compat/win is compiled once and its include/link interface reaches every
    # C target that needs the POSIX surface, libzune and the transcode layer
    # included.  PCRE2 backs libzune's pattern helpers and spoken-media matching.
    add_library(zuuned_wincompat STATIC compat/win/wincompat.c)
    target_include_directories(zuuned_wincompat PUBLIC
        ${ZUUNED_COMPAT_DIR} ${PCRE2_INCLUDE_DIR})
    target_link_libraries(zuuned_wincompat PUBLIC
        ${PCRE2_POSIX_LIBRARY} ${PCRE2_8_LIBRARY})
    target_compile_options(zuuned_wincompat PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:/utf-8>" ${ZUUNED_SHIM_FLAGS})
endif()

# A Windows process opts out of MAX_PATH in its own manifest, and nowhere else:
# the file APIs lift the 260-character ceiling only for an image that declares
# longPathAware, and only on a machine whose registry allows long paths at all.
# An app without the declaration has a music folder whose songs sit in Explorer
# and stay missing from the library - the kind of bug that reads as a corrupt file
# until somebody copies the same folder one level shallower.
#
# The manifest belongs to the linker, but CMake insists on supplying one for every
# Windows target, and two manifests cannot share resource ID 1: the link dies in
# cvtres with a duplicate MANIFEST resource, and the /MANIFEST:EMBED that carries
# ours makes CMake's own /MANIFESTFILE argument evaporate.  /MANIFEST:NO is the
# switch CMake reads before it generates anything, so the target takes the whole
# manifest - the asInvoker trustInfo included, which is why the file is not a
# fragment.  Every Windows executable takes it rather than only the app: the
# transcode CLI, the library gate and the app reach the same folders through the
# same C runtime, so one of them declaring it is not the same as all of them
# doing so.  One function keeps six link lines from drifting apart.
set(ZUUNED_WINDOWS_MANIFEST ${CMAKE_CURRENT_LIST_DIR}/packaging/windows/zuuned.manifest)

function(zuuned_long_paths target)
    if(WIN32 AND MSVC)
        set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")
        target_link_options(${target} PRIVATE
            "/MANIFEST:EMBED"
            "/MANIFESTINPUT:${ZUUNED_WINDOWS_MANIFEST}")
    endif()
endfunction()

# ---- libzune ----
set(LIBZUNE_DIR ${CMAKE_SOURCE_DIR}/libzune)

if(WIN32)
    # libzune's own Makefile is Linux-only: pkg-config probes, -l flags and .so
    # detection.  CMake compiles the same sources here instead.  driverkit_usb.c
    # is the macOS backend and is deliberately absent.  USE_LIBAV keeps the
    # in-process FFmpeg thumbnail path that Linux gets via the Makefile.
    set(LIBZUNE_A libzune_build)

    add_library(libzune_build STATIC
        libzune/src/device.c
        libzune/src/track.c
        libzune/src/video.c
        libzune/src/photo.c
        libzune/src/album.c
        libzune/src/playlist.c
        libzune/src/podcast.c
        libzune/src/audiobook.c
        libzune/src/spoken_inventory.c
        libzune/src/thumbnail.c
        libzune/src/zmdb.c
        libzune/src/zmdb_io.c
        libzune/src/podcast_state.c
        libzune/src/transcode.c
        libzune/src/util.c
        libzune/src/usb_recovery.c
        libzune/src/finalize.c
        libzune/src/search.c
        libzune/src/ptp.c
        libzune/src/mtp.c
        libzune/src/mtpz.c
        libzune/src/usb_libusb.c
    )
    target_compile_definitions(libzune_build PRIVATE USE_LIBAV)
    target_include_directories(libzune_build PUBLIC ${LIBZUNE_DIR}/include)
    target_include_directories(libzune_build PRIVATE
        ${LIBZUNE_DIR}/src
        ${ZUUNED_COMPAT_DIR}
        ${LIBUSB_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${LAME_INCLUDE_DIRS}
        ${PCRE2_INCLUDE_DIR})
    target_link_libraries(libzune_build PUBLIC
        ${ZUUNED_WINCOMPAT} ${LIBUSB_LIBRARIES} ${ZUUNED_SYSTEM_LIBS})
    target_compile_options(libzune_build PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:/utf-8>" ${ZUUNED_SHIM_FLAGS})
else()
    # Built by its own Makefile (auto-selects the libusb backend on Linux).
    # CMake just drives `make` and links the resulting static archive.
    set(LIBZUNE_A ${LIBZUNE_DIR}/libzune.a)

    add_custom_target(libzune_build
        COMMAND make -C ${LIBZUNE_DIR}
        BYPRODUCTS ${LIBZUNE_A}
        COMMENT "Building libzune (libusb backend)"
    )
endif()

# ---- App ----
qt_add_executable(zuuned
    src/main.cpp
    src/ApplicationIdentity.cpp
    src/ApplicationIdentity.h
    src/AppSettings.h
    src/ProfilePaths.h
    src/PathText.h
    src/ThreadDrain.h
    src/diagnostics/SessionLog.cpp
    src/diagnostics/SessionLog.h
    src/diagnostics/Redactor.cpp
    src/diagnostics/Redactor.h
    src/diagnostics/SupportReport.cpp
    src/diagnostics/SupportReport.h
    src/diagnostics/DiagnosticsService.cpp
    src/diagnostics/DiagnosticsService.h
    src/artwork/PrintRenderer.cpp
    src/artwork/PrintRenderer.h
    src/artwork/ArtworkRequest.cpp
    src/artwork/ArtworkRequest.h
    src/artwork/ArtworkPipeline.cpp
    src/artwork/ArtworkPipeline.h
    src/DeviceService.cpp
    src/DeviceService.h
    src/DeviceHistory.cpp
    src/DeviceHistory.h
    src/VideoTitleRepair.cpp
    src/VideoTitleRepair.h
    src/library/VideoIdentity.h
    src/DeviceWorker.cpp
    src/DeviceWorker.h
    src/GrungeMaskProvider.cpp
    src/GrungeMaskProvider.h
    src/TrackModel.cpp
    src/TrackModel.h
    src/TrackImportBatch.h
    src/UsbWatcher.cpp
    src/UsbWatcher.h
    src/spoken/SpokenMediaService.cpp
    src/spoken/SpokenMediaService.h
    src/spoken/PodcastQueuePlanner.cpp
    src/spoken/PodcastQueuePlanner.h
    src/spoken/SpokenStore.cpp
    src/spoken/SpokenStore.h
    src/LibraryService.cpp
    src/library/ArtworkHttp.cpp
    src/LibraryService.h
    src/library/LibraryTypes.h
    src/library/LibraryDb.cpp
    src/library/LibraryDb.h
    src/library/LibraryPaths.h
    src/library/PlaylistText.h
    src/library/LibraryScanner.cpp
    src/library/LibraryScanner.h
    src/library/AudioCopyFingerprint.h
    src/library/LocalMusicCopies.h
    src/library/VideoNaming.cpp
    src/library/VideoNaming.h
    src/library/TmdbClient.cpp
    src/library/TmdbClient.h
    src/library/VideoMatcher.cpp
    src/library/VideoMatcher.h
    src/library/VideoBrowserModel.cpp
    src/library/VideoBrowserModel.h
    src/library/FrameThumbnailService.cpp
    src/library/FrameThumbnailService.h
    src/library/LocalTrackModel.cpp
    src/library/LocalTrackModel.h
    src/library/OnlineAlbumArtService.cpp
    src/library/OnlineAlbumArtService.h
    src/library/AlbumArtService.cpp
    src/library/ArtistImageService.cpp
    src/library/ArtistImageService.h
    src/library/MusicIdentityClient.cpp
    src/library/MusicIdentityClient.h
    src/library/AlbumArtService.h
    src/sync/SyncEngine.cpp
    src/sync/SyncEngine.h
    src/sync/SyncQueueModel.cpp
    src/sync/SyncQueueModel.h
)

# The installer and manual command read these exact packaged bytes.
qt_add_resources(zuuned "zuuned_packaging"
    PREFIX "/zuuned/packaging"
    BASE "packaging"
    FILES packaging/68-zuuned.rules)

# The installed icon theme handles menu/file-manager launchers. This compiled
# copy covers direct AppImage launches and the live compositor/taskbar window.
qt_add_resources(zuuned "zuuned_app_identity"
    PREFIX "/zuuned/app"
    BASE "packaging/icons/hicolor/256x256/apps"
    FILES packaging/icons/hicolor/256x256/apps/zuuned.png)

# Must be set BEFORE qt_add_qml_module — the flag is read when the module
# processes the file. After, it's silently ignored and Theme registers as
# a plain type: every Theme.* binding falls back to defaults (white app).
set_source_files_properties(qml/Theme.qml qml/TrayState.qml qml/PhotoTrayState.qml qml/BookTrayState.qml qml/Prefs.qml qml/Gallery.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

# Put the QML files in the module's canonical resource directory. Qt 6.8.2
# treats the QTP0004 subdirectory redirect plus an explicit self-import as
# ambiguous. Aliasing away the source-only qml/ prefix gives implicit and
# explicit imports one location while keeping fonts/images/JS paths relative.
# This glob assigns aliases only; the explicit module inventory below still
# decides which files are compiled and shipped.
file(GLOB_RECURSE zuuned_qml_resources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/images/*")
foreach(resource IN LISTS zuuned_qml_resources)
    set_source_files_properties("qml/${resource}" PROPERTIES QT_RESOURCE_ALIAS "${resource}")
endforeach()

qt_add_qml_module(zuuned
    URI Zuuned
    VERSION 1.0
    NO_GENERATE_EXTRA_QMLDIRS
    QML_FILES
        qml/MediaKindIcon.qml
        qml/SpokenPlaybackControls.qml
        qml/PodcastPreferences.qml
        qml/SpokenFolderImport.qml
        qml/PodcastStatus.qml
        qml/PodcastCleanupControl.qml
        qml/PodcastSyncPicker.qml
        qml/SpokenMediaPage.qml
        qml/SpokenDetailHeading.qml
        qml/SpokenShelf.qml
        qml/SpokenButton.qml
        qml/SpokenCheckBox.qml
        qml/BookTrayState.qml
        qml/BookLibraryTray.qml
        qml/RoundedArtwork.qml
        qml/SpokenCard.qml
        qml/SpokenCoverSheet.qml
        qml/SpokenImportSheet.qml
        qml/Main.qml
        qml/Theme.qml
        qml/GradientText.qml
        qml/GrungeGlass.qml
        qml/EnvironmentBackground.qml
        qml/SidebarView.qml
        qml/PivotBar.qml
        qml/DevicePanel.qml
        qml/QueueEntryView.qml
        qml/VideoTitleRepairSheet.qml
        qml/MusicPage.qml
        qml/TrackListRow.qml
        qml/OnComputerBadge.qml
        qml/ArtistCard.qml
        qml/AlbumCard.qml
        qml/GenreTile.qml
        qml/MusicBreadcrumbBar.qml
        qml/ArtistDetail.qml
        qml/AlbumDetail.qml
        qml/DetailActionButton.qml
        qml/AlbumArt.qml
        qml/ArtworkImage.qml
        qml/ArtworkStyleSettings.qml
        qml/AlbumArtPlaceholder.qml
        qml/ZuneSearchField.qml
        qml/DeviceListPage.qml
        qml/LocalLibraryPage.qml
        qml/SplashView.qml
        qml/OnboardingView.qml
        qml/SettingsPage.qml
        qml/DiagnosticsSettings.qml
        qml/ToastHost.qml
        qml/CurvedText.qml
        qml/SpinningDiscView.qml
        qml/VinylRecordView.qml
        qml/QueueRibbonBar.qml
        qml/VolumeSlim.qml
        qml/CassetteTile.qml
        qml/ZuneScrollBar.qml
        qml/MiniPlayerWindow.qml
        qml/OnDeviceBadge.qml
        qml/ZuneMenuItem.qml
        qml/PosterCard.qml
        qml/LibraryVideosView.qml
        qml/VideoMovieDetail.qml
        qml/VideoSeriesDetail.qml
        qml/TmdbLookupSheet.qml
        qml/DeviceSpokenPage.qml
        qml/DeviceVideosPage.qml
        qml/CapacityRejectSheet.qml
        qml/RenameSheet.qml
        qml/VideoPlayerView.qml
        qml/OrbitalControls.qml
        qml/LibraryPhotosView.qml
        qml/PhotoGalleryView.qml
        qml/DevicePicturesPage.qml
        qml/PlaylistsPage.qml
        qml/SyncCompleteSheet.qml
        qml/SplitAddButton.qml
        qml/QuickPicker.qml
        qml/ZuneMenu.qml
        qml/TrayState.qml
        qml/Gallery.qml
        qml/GalleryView.qml
        qml/GenreDetail.qml
        qml/CustomizeSheet.qml
        qml/CustomizeArtwork.qml
        qml/CustomizeField.qml
        qml/MusicIdentity.js
        qml/PageHeaderBand.qml
        qml/Prefs.qml
        qml/PlaylistTray.qml
        qml/PhotoTrayState.qml
        qml/PhotoAlbumTray.qml
        qml/AddToPlaylistMenu.qml
        qml/EditTrackSheet.qml
        qml/DragGhost.qml
        qml/AlphabetRail.qml
        qml/NowPlayingIsland.qml
    RESOURCES
        qml/images/zuuned.png
        qml/fonts/PermanentMarker-Regular.ttf
        qml/fonts/SpecimenAlien-Regular.ttf
)

add_dependencies(zuuned libzune_build)

# Re-evaluate source identification at every build, including after a commit
# without a CMake reconfigure. The helper only replaces changed output files.
set(ZUUNED_SOURCE_REVISION "" CACHE STRING "Full app revision for a source archive without Git metadata")
set(ZUUNED_LIBZUNE_REVISION "" CACHE STRING "Full libzune revision for a source archive without Git metadata")
set(ZUUNED_SOURCE_DIRTY "" CACHE STRING "Archive app state: true or false; unset means unknown")
set(ZUUNED_LIBZUNE_DIRTY "" CACHE STRING "Archive libzune state: true or false; unset means unknown")
add_custom_target(zuuned_build_info
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/generated"
        "-DPROJECT_VERSION_VALUE=${PROJECT_VERSION}"
        "-DQT_VERSION_VALUE=${Qt6_VERSION}"
        "-DZUUNED_SOURCE_REVISION=${ZUUNED_SOURCE_REVISION}"
        "-DZUUNED_LIBZUNE_REVISION=${ZUUNED_LIBZUNE_REVISION}"
        "-DZUUNED_SOURCE_DIRTY=${ZUUNED_SOURCE_DIRTY}"
        "-DZUUNED_LIBZUNE_DIRTY=${ZUUNED_LIBZUNE_DIRTY}"
        -P "${CMAKE_SOURCE_DIR}/packaging/WriteBuildInfo.cmake"
    BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/generated/ZuunedBuildInfo.h"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/zuuned-build.json"
    VERBATIM
)
add_dependencies(zuuned zuuned_build_info)
target_include_directories(zuuned PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The app's own C++ carries its platform ifdefs, so compat/win is on the include
# path but the forced-include shim deliberately is not.
target_include_directories(zuuned PRIVATE src src/artwork src/diagnostics src/library src/sync ${LIBZUNE_DIR}/include ${LIBUSB_INCLUDE_DIRS} ${ZUUNED_COMPAT_DIR})
target_link_libraries(zuuned PRIVATE
    Qt6::Quick Qt6::Gui Qt6::Concurrent Qt6::Network
    ${LIBZUNE_A}
    ${ZUUNED_SYSTEM_LIBS}
    ${LIBUSB_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    zuuned_transcode
    zuuned_spoken
    zuuned_presence
    ${ZUUNED_WINCOMPAT}
)

# ════════════════════════════════════════════════════════════
# Interface language (2026-10-24).  i18n/zuuned_zh_CN.ts holds the Chinese for
# every page, menu and Settings tab; the Chinese itself lives in i18n/zh_CN.tsv,
# never in QML.  lrelease compiles it into the
# :/i18n resource prefix that src/main.cpp's installUiLanguage() loads.  The
# .ts is regenerated by i18n/update.ps1 (lupdate), not by hand, and its
# translations are written by hand -- see docs/I18N.md for what is and is not
# covered.
# ════════════════════════════════════════════════════════════
if(Qt6LinguistTools_FOUND)
    qt_add_translations(zuuned
        TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/i18n/zuuned_zh_CN.ts"
        RESOURCE_PREFIX /i18n)
    message(STATUS "[i18n] zh_CN catalogue will be built into zuuned")
else()
    message(STATUS "[i18n] no Qt6 LinguistTools; zuuned builds English-only")
endif()

# ════════════════════════════════════════════════════════════
# "Is a Zune plugged in, and may anyone open it?"  Not the same question on
# every platform, and on Windows the two sources genuinely disagree (see
# src/DevicePresence.h), so the answer is one small lib with one .cpp per
# platform plus devicetool, the CLI that lets a script check it headlessly.
# ════════════════════════════════════════════════════════════
add_library(zuuned_presence STATIC
    src/DevicePresence.cpp
    src/DevicePresence.h
)
if(WIN32)
    target_sources(zuuned_presence PRIVATE src/DevicePresenceWin.cpp)
else()
    target_sources(zuuned_presence PRIVATE src/DevicePresenceUnix.cpp)
endif()
target_include_directories(zuuned_presence PUBLIC src)
target_link_libraries(zuuned_presence PUBLIC Qt6::Core)
if(WIN32)
    # Devnode enumeration lives in cfgmgr32.  Nothing else in the app touches
    # the PnP manager, which keeps the device-side surface where it can be audited.
    target_link_libraries(zuuned_presence PUBLIC cfgmgr32)
endif()

add_executable(devicetool src/devicetool.cpp)
target_link_libraries(devicetool PRIVATE zuuned_presence Qt6::Core ${LIBUSB_LIBRARIES}
    ${ZUUNED_WINCOMPAT})
target_include_directories(devicetool PRIVATE ${LIBUSB_INCLUDE_DIRS})
zuuned_long_paths(devicetool)

# ═══════════════════════════════════════════════════════════════════
# Phase 2: transcode layer (transcode/) — shared C logic with the mac
# app, built against SYSTEM FFmpeg + LAME. Standalone static lib +
# the transcodetool CLI gate.  The zuuned app links this library too, so
# sync transcodes in-process instead of shelling out to the CLI gate.
# ═══════════════════════════════════════════════════════════════════
if(NOT WIN32)
    pkg_check_modules(FFMPEG REQUIRED
        libavformat libavcodec libavutil libswresample libswscale)
endif()

add_library(zuuned_transcode STATIC
    transcode/libav_transcode.c
    transcode/audio_encode.c
    transcode/id3_rewrite.c
)
if(WIN32)
    # WMA Lossless is not an FFmpeg target.  The encoder is the one in the Windows
    # Media Format runtime, so the Windows build adds one translation unit of
    # in-process WMSDK calls and three import libraries for DLLs that are already
    # on every Windows install - no vendored codec and no forked encoder project.
    # Elsewhere the same two entry points come from the stub, which keeps this
    # library's POSIX link line C-only: transcodetool is a C executable and has no
    # reason to acquire a C++ runtime for two stubs.
    target_sources(zuuned_transcode PRIVATE transcode/wma_lossless_enc.cpp)
    target_link_libraries(zuuned_transcode PUBLIC wmvcore wmcodecdspuuid ole32)
else()
    target_sources(zuuned_transcode PRIVATE transcode/wma_lossless_stub.c)
endif()
target_include_directories(zuuned_transcode PUBLIC transcode ${FFMPEG_INCLUDE_DIRS}
    ${ZUUNED_COMPAT_DIR} ${LAME_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIR})
target_link_libraries(zuuned_transcode PUBLIC
    ${FFMPEG_LIBRARIES} ${LAME_LIBRARIES} ${ZUUNED_LIBM} ${ZUUNED_WINCOMPAT})
if(WIN32)
    # Shared with the Linux/mac build, so it leans on the same POSIX surface
    # libzune does; see compat/win.
    # /utf-8 covers C++ here as well, which is the exception: this library's only
    # C++ source is the WMSDK encoder, it is pure ASCII, and the shared
    # libav_transcode.h it includes is UTF-8 - left to the console code page that
    # header draws C4819 on every compile.  Other targets keep the C-only spelling
    # because their C++ sources are not all clean ASCII.
    target_compile_options(zuuned_transcode PRIVATE
        "/utf-8" ${ZUUNED_SHIM_FLAGS})
endif()

add_executable(transcodetool transcode/transcodetool.c)
if(WIN32)
    # The CLI gate also names libzune objects.  The import libraries it needs
    # are spelled out here so the Windows link line stays explicit.
    target_link_libraries(transcodetool PRIVATE zuuned_transcode libzune_build
        ${LAME_LIBRARIES} ${ZUUNED_WINCOMPAT})
    target_compile_options(transcodetool PRIVATE "$<$<COMPILE_LANGUAGE:C>:/utf-8>")
else()
    target_link_libraries(transcodetool PRIVATE zuuned_transcode)
endif()
zuuned_long_paths(transcodetool)

# MTP framing gate (mtpframingtool).  The SendObject data phase streams from
# the file now instead of being malloc()ed whole, and what a Zune must not be
# able to notice is the difference: chunk sizes, max-packet alignment and
# zero-length-packet placement have to come out identical.  This runs both
# paths against a mock USB backend and compares every recorded bulk_out write.
# Software only - it opens no device handle and makes no libusb call, which is
# exactly why it can run with no Zune attached.
if(WIN32)
add_executable(mtpframingtool libzune/tools/mtpframingtool.c)
target_include_directories(mtpframingtool PRIVATE
    ${LIBZUNE_DIR}/src ${LIBZUNE_DIR}/include ${ZUUNED_COMPAT_DIR})
target_link_libraries(mtpframingtool PRIVATE ${LIBZUNE_A} ${ZUUNED_WINCOMPAT}
    ${LIBUSB_LIBRARIES} ${ZUUNED_SYSTEM_LIBS})
target_compile_options(mtpframingtool PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:/utf-8>" ${ZUUNED_SHIM_FLAGS})
zuuned_long_paths(mtpframingtool)
endif()

# Private spoken-media foundation. Reusable worker-only parsers; the inspection
# harness is a developer target, never an AppImage payload or runtime dependency.
add_library(zuuned_spoken STATIC
    src/spoken/PodcastFeed.cpp
    src/spoken/AudiobookProbe.cpp
)
target_include_directories(zuuned_spoken PUBLIC src/spoken PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(zuuned_spoken PUBLIC Qt6::Core
    PRIVATE ${FFMPEG_LIBRARIES} ${ZUUNED_WINCOMPAT})
add_executable(spokenmediatool src/spoken/spokenmediatool.cpp)
target_link_libraries(spokenmediatool PRIVATE zuuned_spoken ${ZUUNED_WINCOMPAT})
zuuned_long_paths(spokenmediatool)

# ═══════════════════════════════════════════════════════════════════
# Phase 3: library core CLI gate (librarytool) — scans a folder into a
# temp DB via the same LibraryDb/LibraryScanner the app uses.
# ═══════════════════════════════════════════════════════════════════
qt_add_executable(librarytool
    src/library/librarytool.cpp
    src/library/LibraryDb.cpp
    src/library/LibraryScanner.cpp
    # The cover self-test answers the real resolver, so the real client and its
    # transport are linked in; the fetch the test injects never reaches them.
    src/library/MusicIdentityClient.cpp
    src/library/ArtworkHttp.cpp
    src/library/VideoNaming.cpp
    src/library/TmdbClient.cpp
    src/library/VideoMatcher.cpp
    src/library/VideoMatcher.h
)
add_dependencies(librarytool libzune_build)
target_include_directories(librarytool PRIVATE
    src/library ${LIBZUNE_DIR}/include ${SQLITE3_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(librarytool PRIVATE
    Qt6::Core Qt6::Concurrent Qt6::Network
    ${SQLITE3_LIBRARIES}
    ${LIBZUNE_A}
    ${ZUUNED_SYSTEM_LIBS}
    ${LIBUSB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${ZUUNED_WINCOMPAT}
)
zuuned_long_paths(librarytool)

# ═══════════════════════════════════════════════════════════════════
# Phase 4: music playback core (src/player/) — MpvController (libmpv
# audio context + event thread) and PlayerService (QML singleton,
# PlayerService.swift port). playertool is the headless CLI gate.
# ═══════════════════════════════════════════════════════════════════
if(NOT WIN32)
    pkg_check_modules(MPV REQUIRED mpv)
endif()

target_sources(zuuned PRIVATE
    src/player/MpvController.cpp
    src/player/MpvController.h
    src/player/PlayerService.cpp
    src/player/PlayerService.h
    # Phase 7: video player (render API + binge queue service)
    src/player/MpvVideoItem.cpp
    src/player/MpvVideoItem.h
    src/player/VideoPlayerService.cpp
    src/player/VideoPlayerService.h
)
if(NOT WIN32)
    # MPRIS is the Linux D-Bus media-key integration.  There is no Windows
    # equivalent yet, and Qt6::DBus is not part of the Windows Qt build either.
    target_sources(zuuned PRIVATE
        src/player/MprisService.cpp
        src/player/MprisService.h
    )
endif()
target_include_directories(zuuned PRIVATE src/player ${MPV_INCLUDE_DIRS})
zuuned_long_paths(zuuned)

if(WIN32)
    target_link_libraries(zuuned PRIVATE ${MPV_LIBRARIES} Qt6::OpenGL)
else()
    target_link_libraries(zuuned PRIVATE ${MPV_LIBRARIES} Qt6::OpenGL Qt6::DBus)
endif()

qt_add_executable(playertool
    src/player/playertool.cpp
    src/player/MpvController.cpp
    src/player/PlayerService.cpp
)
target_include_directories(playertool PRIVATE src/player ${MPV_INCLUDE_DIRS})
target_link_libraries(playertool PRIVATE
    Qt6::Core Qt6::Qml
    ${MPV_LIBRARIES}
    ${ZUUNED_WINCOMPAT}
)
zuuned_long_paths(playertool)

# ── Install (E3 ship mechanics) ──
include(GNUInstallDirs)

# Linux ship mechanics only.  Windows ships as a self-contained directory put
# together with the steps in BUILDING.md (ninja, then windeployqt beside the exe
# plus the DLLs the vcpkg toolchain already copies next to it), so there is no
# Windows install layout yet and `cmake --install` deliberately does nothing.
if(NOT WIN32)
    install(TARGETS zuuned RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES LICENSE COPYING.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/zuuned)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/zuuned-build.json"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/zuuned)
    install(FILES packaging/zuuned.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/icons/zuuned.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES packaging/icons/hicolor/${size}x${size}/apps/zuuned.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
    endforeach()
    # udev rules are architecture independent: GNUInstallDirs' libdir may be a
    # Debian multiarch directory or lib64, neither of which udev searches.
    install(FILES packaging/68-zuuned.rules
            DESTINATION lib/udev/rules.d)
    install(FILES packaging/68-zuuned.rules
            DESTINATION lib/udev/rules.d RENAME 72-zuuned.rules)
endif()
