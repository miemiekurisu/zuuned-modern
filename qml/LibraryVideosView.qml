import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Zuuned

// Local video library — port of LibraryVideosView.swift.
// Pivots: Movies / TV Shows / Anime / Music Videos / Other / Needs
// Match (one pivot family with the device page — grid study
// 2026-09-06). Poster cards appear only when FULLY ready (TMDB lookup
// done + poster on disk) — the "pop pop pop" as lookups finish. TV
// tiles show immediately from folder-truth, with frame-grab faces when
// TMDB has no poster yet. Music Videos / Other are filed by hand and
// skip the readiness gate entirely.
//
// ALL is the landing pivot (2026-11-14): every file in the library, one
// card each, whatever the TMDB lookup made of it. Before it a file the
// lookup never resolved sat on no shelf anyone would think to open -
// the page looked empty and the files looked lost. The faces come from
// TMDB when TMDB has one and from our own frame grab when it does not.
Item {
    id: root

    property string searchText: ""
    property string currentTab: "All"
    onCurrentTabChanged: gallery.letterFilter = ""
    readonly property bool flatTab: currentTab === "All"
        || currentTab === "Movies"
        || currentTab === "Music Videos" || currentTab === "Other"

    // Drill state: null | {kind:"movie", id} | {kind:"series", key}
    property var detail: null
    readonly property string drillTitle: detail === null ? ""
        : (detail.name
           ?? (detail.kind === "movie"
               ? (videoBrowser.movieDetail(detail.id).title ?? "")
               : detail.key))

    VideoBrowserModel {
        id: videoBrowser
        library: LibraryService
        deviceVideos: DeviceService.videosList
        deviceConnected: DeviceService.connected
    }

    // QA harness (screenshot runs): `--drill-first-movie` opens the
    // first movie's detail once its group is ready, so the drill page
    // can be captured without a click.
    Connections {
        target: videoBrowser
        function onGroupsChanged() {
            if (root.detail === null
                && Qt.application.arguments.indexOf("--drill-first-movie") >= 0
                && videoBrowser.movieGroups.length > 0)
                root.detail = { kind: "movie",
                                id: videoBrowser.movieGroups[0].id,
                                name: videoBrowser.movieGroups[0].title }
        }
    }

    function matchesSearch(text) {
        return root.searchText === ""
            || text.toLowerCase().includes(root.searchText.toLowerCase())
    }
    readonly property var shownAll: videoBrowser.allVideos.filter(
        v => matchesSearch(v.displayTitle) || matchesSearch(v.filename))
    readonly property var shownMovies: videoBrowser.movieGroups.filter(
        m => matchesSearch(m.title) || matchesSearch(m.filename))
    readonly property var shownSeries: videoBrowser.seriesList.filter(
        s => matchesSearch(s.name))
    readonly property var shownAnime: videoBrowser.animeList.filter(
        s => matchesSearch(s.name))
    readonly property var shownMusicVideos: videoBrowser.musicVideosList.filter(
        v => matchesSearch(v.displayTitle) || matchesSearch(v.filename))
    readonly property var shownOthers: videoBrowser.othersList.filter(
        v => matchesSearch(v.displayTitle) || matchesSearch(v.filename))
    readonly property var shownUnmatchedGroups: videoBrowser.unmatchedSeriesGroups.filter(
        g => matchesSearch(g.name))
    readonly property var shownUnmatchedLoose: videoBrowser.unmatchedLoose.filter(
        v => matchesSearch(v.filename))

    // Menus and pickers outlive individual tiles. Resolve at invocation time so
    // Customize's surgical repaint cannot leave an older transfer/play payload.
    function currentVideo(v) {
        return v && v.id !== undefined ? videoBrowser.movieDetail(v.id) : ({})
    }
    function itemIds(v) {
        return v.versionIds ?? [v.id]
    }
    function customizeVideo(v, ids) {
        const row = currentVideo(v)
        if (row.id === undefined) return
        customizeSheet.openFor({
            ids: ids ?? [row.id], kind: row.isTV ? "series" : "movie",
            title: row.isTV ? row.series : (row.title || row.cleanedQuery || row.filename),
            poster: row.poster ?? "", tmdbId: row.tmdbId ?? 0,
            season: row.season, episode: row.episode, episodeTitle: row.episodeTitle
        })
    }
    function customizeSeries(key) {
        const ids = videoBrowser.seriesEpisodeIds(key)
        if (ids.length > 0) customizeVideo({id: ids[0]}, ids)
    }
    function menuForMovie(v) {
        const row = currentVideo(v)
        if (row.id === undefined) return
        movieMenu.target = Object.assign({}, row, {versionIds: itemIds(v)})
        movieMenu.popup()
    }
    function episodeSyncItem(ep) {
        ep = currentVideo(ep)
        return {
            filepath: ep.filepath,
            // NO show-title fallback: a season of episodes all named
            // after the show reads as a bug. Empty title → the queue
            // shows SxxEyy and the wire name stays "Series - SxxEyy".
            title: ep.episodeTitle ?? "",
            description: ep.description,
            posterPath: ep.posterPath,
            series: ep.series,
            season: ep.season,
            episode: ep.episode,
            category: ep.category,
            durationMs: ep.durationMs,
            filesize: ep.filesize,
            libraryId: ep.id
        }
    }
    // Toast what ACTUALLY happened. Capacity refusals get the modal, so
    // the toast stays quiet about those; duplicates were previously a
    // silent nothing — the worst kind of button.
    function toastAddResult(r, noun) {
        if (r.error) { toastHost.show(r.error); return }
        if (r.added > 0)
            toastHost.show("queued " + (r.added === 1 ? "1 " + noun
                           : r.added + " " + noun + "s") + " for the device")
        else if (r.rejected === 0 && r.duplicates > 0)
            toastHost.show(r.duplicates === 1
                           ? "already in the queue"
                           : "all " + r.duplicates + " already in the queue")
    }
    function videoSyncItem(v) {
        const row = currentVideo(v)
        return row.isTV ? episodeSyncItem(row) : movieSyncItem(row)
    }
    function queueEpisode(ep) {
        if (!DeviceService.connected || currentVideo(ep).id === undefined) return
        toastAddResult(SyncEngine.addVideos([videoSyncItem(ep)]), "episode")
    }
    function queueEpisodes(eps) {
        if (!DeviceService.connected) return
        eps = eps.map(currentVideo).filter(e => e.id !== undefined)
        if (eps.length === 0) return
        // ONE call for the whole batch — per-episode calls each popped
        // their own capacity modal, and only the last survived.
        toastAddResult(SyncEngine.addVideos(eps.map(e => root.videoSyncItem(e))),
                       "episode")
    }
    function movieSyncItem(m) {
        m = currentVideo(m)
        return {
            filepath: m.filepath,
            // Filed shelves carry displayTitle (cleaned filename when
            // unmatched) — movies just have title.
            title: m.displayTitle ?? m.title,
            description: m.description,
            posterPath: m.posterPath,
            category: m.category,
            durationMs: m.durationMs,
            filesize: m.filesize,
            libraryId: m.id
        }
    }
    function seriesSyncItems(key) {
        let eps = []
        for (const s of videoBrowser.seriesDetail(key).seasons ?? [])
            eps = eps.concat(videoBrowser.episodesForSeason(key, s))
        return eps.map(e => root.episodeSyncItem(e))
    }
    function queueMovie(m) {
        if (!DeviceService.connected || currentVideo(m).id === undefined) return
        toastAddResult(SyncEngine.addVideos([videoSyncItem(m)]),
                       m.category === "music_video" ? "music video" : "movie")
    }
    function queueSeries(key) {
        let eps = []
        for (const s of videoBrowser.seriesDetail(key).seasons ?? [])
            eps = eps.concat(videoBrowser.episodesForSeason(key, s))
        queueEpisodes(eps)
    }

    // ── Playback (Phase 7) ──
    // Mac resume rules: resume only when >60s in and not inside the
    // last 30s of the file.
    function resumeMsFor(v) {
        const pos = v.lastPositionMs ?? 0
        const dur = v.durationMs ?? 0
        return (pos > 60000 && (dur === 0 || pos < dur - 30000)) ? pos : 0
    }
    function episodePlayTitle(e) {
        const label = "S" + e.season + "E" + e.episode
        const name = e.episodeTitle !== "" ? e.episodeTitle : label
        return (e.series !== "" ? e.series + " — " : "") + name
    }
    function posterUrlFor(v) {
        const p = v.posterPath ?? v.poster ?? ""
        if (p === "") return ""
        return p.startsWith("file:") ? p : ZPathText.url(p)
    }
    function playMovie(m) {
        m = currentVideo(m)
        if (m.id === undefined) return
        VideoPlayerService.setQueue([{
            filepath: m.filepath,
            title: m.isTV ? root.episodePlayTitle(m) : (m.displayTitle || m.title || m.filename),
            libraryId: m.id,
            poster: root.posterUrlFor(m)
        }], 0, resumeMsFor(m))
    }
    // Binge queue: the given episodes from startIndex, resuming the
    // start episode when the rules allow.
    function playEpisodes(eps, startIndex) {
        const selectedId = eps[startIndex]?.id
        eps = eps.map(currentVideo).filter(e => e.id !== undefined)
        startIndex = eps.findIndex(e => e.id === selectedId)
        if (eps.length === 0)
            return
        const idx = Math.max(0, Math.min(startIndex, eps.length - 1))
        VideoPlayerService.setQueue(eps.map(e => ({
            filepath: e.filepath,
            title: root.episodePlayTitle(e),
            libraryId: e.id,
            poster: root.posterUrlFor(e)
        })), idx, resumeMsFor(eps[idx]))
    }
    // Card play: a series starts at its first unwatched episode.
    function playSeries(key) {
        let eps = []
        for (const s of videoBrowser.seriesDetail(key).seasons ?? [])
            eps = eps.concat(videoBrowser.episodesForSeason(key, s))
        const start = eps.findIndex(e => !e.watched)
        playEpisodes(eps, start < 0 ? 0 : start)
    }

    component UnmatchedGesture: MouseArea {
        id: unmatchedGesture
        objectName: "unmatchedGesture"
        property var rows: []
        property string label: ""
        property bool group: false
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        drag.target: DeviceService.connected ? unmatchedGhost : null
        preventStealing: DeviceService.connected
        onPressed: mouse => unmatchedGhost.place(this, mouse)
        onReleased: unmatchedGhost.drop()
        onClicked: mouse => {
            if (mouse.button !== Qt.RightButton || rows.length === 0) return
            unmatchedMenu.rows = rows
            unmatchedMenu.group = group
            unmatchedMenu.popup()
        }
        DragGhost {
            id: unmatchedGhost
            area: unmatchedGesture
            dragKind: "videos"
            dragName: unmatchedGesture.label
            dragTracks: () => unmatchedGesture.rows.map(root.currentVideo)
                .filter(v => v.id !== undefined)
                .map(v => v.isTV ? root.episodeSyncItem(v) : root.movieSyncItem(v))
            Rectangle {
                width: Theme.spaceXxl; height: width * 1.5
                radius: Theme.radiusSm; color: Theme.artworkPlaceholder
            }
        }
    }

    component UnmatchedRow: Item {
        id: unmatchedRow
        default property alias content: unmatchedContent.data
        property var rows: []
        property string label: ""
        property bool group: false
        implicitHeight: unmatchedContent.implicitHeight
        UnmatchedGesture { rows: unmatchedRow.rows; label: unmatchedRow.label; group: unmatchedRow.group }
        RowLayout {
            id: unmatchedContent
            anchors.fill: parent
            spacing: Theme.spaceMd
        }
    }

    // ── Page column: band (always) · breadcrumb (drilled) · content ──
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header band: pivots · counts · search — fixed height,
        // pivots on the spine (one chrome band, 2026-09-06)
        PageHeaderBand {
            Layout.leftMargin: Gallery.spine(root.width)
            Layout.rightMargin: Theme.spaceXxxl

            PivotBar {
                id: pivotBar
                // Natural tab width — the counts + search claim the rest.
                //
                // The reservation COMPRESSES on narrow windows. A fixed
                // 660 made this band's implicit width (spine + 660 +
                // spacing + search + inset ≈ 959) exceed the content
                // column whenever the device panel was open on a
                // smaller window; the page's ColumnLayout then sized
                // its fillWidth children to 959, the poster grid
                // inherited that width, and its right columns were
                // clipped under the panel (Orson 2026-09-11). Floor is
                // the tabs' own natural width, so they never crowd the
                // search; roomy windows still get exactly 660.
                // root-derived, never parent.width — a self-referential
                // width in a layout row rearranges recursively.
                // 700 rather than 660 since All joined the row; the
                // tabs' own width stays the floor, so a narrow window
                // still gets its pivots whole.
                Layout.preferredWidth: Math.min(700,
                    Math.max(implicitWidth, root.width - 300))
                // Never below the tabs' own width — the band can now
                // compress, and the Row inside doesn't clip, so a
                // smaller reservation would let the search ride over
                // the tabs instead of shrinking something flexible.
                Layout.minimumWidth: implicitWidth
                tabs: ["All", "Movies", "TV Shows", "Anime",
                       "Music Videos", "Other", "Needs Match"]
                labels: [qsTr("All"), qsTr("Movies"), qsTr("TV Shows"),
                         qsTr("Anime"), qsTr("Music Videos"), qsTr("Other"),
                         qsTr("Needs Match")]
                current: root.currentTab
                onCurrentChanged: {
                    root.currentTab = current
                    root.detail = null
                }
            }

            Item { Layout.fillWidth: true }

            ColumnLayout {
                spacing: 2
                Layout.alignment: Qt.AlignVCenter
                // Drop the counts before they crowd the row. Threshold
                // covers the full band (spine + 660 pivot + gaps +
                // search + inset ≈ 940) plus the counts' own ~130 —
                // below this the tabs and search take the room and the
                // grid keeps its scrollbar and A–Z rail.
                visible: root.width > 1150
                Text {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("%1 movies, %2 tv episodes")
                      .arg(videoBrowser.movieCount).arg(videoBrowser.totalEpisodeCount)
                    font.pixelSize: 11
                    font.weight: Font.Light
                    color: Theme.textDim
                }
                Text {
                    Layout.alignment: Qt.AlignRight
                    visible: videoBrowser.pendingCount > 0
                    text: qsTr("%1 still being looked up...").arg(videoBrowser.pendingCount)
                    font.pixelSize: 10
                    color: Qt.alpha(Theme.pink, 0.8)
                }
                Text {
                    Layout.alignment: Qt.AlignRight
                    visible: videoBrowser.unmatchedCount > 0
                             && root.currentTab !== "Needs Match"
                    text: qsTr("%1 need manual match").arg(videoBrowser.unmatchedCount)
                    font.pixelSize: 10
                    color: Qt.alpha(Theme.orange, 0.85)
                }
            }

            ZuneSearchField {
                // Narrow windows give the tabs the room instead
                Layout.preferredWidth: root.width < 900 ? 110 : 160
                Layout.minimumWidth: 110
                Layout.alignment: Qt.AlignVCenter
                onTextChanged: root.searchText = text
            }
        }

        // Breadcrumb under the band — the tabs NEVER leave (music's
        // drill pattern, one style app-wide)
        MusicBreadcrumbBar {
            Layout.fillWidth: true
            visible: root.detail !== null
            inset: Gallery.spine(root.width)
            // The tab's label, not its token - a breadcrumb reading
            // "All" inside a Chinese interface is a stray.
            crumbs: [pivotBar.labels[pivotBar.tabs.indexOf(root.currentTab)]
                     ?? root.currentTab, root.drillTitle]
            onBackClicked: root.detail = null
            onCrumbClicked: root.detail = null
        }

        // ── Content ──
        Item {
            objectName: "videoContent"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spaceMd

            // Poster gallery — capped columns, centered block, letter
            // breaks, rail on the shoulder (grid study 2026-09-06)
            GalleryView {
                id: gallery
                anchors.fill: parent
                visible: root.currentTab !== "Needs Match"
                         && root.detail === null
                idealCell: 190
                maxCols: 8
                // Posters scale down on a small window, but the column
                // cap never lifts — Hick's law keeps the wall calm
                liftColumnCap: false
                anchorX: Gallery.spine(root.width)
                cellHeightFor: (w) => Math.round(w * 1.5) + 56
                nameOf: (r) => r.displayTitle ?? r.title ?? r.name ?? ""
                rows: root.currentTab === "All" ? root.shownAll
                    : root.currentTab === "Movies" ? root.shownMovies
                    : root.currentTab === "TV Shows" ? root.shownSeries
                    : root.currentTab === "Anime" ? root.shownAnime
                    : root.currentTab === "Music Videos" ? root.shownMusicVideos
                    : root.currentTab === "Other" ? root.shownOthers
                    : []
                cardDelegate: Item {
                    id: cell
                    required property var modelData
                    width: gallery.plan.cell
                    height: gallery.cellHeightFor(gallery.plan.cell)

                    readonly property bool isMovie: root.currentTab === "Movies"
                    // All and the filed shelves: flat single files,
                    // like movies but with no detail page and no
                    // readiness gate
                    readonly property bool isFiled:
                        root.currentTab === "All"
                        || root.currentTab === "Music Videos"
                        || root.currentTab === "Other"
                    readonly property bool isSingle: isMovie || isFiled
                    // Surgical refresh: a corrected row's fresh fields
                    // land here (keyed by the row's id — the series
                    // card reads its representative episode) so only
                    // this tile repaints, no grid reset.
                    readonly property var ov: {
                        const key = isSingle ? String(modelData.id)
                                             : String(modelData.firstEpisodeId)
                        return videoBrowser.rowOverrides[key] ?? null
                    }
                    // Merged view for the single-item cards
                    readonly property var r: Object.assign({}, modelData, ov ?? {})

                    PosterCard {
                        posterW: gallery.plan.cell
                        dragItems: () => parent.isSingle
                            ? [root.videoSyncItem(parent.r)]
                            : root.seriesSyncItems(parent.modelData.key)
                        posterSource: {
                            // Override poster wins (movie row, or the
                            // series' representative episode row)
                            const p = parent.ov ? (parent.ov.poster ?? "")
                                                : parent.modelData.poster
                            if (p !== "")
                                return p
                            if (parent.isMovie)
                                return ""
                            // A TMDB episode still is a better face than
                            // a frame we decoded ourselves.
                            const st = parent.modelData.still ?? ""
                            if (st !== "")
                                return st
                            // Frame-grab fallback: the All wall, TMDB-less
                            // series, and the filed shelves (music videos
                            // rarely match)
                            const key = parent.isFiled
                                ? String(parent.modelData.id)
                                : String(parent.modelData.firstEpisodeId)
                            const s = FrameThumbnailService.stills[key]
                            return s === undefined ? "" : s
                        }
                        title: parent.isFiled ? parent.r.displayTitle
                             : parent.isMovie ? parent.r.title
                                              : parent.modelData.name
                        subtitle: {
                            if (parent.isSingle)
                                return (parent.r.year + " "
                                        + parent.r.genres).trim()
                            let s = parent.modelData.episodeCount
                                + (parent.modelData.episodeCount === 1
                                   ? " episode · " : " episodes · ")
                                + parent.modelData.seasonCount
                                + (parent.modelData.seasonCount === 1
                                   ? " season" : " seasons")
                            // Partially synced: say how much is over there
                            // (full sync gets the corner badge instead)
                            const n = parent.modelData.onDeviceCount ?? 0
                            if (n > 0 && n < parent.modelData.episodeCount)
                                s += " · " + n + " on zune"
                            return s
                        }
                        badge: parent.isMovie ? "MOVIE"
                             : root.currentTab === "Anime" ? "ANIME"
                             : root.currentTab === "Music Videos" ? "MV"
                             : root.currentTab === "Other"
                               || root.currentTab === "All" ? "" : "TV"
                        badgeColor: parent.isMovie ? Theme.orange
                                  : root.currentTab === "Music Videos"
                                    ? Theme.activePink : Theme.pink
                        isOnDevice: parent.modelData.isOnDevice
                        versionCount: parent.isMovie ? parent.modelData.versionCount : 1

                        // Filed shelves have no detail page — the card
                        // plays (artwork is the play button)
                        onOpened: {
                            if (parent.isFiled)
                                root.playMovie(parent.r)
                            else
                                root.detail = parent.isMovie
                                    ? {kind: "movie", id: parent.modelData.id,
                                       name: parent.r.title}
                                    : {kind: "series", key: parent.modelData.key,
                                       name: parent.modelData.name}
                        }
                        onPlayed: parent.isSingle
                            ? root.playMovie(parent.r)
                            : root.playSeries(parent.modelData.key)
                        // Single files — plain ＋, no ▾
                        isContainer: !parent.isSingle
                        onAddAll: parent.isSingle
                            ? root.queueMovie(parent.r)
                            : root.queueSeries(parent.modelData.key)
                        onPickParts: {
                            if (!DeviceService.connected) return
                            if (parent.isSingle) {
                                root.queueMovie(parent.r)
                            } else {
                                // Season-first groups — nobody scrolls
                                // 278 flat episodes (design pivot).
                                const key = parent.modelData.key
                                const seasons = videoBrowser.seriesDetail(key).seasons ?? []
                                let flat = []
                                const groups = seasons.map(s => {
                                    const eps = videoBrowser.episodesForSeason(key, s)
                                    flat = flat.concat(eps)
                                    return {
                                        id: "season" + s,
                                        label: s > 0 ? qsTr("Season %1").arg(s) : qsTr("Specials"),
                                        sublabel: qsTr("%1 eps").arg(eps.length),
                                        children: eps.map(e => ({
                                            id: String(e.id),
                                            label: "E" + e.episode
                                                + (e.episodeTitle !== ""
                                                       ? " · " + e.episodeTitle : ""),
                                            sublabel: ""
                                        }))
                                    }
                                })
                                root._pickerEpisodes = flat
                                episodePicker.openAt(parent,
                                    parent.modelData.title ?? parent.modelData.name
                                        ?? "series",
                                    groups)
                            }
                        }
                        onRightClicked: {
                            if (parent.isSingle) {
                                root.menuForMovie(parent.r)
                            } else {
                                seriesMenu.target = parent.modelData
                                seriesMenu.popup()
                            }
                        }

                        Component.onCompleted: {
                            if (parent.modelData.poster !== ""
                                || (parent.modelData.still ?? "") !== "")
                                return
                            if (parent.isFiled)
                                FrameThumbnailService.requestStill(
                                    parent.modelData.id,
                                    parent.modelData.filepath,
                                    parent.modelData.durationMs)
                            else if (!parent.isMovie)
                                FrameThumbnailService.requestStill(
                                    parent.modelData.firstEpisodeId,
                                    parent.modelData.firstEpisodePath,
                                    parent.modelData.firstEpisodeDurationMs)
                        }
                    }
                }
            }

            // Empty-state hints for the shelves that need explaining
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spaceSm
                visible: root.detail === null
                      && ((root.currentTab === "All"
                          && videoBrowser.allVideos.length === 0)
                      || (root.currentTab === "Anime"
                          && videoBrowser.animeList.length === 0)
                      || (root.currentTab === "Music Videos"
                          && videoBrowser.musicVideosList.length === 0)
                      || (root.currentTab === "Other"
                          && videoBrowser.othersList.length === 0))
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.currentTab === "All"
                        ? qsTr("No videos yet.")
                        : root.currentTab === "Anime" ? qsTr("No anime sources yet.")
                        : root.currentTab === "Music Videos"
                          ? qsTr("No music videos filed yet.") : qsTr("Nothing filed as other.")
                    font.pixelSize: 14
                    font.weight: Font.Light
                    color: Theme.textSecondary
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 440
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: root.currentTab === "All"
                        ? qsTr("Add a video folder in Settings and its files show up here right away.")
                        : root.currentTab === "Anime"
                        ? qsTr("Add a folder as \"anime\" in Settings, or set an existing source's type to anime there.")
                        : root.currentTab === "Music Videos"
                          ? qsTr("Right-click any movie and pick \"Move to Music Videos\" — or file a Needs Match item as other.")
                          : qsTr("Right-click any movie and pick \"Move to Other\" — or file a Needs Match item as other.")
                    font.pixelSize: 11
                    color: Theme.textDim
                }
            }

            // ── Detail pages replace the grid (mac's drill behavior) ──
            Loader {
                anchors.fill: parent
                active: root.detail !== null && root.detail.kind === "movie"
                sourceComponent: VideoMovieDetail {
                    browser: videoBrowser
                    videoId: root.detail.id
                    onBack: root.detail = null
                    onQueueRequested: m => root.queueMovie(m)
                    onPlayRequested: m => root.playMovie(m)
                    onMovieSelected: id => root.detail = { kind: "movie", id: id }
                    movieSyncItemFn: root.videoSyncItem
                    onCustomizeRequested: (m, ids) => root.customizeVideo(m, ids)
                    onMenuRequested: m => root.menuForMovie(m)
                }
            }
            Loader {
                anchors.fill: parent
                active: root.detail !== null && root.detail.kind === "series"
                sourceComponent: VideoSeriesDetail {
                    browser: videoBrowser
                    seriesKey: root.detail.key
                    episodeSyncItemFn: root.videoSyncItem
                    onBack: root.detail = null
                    onQueueEpisodeRequested: ep => root.queueEpisode(ep)
                    onQueueEpisodesRequested: eps => root.queueEpisodes(eps)
                    onQueueSeriesRequested: key => root.queueSeries(key)
                    onPlayEpisodesRequested: (eps, startIndex) =>
                        root.playEpisodes(eps, startIndex)
                    onCustomizeRequested: key => root.customizeSeries(key)
                    onCustomizeEpisodeRequested: ep => root.customizeVideo(ep)
                    onCustomizeEpisodesRequested: eps => {
                        if (eps.length > 0) root.customizeVideo(eps[0], eps.map(e => e.id))
                    }
                }
            }

            // ── Needs Match ──
            Flickable {
                // Scrollbar at the page edge — one x across sections
                ScrollBar.vertical: ZuneScrollBar {
                    parent: root
                    visible: root.currentTab === "Needs Match"
                             && root.detail === null && size < 1.0
                    anchors.top: parent.top
                    anchors.topMargin: 64
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                }
                anchors.fill: parent
                // Same spine as the gallery tabs — no jump on entry
                anchors.leftMargin: Gallery.spine(root.width)
                anchors.rightMargin: Theme.spaceXxl
                visible: root.currentTab === "Needs Match"
                         && root.detail === null
                clip: true
                contentHeight: unmatchedColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: unmatchedColumn
                    width: parent.width
                    spacing: 0

                    // Empty state
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spaceXxl
                        spacing: Theme.spaceSm
                        visible: root.shownUnmatchedGroups.length === 0
                                 && root.shownUnmatchedLoose.length === 0
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Nothing to match.")
                            font.pixelSize: 14
                            font.weight: Font.Light
                            color: Theme.textSecondary
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Videos whose TMDB lookup finds no result show up here.")
                            font.pixelSize: 11
                            color: Theme.textDim
                        }
                    }

                    // Retry all
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.bottomMargin: Theme.spaceXs
                        visible: root.shownUnmatchedGroups.length > 0
                                 || root.shownUnmatchedLoose.length > 0
                        Item { Layout.fillWidth: true }
                        Text {
                            text: qsTr("Retry All")
                            font.pixelSize: 12
                            color: Theme.pink
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    let ids = []
                                    for (const g of videoBrowser.unmatchedSeriesGroups)
                                        ids = ids.concat(g.episodeIds)
                                    for (const v of videoBrowser.unmatchedLoose)
                                        ids.push(v.id)
                                    LibraryService.requeueForMatch(ids)
                                }
                            }
                        }
                    }

                    // Whole shows — one Fix Match repairs every episode
                    Text {
                        visible: root.shownUnmatchedGroups.length > 0
                        text: qsTr("SERIES — one match fixes every episode")
                        font.pixelSize: 11
                        font.letterSpacing: 1
                        color: Theme.textDim
                        Layout.bottomMargin: Theme.spaceXs
                    }
                    Repeater {
                        model: root.shownUnmatchedGroups
                        delegate: UnmatchedRow {
                            rows: modelData.episodeIds.map(id => ({id: id}))
                            label: modelData.name
                            group: true
                            id: unmatchedGroup
                            required property var modelData
                            Layout.fillWidth: true

                            Rectangle {
                                Layout.preferredWidth: Theme.spaceLg
                                Layout.preferredHeight: Theme.spaceXxl
                                Layout.leftMargin: Theme.spaceMd
                                radius: Theme.radiusSm
                                color: Theme.artworkPlaceholder
                            }
                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.spaceSm
                                Layout.bottomMargin: Theme.spaceSm
                                Text {
                                    text: unmatchedGroup.modelData.name
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: (unmatchedGroup.modelData.episodeCount === 1
                                             ? qsTr("%1 episode") : qsTr("%1 episodes"))
                                            .arg(unmatchedGroup.modelData.episodeCount)
                                    font.pixelSize: 11
                                    color: Theme.textDim
                                }
                            }
                            Text {
                                text: qsTr("Customize…")
                                font.pixelSize: 12
                                color: Theme.pink
                                Layout.rightMargin: Theme.spaceMd
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.customizeVideo(
                                        {id: unmatchedGroup.modelData.episodeIds[0]},
                                        unmatchedGroup.modelData.episodeIds)
                                }
                            }
                            SplitAddButton {
                                visible: DeviceService.connected
                                Layout.rightMargin: Theme.spaceMd
                                showPick: false
                                onAddAll: root.queueEpisodes(unmatchedGroup.rows)
                            }
                        }
                    }

                    // Loose items
                    Text {
                        visible: root.shownUnmatchedLoose.length > 0
                        text: qsTr("INDIVIDUAL ITEMS")
                        font.pixelSize: 11
                        font.letterSpacing: 1
                        color: Theme.textDim
                        Layout.topMargin: Theme.spaceMd
                        Layout.bottomMargin: Theme.spaceXs
                    }
                    Repeater {
                        model: root.shownUnmatchedLoose
                        delegate: UnmatchedRow {
                            rows: [modelData]
                            label: modelData.filename
                            id: looseRow
                            required property var modelData
                            Layout.fillWidth: true

                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Layout.leftMargin: Theme.spaceMd
                                Layout.topMargin: Theme.spaceSm
                                Layout.bottomMargin: Theme.spaceSm
                                Text {
                                    text: looseRow.modelData.filename
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: qsTr("searched as: %1").arg(looseRow.modelData.cleanedQuery)
                                    font.pixelSize: 11
                                    color: Theme.textDim
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            // The honest exit for things TMDB will never
                            // know: file it under Other and stop matching.
                            Text {
                                text: qsTr("file as other")
                                font.pixelSize: 12
                                color: Theme.textSecondary
                                Layout.rightMargin: Theme.spaceXs
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: LibraryService.setVideoCategory(
                                        looseRow.modelData.id, "other")
                                }
                            }
                            Text {
                                text: qsTr("Customize…")
                                font.pixelSize: 12
                                color: Theme.pink
                                Layout.rightMargin: Theme.spaceXs
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.customizeVideo(looseRow.modelData)
                                }
                            }
                            Text {
                                text: "✕"
                                font.pixelSize: 12
                                color: Theme.textDim
                                Layout.rightMargin: Theme.spaceMd
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: LibraryService.deleteVideo(
                                        looseRow.modelData.id)
                                }
                            }
                            SplitAddButton {
                                visible: DeviceService.connected
                                Layout.rightMargin: Theme.spaceMd
                                showPick: false
                                onAddAll: looseRow.modelData.isTV ? root.queueEpisode(looseRow.modelData) : root.queueMovie(looseRow.modelData)
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Quick picker (▾ on series cards) ──
    property var _pickerEpisodes: []
    QuickPicker {
        id: episodePicker
        actionWord: "queue"
        onPicked: ids => {
            const set = {}
            for (const i of ids) set[i] = true
            root.queueEpisodes(
                root._pickerEpisodes.filter(e => set[String(e.id)]))
        }
    }

    // ── Context menus ──
    ZuneMenu {
        id: movieMenu
        objectName: "videoMovieMenu"
        property var target: null
        // Canonical order: Play · Queue · type-specific · Delete
        ZuneMenuItem {
            text: qsTr("Play")
            onTriggered: root.playMovie(movieMenu.target)
        }
        ZuneMenuItem {
            text: qsTr("Queue for Zune")
            visible: DeviceService.connected
            height: visible ? implicitHeight : 0
            onTriggered: root.queueMovie(movieMenu.target)
        }
        ZuneMenuItem {
            text: qsTr("Customize…")
            onTriggered: root.customizeVideo(movieMenu.target, root.itemIds(movieMenu.target))
        }
        ZuneMenuItem {
            text: qsTr("Convert to TV Show")
            onTriggered: LibraryService.reclassifyVideos(
                movieMenu.target.versionIds, "tv")
        }
        // Filing moves (grid study 2026-09-06): each entry shows only
        // where it changes something. Height-guarded — an invisible
        // MenuItem otherwise leaves its gap in the menu.
        ZuneMenuItem {
            readonly property bool shown:
                (movieMenu.target?.category ?? "") !== "music_video"
            visible: shown
            height: shown ? implicitHeight : 0
            text: qsTr("Move to Music Videos")
            onTriggered: LibraryService.reclassifyVideos(
                movieMenu.target.versionIds, "music_video")
        }
        ZuneMenuItem {
            readonly property bool shown:
                (movieMenu.target?.category ?? "") !== "other"
            visible: shown
            height: shown ? implicitHeight : 0
            text: qsTr("Move to Other")
            onTriggered: LibraryService.reclassifyVideos(
                movieMenu.target.versionIds, "other")
        }
        ZuneMenuItem {
            readonly property bool shown:
                (movieMenu.target?.category ?? "") === "music_video"
                || (movieMenu.target?.category ?? "") === "other"
            visible: shown
            height: shown ? implicitHeight : 0
            text: qsTr("Move to Movies")
            onTriggered: LibraryService.reclassifyVideos(
                movieMenu.target.versionIds, "movie")
        }
        ZuneMenuItem {
            text: qsTr("Delete from Library")
            danger: true
            onTriggered: LibraryService.deleteVideos(movieMenu.target.versionIds)
        }
    }

    ZuneMenu {
        id: seriesMenu
        property var target: null
        ZuneMenuItem {
            text: qsTr("Play")
            onTriggered: root.playSeries(seriesMenu.target.key)
        }
        ZuneMenuItem {
            text: qsTr("Queue All Episodes for Zune")
            visible: DeviceService.connected
            height: visible ? implicitHeight : 0
            onTriggered: root.queueSeries(seriesMenu.target.key)
        }
        ZuneMenuItem {
            text: qsTr("Customize…")
            onTriggered: root.customizeSeries(seriesMenu.target.key)
        }
        ZuneMenuItem {
            text: qsTr("Delete Series")
            danger: true
            onTriggered: LibraryService.deleteVideos(
                videoBrowser.seriesEpisodeIds(seriesMenu.target.key))
        }
    }

    ZuneMenu {
        id: unmatchedMenu
        objectName: "unmatchedMenu"
        property var rows: []
        property bool group: false
        ZuneMenuItem {
            text: qsTr("Play")
            onTriggered: {
                const rows = unmatchedMenu.rows.map(root.currentVideo).filter(v => v.id !== undefined)
                if (rows.length === 0) return
                if (unmatchedMenu.group || rows[0].isTV) root.playEpisodes(rows, 0)
                else root.playMovie(rows[0])
            }
        }
        ZuneMenuItem {
            text: qsTr("Queue for Zune")
            visible: DeviceService.connected
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (!DeviceService.connected) return
                const rows = unmatchedMenu.rows.map(root.currentVideo).filter(v => v.id !== undefined)
                root.toastAddResult(SyncEngine.addVideos(rows.map(v => v.isTV
                    ? root.episodeSyncItem(v) : root.movieSyncItem(v))), "video")
            }
        }
        ZuneMenuItem {
            text: qsTr("Customize…")
            onTriggered: {
                const ids = unmatchedMenu.rows.map(v => v.id)
                if (ids.length > 0) root.customizeVideo({id: ids[0]}, ids)
            }
        }
        ZuneMenuItem {
            text: qsTr("Delete from Library")
            danger: true
            onTriggered: LibraryService.deleteVideos(unmatchedMenu.rows.map(v => v.id))
        }
    }

    CustomizeSheet {
        id: customizeSheet
        onSaved: message => toastHost.show(message)
    }
}
