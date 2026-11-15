import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Zuuned

// What one track actually is, as opposed to what the library or the player says
// it is.  A library row is a tag read once at scan time and the player's
// property sheet is a claim the player made when it ingested the file; the two
// disagree more often than the old UI admitted, because nothing here had read
// the bytes.  This sheet does, and shows both answers side by side when they
// differ.  It has no Save button on purpose: there is nothing here to edit, and
// a control that could not change a measurement would misrepresent what the
// sheet is.
//
// A local row reads the file on this machine.  A device row reads the front of
// the object off the player, because a container header names its codec inside
// the first few kilobytes and a whole 40 MB track is not worth a minute of a
// browsing worker to answer one question - so the sheet also says how much of
// the object it actually looked at.  Where even that read is impossible the
// reason is shown rather than quietly replaced by the player's own claim.
Popup {
    id: sheet

    // Which row this is answering.  Exactly one of these is set by openFor().
    property double libraryId: -1
    property double itemId: 0
    property var current: ({})
    property var details: ({})
    property string errorMessage: ""
    // Held rather than derived, so a row with nothing to read reports that
    // immediately instead of spinning at a question that was never asked.
    property bool waiting: false

    readonly property bool forDevice: itemId > 0
    readonly property var rows: buildRows(details, current, forDevice)
    readonly property var claims: details.properties || []
    // Both sources may have answered nothing at all, which is when the error
    // line is the whole sheet and must not be sharing it with empty rows.
    readonly property bool bare: rows.length === 0

    function openFor(track) {
        const t = track || ({})
        current = Object.assign({}, t)
        details = ({})
        errorMessage = ""
        if (t.libraryId !== undefined && t.libraryId >= 0) {
            libraryId = t.libraryId
            itemId = 0
            waiting = true
            LibraryService.requestTrackDetails(libraryId)
        } else if (t.itemId !== undefined && t.itemId > 0) {
            libraryId = -1
            itemId = t.itemId
            waiting = true
            DeviceService.requestObjectDetails(itemId)
        } else {
            libraryId = -1
            itemId = 0
            waiting = false
            errorMessage = qsTr("This row has no file to read.")
        }
        bodyScroll.contentY = 0
        open()
    }

    function adopt(answer, id) {
        // A sheet can be moved to another row while the first question is still
        // in flight, so an answer only counts if it names this row.
        if (id !== undefined && id !== itemId)
            return
        details = answer || ({})
        waiting = false
        errorMessage = details.ok === false && details.error
            ? String(details.error)
            : (details.probed === true ? "" : qsTr("Nothing in these bytes could be read as audio."))
    }

    function fmtClock(ms) {
        const total = Math.round(Number(ms) / 1000)
        if (!(total > 0)) return ""
        const m = Math.floor(total / 60)
        const s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    function fmtBytes(n) {
        const v = Number(n)
        if (!(v > 0)) return ""
        if (v >= 1048576) return (v / 1048576).toFixed(1) + " MB"
        if (v >= 1024) return Math.round(v / 1024) + " KB"
        return Math.round(v) + " B"
    }

    function fmtRate(bps) {
        const v = Number(bps)
        if (!(v > 0)) return ""
        return (v / 1000).toFixed(v >= 100000 ? 0 : 1) + " kb/s"
    }

    function fmtHz(hz) {
        const v = Number(hz)
        if (!(v > 0)) return ""
        const k = v / 1000
        return (Math.abs(k - Math.round(k)) < 0.0005 ? Math.round(k) : k.toFixed(3)) + " kHz"
    }

    function fmtChannels(n) {
        const v = Number(n)
        if (!(v > 0)) return ""
        if (v === 1) return qsTr("1 (mono)")
        if (v === 2) return qsTr("2 (stereo)")
        return String(v)
    }

    // A missing key stays a missing key.  0 and "unknown" are different
    // sentences and only one of them is a measurement.
    function put(rows, label, value) {
        if (value === undefined || value === null) return
        const text = String(value)
        if (text === "") return
        rows.push({ label: label, value: text })
    }

    function buildRows(d, c, device) {
        const rows = []
        if (d.probed === true) {
            rows.push({ header: qsTr("What the file says") })
            put(rows, qsTr("Container"), d.format)
            put(rows, qsTr("Codec"), d.codec
                + (d.lossless === true ? " - " + qsTr("lossless") : ""))
            put(rows, qsTr("Bit rate"), fmtRate(d.bitRate))
            put(rows, qsTr("Sample rate"), fmtHz(d.sampleRate))
            put(rows, qsTr("Channels"), fmtChannels(d.channels))
            put(rows, qsTr("Bit depth"), Number(d.bitsPerSample) > 0
                ? d.bitsPerSample + "-bit" : "")
            // A capped read cannot know the length: FFmpeg times what it was
            // given, so a 512 KB front would report a few seconds of a
            // three-minute song.  Only a whole file is believed here, and the
            // player's own number answers for the objects in front of it.
            put(rows, qsTr("Duration"), fmtClock(
                device && d.partial === true
                    ? (d.playerDurationMs || d.durationMs)
                    : (d.durationMs || d.playerDurationMs)))
        }
        rows.push({ header: device ? qsTr("On the player") : qsTr("On this PC") })
        put(rows, qsTr("Size"), fmtBytes(d.size))
        if (device) {
            put(rows, qsTr("Object"), d.objectId)
            put(rows, qsTr("Object name"), d.objectName)
            put(rows, qsTr("Player calls it"), d.playerFormat)
            put(rows, qsTr("Player duration"), fmtClock(d.playerDurationMs))
            if (d.partial === true)
                put(rows, qsTr("Read from the player"),
                    fmtBytes(d.readBytes) + " / " + fmtBytes(d.size))
        } else {
            put(rows, qsTr("File"), d.filepath
                ? ZPathText.display(String(d.filepath)) : "")
        }
        return rows
    }

    width: Math.min(Theme.customizeCompactAt,
                    (Overlay.overlay ? Overlay.overlay.width : Theme.customizeCompactAt) - Theme.spaceXxxl)
    height: Math.min(Theme.customizeHeight - Theme.spaceHuge,
                     (Overlay.overlay ? Overlay.overlay.height : Theme.customizeHeight) - Theme.spaceXxxl)
    modal: true
    padding: 0
    anchors.centerIn: Overlay.overlay
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Qt.alpha(Theme.bg, 0.7) }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionBase }
    }
    exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: Theme.motionFast } }

    background: Rectangle {
        color: Theme.customizeSurface
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.customizeSurface }
            GradientStop { position: 1; color: Theme.surfaceBg }
        }
        radius: Theme.radiusLg
        border.width: 1
        border.color: Qt.alpha(Theme.pink, 0.28)
    }

    // Both roads answer here and neither may be swallowed: the sheet is the
    // only thing that can stop saying "reading", and a lost answer would leave
    // it saying that forever.
    Connections {
        target: LibraryService
        function onTrackDetailsReady(details) {
            if (sheet.libraryId < 0 || details.libraryId === undefined) return
            if (Number(details.libraryId) !== sheet.libraryId) return
            sheet.adopt(details)
        }
    }
    Connections {
        target: DeviceService
        function onObjectDetailsReady(itemId, details) {
            if (!sheet.forDevice) return
            sheet.adopt(details, Number(itemId))
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXxxl
            Layout.rightMargin: Theme.spaceLg
            Layout.topMargin: Theme.spaceXxl
            Layout.bottomMargin: Theme.spaceLg
            spacing: Theme.spaceLg

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Text {
                    text: qsTr("FILE DETAILS")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.customizeLabelSize
                    font.letterSpacing: 1.6
                }
                Text {
                    Layout.fillWidth: true
                    text: String(sheet.current.title || "") !== ""
                          ? sheet.current.title : qsTr("Untitled")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.customizeHeadingSize
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: [sheet.current.artist || "", sheet.current.album || ""]
                          .filter(function (p) { return p !== "" }).join(" - ")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.customizeBodySize
                    elide: Text.ElideRight
                }
            }

            Button {
                id: closeButton
                Layout.alignment: Qt.AlignTop
                implicitHeight: Theme.spaceHuge - Theme.spaceSm
                implicitWidth: implicitHeight
                flat: true
                hoverEnabled: true
                text: "×"
                font.pixelSize: Theme.customizeHeadingSize
                Accessible.name: qsTr("Close file details")
                contentItem: Text {
                    text: closeButton.text
                    color: closeButton.hovered ? Theme.textPrimary : Theme.textSecondary
                    font: closeButton.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radiusSm
                    color: closeButton.hovered ? Theme.cardHover : Theme.transparent
                }
                onClicked: sheet.close()
            }
        }

        Flickable {
            id: bodyScroll
            objectName: "trackDetailsBody"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spaceXxxl
            Layout.rightMargin: Theme.spaceXxxl
            Layout.bottomMargin: Theme.spaceXl
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ZuneScrollBar {}

            ColumnLayout {
                id: body
                width: bodyScroll.width - Theme.spaceMd
                spacing: Theme.spaceXxs

                Text {
                    Layout.fillWidth: true
                    visible: sheet.waiting
                    text: qsTr("Reading…")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.customizeBodySize
                    font.italic: true
                }

                Text {
                    Layout.fillWidth: true
                    visible: !sheet.waiting && sheet.errorMessage !== ""
                    text: sheet.errorMessage
                    color: Theme.warning
                    font.pixelSize: Theme.customizeBodySize
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: sheet.visible ? sheet.rows : []
                    delegate: ColumnLayout {
                        id: detailRow
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.topMargin: detailRow.modelData.header !== undefined
                                          ? Theme.spaceMd : 0
                        spacing: Theme.spaceXxs

                        Text {
                            Layout.fillWidth: true
                            visible: detailRow.modelData.header !== undefined
                            text: detailRow.modelData.header !== undefined
                                  ? detailRow.modelData.header : ""
                            color: Theme.textSubtle
                            font.pixelSize: Theme.customizeLabelSize
                            font.letterSpacing: 1.4
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: detailRow.modelData.header === undefined
                            spacing: Theme.spaceMd

                            Text {
                                Layout.preferredWidth: 132
                                text: detailRow.modelData.label !== undefined
                                      ? detailRow.modelData.label : ""
                                color: Theme.textSecondary
                                font.pixelSize: Theme.customizeBodySize
                                font.weight: Font.Light
                            }
                            Text {
                                Layout.fillWidth: true
                                text: detailRow.modelData.value !== undefined
                                      ? detailRow.modelData.value : ""
                                color: Theme.textPrimary
                                font.pixelSize: Theme.customizeBodySize
                                wrapMode: Text.WrapAnywhere
                                horizontalAlignment: Text.AlignLeft
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            visible: detailRow.modelData.header === undefined
                            height: Theme.hairline
                            color: Theme.separator
                        }
                    }
                }

                // The player's own words, verbatim.  They are not summarised
                // because the reason this sheet exists is that the claim and the
                // bytes are not always the same sentence, and a reader who
                // cannot see the claim cannot see the disagreement.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spaceMd
                    spacing: Theme.spaceXxs
                    visible: !sheet.waiting && sheet.claims.length > 0

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("What the player claims")
                        color: Theme.textSubtle
                        font.pixelSize: Theme.customizeLabelSize
                        font.letterSpacing: 1.4
                    }
                    Repeater {
                        model: sheet.visible && sheet.claims.length > 0 ? sheet.claims : []
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            text: String(modelData)
                            color: Theme.textMid
                            font.pixelSize: Theme.customizeCaptionSize
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                }
            }
        }
    }
}