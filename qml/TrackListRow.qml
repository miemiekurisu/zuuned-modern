import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

// Port of MusicRows.swift TrackRow — one shared row for every track list.
// Columns toggle per context (songs / albumTracks / artistTracks), the
// artist/album cells become links when a handler is wired, hover tints
// the row pink, and a 1px separator underlines it.
Rectangle {
    id: row

    // {title, artist, album, durationMs, trackNumber, itemId}
    required property var track

    property bool showNumber: false
    property int displayTrackNumber: -1 // playlist position is presentation, never identity
    property bool showArt: false
    // Local library rows key art by (albumartist, album, filepath)
    property bool localArt: false
    property bool showArtist: true
    property bool showAlbum: true
    property bool artistLink: false
    property bool albumLink: false
    property bool isNowPlaying: false
    // Enables the right-click "Add to Sync Queue" menu (local rows only)
    property bool syncable: false
    // Library rows with a libraryId get the Add to Playlist submenu
    readonly property bool playlistable:
        syncable && track.libraryId !== undefined && track.libraryId >= 0
    readonly property bool playable: syncable && !!track.filepath
    readonly property bool canAdd: syncable && (TrayState.building || DeviceService.connected)
    // Device rows: expose Delete from Device in the context menu
    property bool deletable: false
    // Device rows: expose Save to Library (track download) in the menu
    property bool saveable: false
    readonly property bool onComputer: {
        if (!saveable) return false
        // Read the revision so scan/import/edit/delete changes repaint the
        // badge; the native lookup is constant-time, even in large libraries.
        const revision = LibraryService.localTrackIdentityRevision
        const deviceRevision = DeviceService.musicIdentityRevision
        return LibraryService.hasLocalTrack(track.artist || "", track.album || "", track.title || "",
            track.discNumber || 0, track.trackNumber || 0,
            track.discNumberReliable !== false, track.trackNumberReliable !== false,
            DeviceService.musicIdentityPeers(track))
    }

    signal artistTapped()
    signal albumTapped()
    signal played()
    signal syncRequested()
    signal deleteRequested()
    signal saveRequested()
    signal editRequested()      // UX-2 Edit Info (library rows)
    signal detailsRequested()   // what the bytes actually are (both row kinds)

    width: ListView.view ? ListView.view.width : 400
    height: 34
    color: rowArea.containsMouse || artistArea.containsMouse || albumArea.containsMouse
           ? Theme.rowHoverPink : "transparent"

    function fmtDuration(ms) {
        if (!ms || ms <= 0) return ""
        const total = Math.floor(ms / 1000)
        const m = Math.floor(total / 60)
        const s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function playbackItem() {
        return { filepath: track.filepath || "", title: track.title || "",
            artist: track.artist || "", album: track.album || "",
            albumartist: track.albumartist || track.artist || "",
            libraryId: track.libraryId !== undefined ? track.libraryId : -1 }
    }

    MouseArea {
        id: rowArea
        objectName: "trackDragArea"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onDoubleClicked: row.played()
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton
                && (row.syncable || row.deletable || row.saveable))
                contextMenu.popup()
        }
        // All media dragging requires a Zune. Playlist menu actions
        // remain available offline; preventStealing preserves the drag
        // gesture inside a scrolling list once the device is connected.
        readonly property bool dragArmed:
            DeviceService.connected && (row.saveable || row.playlistable)
        drag.target: dragArmed ? dragGhost : null
        preventStealing: dragArmed
        onPressed: mouse => {
            const p = rowArea.mapToItem(Overlay.overlay, mouse.x, mouse.y)
            dragGhost.x = p.x + 8
            dragGhost.y = p.y + 8
        }
        onReleased: {
            if (dragGhost.Drag.active)
                dragGhost.Drag.drop()
        }
    }

    // Drag ghost — a small card with a little drop shadow riding the
    // cursor (design contract). Lives on the window overlay so it
    // floats above every island.
    Item {
        id: dragGhost
        parent: Overlay.overlay ?? row
        width: ghostPill.width
        height: ghostPill.height
        visible: Drag.active
        Drag.active: rowArea.drag.active && DeviceService.connected
        // Device rows carry a distinct key so they land ONLY on the
        // library sidebar (pull-to-library), never back on the device
        // panel's sync queue.
        Drag.keys: row.saveable ? ["zuuned-device-tracks"] : ["zuuned-tracks"]
        Drag.hotSpot.x: 10
        Drag.hotSpot.y: 10
        Drag.onActiveChanged: Drag.active ? TrayState.dragStarted()
                                          : TrayState.dragEnded()
        property var dragTracks: [{
            filepath: row.track.filepath || "",
            title: row.track.title || "",
            artist: row.track.artist || "",
            albumartist: (row.track.albumartist && row.track.albumartist.length > 0)
                         ? row.track.albumartist : (row.track.artist || ""),
            album: row.track.album || "", genre: row.track.genre || "",
            trackNumber: row.track.trackNumber || 0,
            discNumber: row.track.discNumber || 0,
            year: row.track.year || 0,
            trackNumberReliable: row.track.trackNumberReliable !== false,
            discNumberReliable: row.track.discNumberReliable !== false,
            durationMs: row.track.durationMs || 0,
            itemId: row.track.itemId || 0,
            libraryId: row.track.libraryId !== undefined ? row.track.libraryId : -1
        }]
        // Unified device→library payload the sidebar reads (device rows
        // carry a real itemId; library rows carry 0 and are ignored on
        // the library side anyway).
        property var dragPayload: dragTracks

        // The ghost is the track's album art with the title riding
        // under it — you're moving the SONG, not its words.
        Item {
            id: ghostPill
            width: 56
            height: 56 + ghostLabel.implicitHeight + 4
            rotation: 3

            AlbumArt {
                width: 56
                height: 56
                itemId: row.track.itemId || 0
                local: row.localArt
                artist: (row.track.albumartist && row.track.albumartist.length > 0)
                        ? row.track.albumartist : (row.track.artist || "")
                album: row.track.album || ""
                filepath: row.track.filepath || ""
                cornerRadius: Theme.radiusMd
            }
            Rectangle {
                width: 56
                height: 56
                radius: 4
                color: "transparent"
                border.width: 1
                border.color: Qt.alpha(Theme.orange, 0.75)
            }
            Text {
                id: ghostLabel
                anchors.top: parent.top
                anchors.topMargin: 58
                width: 76
                anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter
                text: row.track.title || ""
                font.pixelSize: 10
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
        }
        DropShadow {
            anchors.fill: ghostPill
            source: ghostPill
            radius: 16
            samples: 21
            verticalOffset: 7
            color: Qt.rgba(0, 0, 0, 0.6)
            rotation: ghostPill.rotation
            z: -1
        }
    }

    // Themed context menu (mac's track context menu — sync entry only
    // until copy/delete port)
    Menu {
        id: contextMenu
        objectName: "trackContextMenu"

        background: Rectangle {
            implicitWidth: 190
            color: Theme.surfaceBg
            border.width: 1
            border.color: Theme.glassBorder
            radius: Theme.radiusLg   // island-panel radius (house rule)
        }

        MenuItem {
            id: syncMenuItem
            objectName: "trackSyncAction"
            // While the playlist tray is building, the row's add feeds
            // the tray (UX-2) — say so.
            text: TrayState.building ? qsTr("Add to Playlist") : qsTr("Add to Sync Queue")
            implicitHeight: 30
            contentItem: Text {
                text: syncMenuItem.text
                font.pixelSize: 13
                font.weight: Font.Light
                color: !syncMenuItem.enabled ? Theme.textGhost
                    : syncMenuItem.highlighted ? Theme.textPrimary : Theme.textMid
                leftPadding: Theme.spaceSm
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: syncMenuItem.highlighted ? Theme.rowHoverPink : "transparent"
            }
            enabled: row.canAdd
            onTriggered: { if (row.canAdd) row.syncRequested() }
            visible: row.syncable
            height: row.syncable ? implicitHeight : 0
        }

        ZuneMenuItem {
            objectName: "trackPlayNextAction"
            text: qsTr("Play Next")
            visible: row.playable
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (row.playable) PlayerService.playNext([row.playbackItem()])
            }
        }
        ZuneMenuItem {
            objectName: "trackNowPlayingAction"
            text: qsTr("Add to Now Playing")
            visible: row.playable
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (row.playable) PlayerService.appendToQueue([row.playbackItem()])
            }
        }

        MenuItem {
            id: editMenuItem
            text: qsTr("Edit Info…")
            implicitHeight: 30
            visible: row.playlistable
            height: row.playlistable ? implicitHeight : 0
            contentItem: Text {
                text: editMenuItem.text
                font.pixelSize: 13
                font.weight: Font.Light
                color: editMenuItem.highlighted ? Theme.textPrimary : Theme.textMid
                leftPadding: Theme.spaceSm
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: editMenuItem.highlighted ? Theme.rowHoverPink : "transparent"
            }
            onTriggered: row.editRequested()
        }

        // One track, one lookup: the album this row belongs to, not the whole
        // library. Library rows only - a device track has no local file whose
        // cover could be found, and its album art already came off the player.
        MenuItem {
            id: lookupMenuItem
            text: qsTr("Search Cover Online")
            implicitHeight: 30
            visible: row.playlistable
            height: row.playlistable ? implicitHeight : 0
            contentItem: Text {
                text: lookupMenuItem.text
                font.pixelSize: 13
                font.weight: Font.Light
                color: lookupMenuItem.highlighted ? Theme.textPrimary : Theme.textMid
                leftPadding: Theme.spaceSm
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: lookupMenuItem.highlighted ? Theme.rowHoverPink : "transparent"
            }
            onTriggered: {
                const album = row.track.album || ""
                const artist = row.track.albumartist && row.track.albumartist.length > 0
                    ? row.track.albumartist : (row.track.artist || "")
                if (album === "" || artist === "") return
                LibraryService.retryArtwork("album", { artist: artist, album: album })
            }
        }

        MenuItem {
            id: saveMenuItem
            objectName: "trackSaveAction"
            text: qsTr("Save to Library")
            implicitHeight: 30
            visible: row.saveable
            enabled: DeviceService.connected
            height: row.saveable ? implicitHeight : 0
            contentItem: Text {
                text: saveMenuItem.text
                font.pixelSize: 13
                font.weight: Font.Light
                color: !saveMenuItem.enabled ? Theme.textGhost
                    : saveMenuItem.highlighted ? Theme.textPrimary : Theme.textMid
                leftPadding: Theme.spaceSm
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: saveMenuItem.highlighted ? Theme.rowHoverPink : "transparent"
            }
            onTriggered: { if (DeviceService.connected) row.saveRequested() }
        }

        MenuItem {
            id: deleteMenuItem
            objectName: "trackDeleteAction"
            text: qsTr("Delete from Device")
            implicitHeight: 30
            visible: row.deletable
            enabled: DeviceService.connected
            height: row.deletable ? implicitHeight : 0
            contentItem: Text {
                text: deleteMenuItem.text
                font.pixelSize: 13
                font.weight: Font.Light
                color: !deleteMenuItem.enabled ? Theme.textGhost
                    : deleteMenuItem.highlighted ? Theme.error : Theme.textMid
                leftPadding: Theme.spaceSm
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: deleteMenuItem.highlighted ? Theme.rowHoverPink : "transparent"
            }
            onTriggered: { if (DeviceService.connected) row.deleteRequested() }
        }

        // The one question neither a library row nor a player's property sheet
        // can answer: what the bytes actually are.  Both row kinds get it,
        // because a scan-time tag read and a claim the player made on ingest
        // are both second-hand, and this is the only entry that goes and looks.
        ZuneMenuItem {
            objectName: "trackDetailsAction"
            text: qsTr("File Details…")
            visible: row.syncable || row.deletable || row.saveable
            height: visible ? implicitHeight : 0
            onTriggered: row.detailsRequested()
        }
    }

    // UX-2: Add to Playlist ▸ — inserted right after the sync entry on
    // library rows (submenus can't hide via visible/height like items,
    // so it's attached conditionally instead).
    AddToPlaylistMenu {
        id: plSubmenu
        onPicked: (playlistId, name) => {
            LibraryService.addToPlaylist(playlistId, [row.track.libraryId])
            toastHost.show("added to '" + name + "'")
        }
        onNewRequested: {
            TrayState.openNew()
            TrayState.addTracks([{
                id: row.track.libraryId, title: row.track.title || "",
                artist: row.track.artist || "", album: row.track.album || "",
                durationMs: row.track.durationMs || 0,
                filepath: row.track.filepath || ""
            }])
        }
    }
    Component.onCompleted: {
        if (row.playlistable)
            contextMenu.insertMenu(1, plSubmenu)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceMd
        spacing: 0

        Item {
            visible: row.showArt
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.rightMargin: Theme.spaceMd

            AlbumArt {
                anchors.fill: parent
                itemId: row.track.itemId || 0
                local: row.localArt
                artist: (row.track.albumartist && row.track.albumartist.length > 0)
                        ? row.track.albumartist : (row.track.artist || "")
                album: row.track.album || ""
                filepath: row.track.filepath || ""
                cornerRadius: 3
            }

            // Already on the connected Zune (mac OnDeviceBadge)
            OnDeviceBadge {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -3
                size: 18
                visible: {
                    if (!row.localArt || !DeviceService.connected) return false
                    const deviceRevision = DeviceService.musicIdentityRevision
                    const libraryRevision = LibraryService.localTrackIdentityRevision
                    return DeviceService.hasDeviceTrack(row.track, LibraryService.musicIdentityPeers(row.track))
                }
            }
        }

        Text {
            visible: row.showNumber
            Layout.preferredWidth: 30
            Layout.rightMargin: Theme.spaceMd
            // Keel ZMDB track numbers are garbage (2297 on every row) —
            // show only plausible values, mac's "–" for the rest.
            text: row.displayTrackNumber >= 0 ? row.displayTrackNumber
                : row.track.trackNumber > 0 && row.track.trackNumber <= 99 ? row.track.trackNumber : "–"
            font.pixelSize: 12
            font.weight: Font.Light
            font.family: "monospace"
            color: Theme.textDim
            horizontalAlignment: Text.AlignRight
        }

        Text {
            visible: row.isNowPlaying
            Layout.rightMargin: Theme.spaceXs
            text: "▶"
            font.pixelSize: 10
            color: Theme.pink
        }

        Text {
            Layout.minimumWidth: 200
            Layout.preferredWidth: row.width * 0.34
            text: row.track.title
            font.pixelSize: 14
            font.weight: Font.Light
            color: row.isNowPlaying ? Theme.pink : Theme.textMid
            elide: Text.ElideRight
        }

        Item {
            visible: row.showArtist
            Layout.minimumWidth: 150
            Layout.preferredWidth: row.width * 0.22
            Layout.fillHeight: true

            Text {
                id: artistText
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width)
                text: row.track.artist
                font.pixelSize: 14
                font.weight: Font.Light
                font.underline: row.artistLink && artistArea.containsMouse
                color: row.artistLink && artistArea.containsMouse
                       ? Theme.pink : Theme.textSecondary
                elide: Text.ElideRight
            }
            MouseArea {
                id: artistArea
                anchors.fill: artistText
                enabled: row.artistLink
                hoverEnabled: row.artistLink
                cursorShape: row.artistLink ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: row.artistTapped()
            }
        }

        Item {
            visible: row.showAlbum
            Layout.minimumWidth: 150
            Layout.preferredWidth: row.width * 0.22
            Layout.fillHeight: true

            Text {
                id: albumText
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width)
                text: row.track.album
                font.pixelSize: 14
                font.weight: Font.Light
                font.underline: row.albumLink && albumArea.containsMouse
                color: row.albumLink && albumArea.containsMouse
                       ? Theme.pink : Theme.textDim
                elide: Text.ElideRight
            }
            MouseArea {
                id: albumArea
                anchors.fill: albumText
                enabled: row.albumLink
                hoverEnabled: row.albumLink
                cursorShape: row.albumLink ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: row.albumTapped()
            }
        }

        Item { Layout.fillWidth: true }

        Item {
            visible: row.saveable
            Layout.preferredWidth: Theme.locationRowBadgeSize + Theme.spaceSm
            Layout.preferredHeight: Theme.locationRowBadgeSize
            // Reserve the column for all device rows so status changes do
            // not move the duration. It also appears in drills without art.
            OnComputerBadge {
                objectName: "trackComputerBadge"
                size: Theme.locationRowBadgeSize
                visible: row.onComputer
            }
        }

        Text {
            Layout.preferredWidth: 60
            text: row.fmtDuration(row.track.durationMs)
            font.pixelSize: 12
            font.weight: Font.Light
            color: Theme.textDim
            horizontalAlignment: Text.AlignRight
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.separator
    }
}
