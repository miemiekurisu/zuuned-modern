# Regenerate i18n/zuuned_zh_CN.ts from the sources that carry translatable text.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File i18n\update.ps1
#
# lupdate refreshes the <source> strings and keeps every <translation> it can
# match, so the Chinese written by hand survives; strings that disappeared come
# back marked obsolete and -no-obsolete drops them.  The Chinese itself is
# written by hand (or with build/art5/ts_fill.py against a map file), never
# machine-generated: see docs/I18N.md for what is and is not covered.
#
# The list below IS the coverage claim.  C++ wording joins this list only when it
# leaves through UiText::tr() (src/UiText.h), which lupdate extracts under the
# fixed context UiText and the runtime looks up the same way; a bare QStringLiteral
# or QString::fromUtf8 sentence here would be invisible to translation.  A page added here without a translation
# shipped for it is a page that lies about being Chinese.
#
# After this runs, configure/build compiles the .ts into the :/i18n resource
# prefix.  Do not commit the .qm that lrelease or CMake produces.
param(
    [string]$Lupdate = "D:\dev\Qt\6.11.0\msvc2022_64\bin\lupdate.exe"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Lupdate)) {
    throw "lupdate not found at $Lupdate; pass -Lupdate <path> for another Qt."
}
$sources = (
    "qml/AddToPlaylistMenu.qml",
    "qml/AlbumDetail.qml",
    "qml/ArtistCard.qml",
    "qml/ArtistDetail.qml",
    "qml/ArtworkStyleSettings.qml",
    "qml/BookLibraryTray.qml",
    "qml/CapacityRejectSheet.qml",
    "qml/CustomizeSheet.qml",
    "qml/DeviceListPage.qml",
    "qml/DevicePanel.qml",
    "qml/DevicePicturesPage.qml",
    "qml/DeviceSpokenPage.qml",
    "qml/DeviceVideosPage.qml",
    "qml/DiagnosticsSettings.qml",
    "qml/EditTrackSheet.qml",
    "qml/GenreDetail.qml",
    "qml/GenreTile.qml",
    "qml/LibraryPhotosView.qml",
    "qml/LibraryVideosView.qml",
    "qml/LocalLibraryPage.qml",
    "qml/Main.qml",
    "qml/MiniPlayerWindow.qml",
    "qml/MusicBreadcrumbBar.qml",
    "qml/MusicPage.qml",
    "qml/NowPlayingIsland.qml",
    "qml/OnComputerBadge.qml",
    "qml/OnDeviceBadge.qml",
    "qml/OnboardingView.qml",
    "qml/PhotoAlbumTray.qml",
    "qml/PhotoGalleryView.qml",
    "qml/PlaylistTray.qml",
    "qml/PlaylistsPage.qml",
    "qml/PodcastCleanupControl.qml",
    "qml/PodcastPreferences.qml",
    "qml/PodcastSyncPicker.qml",
    "qml/PosterCard.qml",
    "qml/QueueEntryView.qml",
    "qml/QuickPicker.qml",
    "qml/RenameSheet.qml",
    "qml/SettingsPage.qml",
    "qml/SidebarView.qml",
    "qml/SpokenCard.qml",
    "qml/SpokenCoverSheet.qml",
    "qml/SpokenDetailHeading.qml",
    "qml/SpokenFolderImport.qml",
    "qml/SpokenImportSheet.qml",
    "qml/SpokenMediaPage.qml",
    "qml/SpokenPlaybackControls.qml",
    "qml/SyncCompleteSheet.qml",
    "qml/TmdbLookupSheet.qml",
    "qml/ToastHost.qml",
    "qml/TrackListRow.qml",
    "qml/VideoMovieDetail.qml",
    "qml/VideoPlayerView.qml",
    "qml/VideoSeriesDetail.qml",
    "qml/VideoTitleRepairSheet.qml",
    "qml/ZuneSearchField.qml",
    "src/main.cpp",
    "src/LibraryService.cpp",
    "src/DeviceService.cpp",
    "src/DeviceService.h",
    "src/DeviceWorker.cpp",
    "src/DevicePresence.cpp",
    "src/DevicePresenceWin.cpp",
    "src/VideoTitleRepair.cpp",
    "src/artwork/ArtworkPipeline.cpp",
    "src/library/ArtworkHttp.cpp",
    "src/library/ArtistImageService.cpp",
    "src/library/OnlineAlbumArtService.cpp",
    "src/library/LibraryScanner.cpp",
    "src/library/MusicIdentityClient.cpp",
    "src/spoken/AudiobookProbe.cpp",
    "src/spoken/PodcastFeed.cpp",
    "src/spoken/PodcastQueuePlanner.cpp",
    "src/spoken/SpokenStore.cpp",
    "src/spoken/SpokenMediaService.cpp",
    "src/sync/RecoveryIdentity.h",
    "src/sync/SyncQueueModel.cpp",
    "src/sync/SyncEngine.cpp"
)
Push-Location $root
try {
    & $Lupdate -no-obsolete -locations none @sources -ts i18n/zuuned_zh_CN.ts
    if ($LASTEXITCODE -ne 0) { throw "lupdate failed with $LASTEXITCODE" }
}
finally { Pop-Location }
