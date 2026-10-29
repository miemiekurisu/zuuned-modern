import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

// Settings — the W15 mockboard's five sections (Appearance / Library /
// Device / Mixtapes / Health) under the app's PivotBar, flowing Sblk
// sections with segmented-pill (Seg) controls per the Zuuned First-Run
// mock. Deliberate deviations from the mock: PivotBar instead of the
// sidebar nav (house style), and a "Preferred audio language" section
// the mock omits (transcode needs it).
Item {
    id: root

    function typeColor(t) {
        if (t === "music") return Theme.pink
        if (t === "movies") return Theme.orange
        if (t === "tv") return Theme.info
        if (t === "anime") return Theme.purple
        if (t === "photos") return Theme.green
        return Theme.textSecondary
    }

    // The row chip and the download-status suffix have to say the same thing
    // the type pills at the bottom of the section say, and every one of those
    // words lives in the language file rather than here.
    function typeLabel(t) {
        if (t === "music") return qsTr("music")
        if (t === "movies") return qsTr("movies")
        if (t === "tv") return qsTr("tv")
        if (t === "anime") return qsTr("anime")
        if (t === "photos") return qsTr("photos")
        // Two vocabularies meet here.  A watch folder's own type is
        // music | movies | tv | anime | photos | all (src/library/LibraryTypes.h),
        // while the import mark a row carries names a kind -- music | photos |
        // video -- because movies, tv and anime all share one video
        // destination.  Both are keys, so both take their words from the
        // language file.  A key nobody recognised is echoed as it arrived: that
        // is a fact about the data, not a word this file may invent.
        if (t === "all") return qsTr("all")
        if (t === "video") return qsTr("video")
        return t
    }

    function pickFolder(t) {
        mediaFolderDialog.pendingType = t
        mediaFolderDialog.open()
    }

    // ═══════════ Reusable pieces ═══════════

    component SectionTitle: Text {
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 3
        color: Theme.textDim
    }

    component CaptionText: Text {
        font.pixelSize: 12
        font.weight: Font.Light
        color: Theme.textDim
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.maximumWidth: 760
    }

    component PhaseNote: Text {
        font.pixelSize: 12
        font.weight: Font.Light
        color: Theme.textGhost
        // Every one of these is prose sitting inside an Sblk, so it keeps
        // the same measure as the Sblk desc above it. Left unwrapped it
        // reports its whole sentence as implicitWidth, and that is what
        // made Settings > Device 2263 logical px wide.
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.maximumWidth: 760
    }

    component GlassCard: Rectangle {
        default property alias content: cardColumn.data
        Layout.fillWidth: true
        implicitHeight: cardColumn.implicitHeight + Theme.spaceLg * 2
        radius: Theme.radiusLg
        color: Theme.glassBg
        border.color: Theme.glassBorder
        border.width: 1

        ColumnLayout {
            id: cardColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceLg
            spacing: Theme.spaceMd
        }
    }

    component ZSlider: Slider {
        id: zs
        implicitWidth: 200
        implicitHeight: 20
        background: Rectangle {
            x: zs.leftPadding
            y: zs.topPadding + zs.availableHeight / 2 - height / 2
            width: zs.availableWidth
            height: 4
            radius: 2
            color: Theme.cardHover
            Rectangle {
                width: zs.visualPosition * parent.width
                height: parent.height
                radius: 2
                color: Theme.pink
            }
        }
        handle: Rectangle {
            x: zs.leftPadding + zs.visualPosition * (zs.availableWidth - width)
            y: zs.topPadding + zs.availableHeight / 2 - height / 2
            width: 14; height: 14; radius: 7
            color: zs.pressed ? Theme.activePink : Theme.pink
        }
    }

    // Mock "sblk": a flowing section — title + description + its control(s).
    component Sblk: ColumnLayout {
        property string title: ""
        property string desc: ""
        property bool divider: true
        default property alias content: sblkBody.data
        Layout.fillWidth: true
        spacing: Theme.spaceSm
        // Mock's sblk h3 — quiet 15px, not a shouting header
        Text {
            text: title
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.textPrimary
        }
        // Prose keeps a readable measure even in the wide frame
        Text {
            visible: desc.length > 0
            text: desc
            font.pixelSize: 13
            font.weight: Font.Light
            color: Theme.textDim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.maximumWidth: 760
        }
        ColumnLayout {
            id: sblkBody
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceMd
        }
        // Separator between sections
        Rectangle {
            visible: parent.divider
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceLg
            implicitHeight: 1
            color: Theme.separator
        }
    }

    // Mock "pills": one combined rounded-rectangle segmented selector,
    // pink underline on the active segment.
    component Seg: Rectangle {
        id: seg
        property var options: []        // [{ label, value }]
        property string current: ""
        signal picked(string value)
        implicitHeight: 38
        implicitWidth: segRow.implicitWidth
        radius: Theme.radiusMd
        color: Theme.glassBg
        border.width: 1
        border.color: Theme.glassBorder
        Row {
            id: segRow
            height: parent.height
            Repeater {
                model: seg.options
                delegate: Item {
                    required property var modelData
                    required property int index
                    // { disabled: true } — visible but inert (e.g. AMD
                    // until VAAPI decode lands)
                    readonly property bool dim: modelData.disabled === true
                    width: segTxt.implicitWidth + Theme.spaceXl
                    height: seg.height
                    // Active segment — inset rounded thumb + pink underline
                    // (the app's convention), no loud border.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        radius: Theme.radiusSm
                        visible: seg.current === modelData.value
                        color: Theme.cardActive
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            anchors.bottomMargin: 3
                            height: 2
                            radius: 1
                            color: Theme.pink
                        }
                    }
                    Text {
                        id: segTxt
                        anchors.centerIn: parent
                        text: modelData.label
                        font.pixelSize: 13
                        font.weight: Font.Light
                        color: dim ? Theme.textGhost
                                   : seg.current === modelData.value
                                     ? Theme.textPrimary : Theme.textSecondary
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: dim ? Qt.ArrowCursor : Qt.PointingHandCursor
                        onClicked: if (!dim) seg.picked(modelData.value)
                    }
                }
            }
        }
    }

    // Toggle switch (mock ".toggle")
    component Sw: Rectangle {
        id: sw
        property bool on: false
        signal toggled()
        implicitWidth: 40
        implicitHeight: 22
        radius: 11
        color: on ? Qt.rgba(Theme.pink.r, Theme.pink.g, Theme.pink.b, 0.35)
                  : Theme.cardHover
        border.width: 1
        border.color: on ? Qt.rgba(Theme.pink.r, Theme.pink.g, Theme.pink.b, 0.5)
                         : Theme.glassBorder
        Behavior on color { ColorAnimation { duration: 120 } }
        Rectangle {
            width: 16; height: 16; radius: 8
            y: 2
            x: sw.on ? sw.width - width - 2 : 2
            color: "#ffffff"
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: sw.toggled()
        }
    }

    // Glass button (mock's rounded-rect controls: "export all (.m3u)",
    // "back up now", "show me") — the app convention, radius not a pill.
    // primary = the mock's ".btn.primary" pink tint; small = row-scale
    // ("type"/"remove" on folder rows).
    component GlassButton: Rectangle {
        id: gb
        property string text: ""
        property bool disabled: false
        property bool primary: false
        property bool small: false
        signal clicked()
        implicitHeight: small ? 26 : 34
        implicitWidth: gbTxt.implicitWidth + (small ? Theme.spaceLg : Theme.spaceXl)
        radius: Theme.radiusMd
        opacity: disabled ? 0.4 : 1.0
        color: primary
               ? Qt.rgba(Theme.pink.r, Theme.pink.g, Theme.pink.b,
                         gbArea.containsMouse && !disabled ? 0.22 : 0.14)
               : (gbArea.containsMouse && !disabled ? Theme.cardHover
                                                    : Theme.glassBg)
        border.width: 1
        border.color: primary
                      ? Qt.rgba(Theme.pink.r, Theme.pink.g, Theme.pink.b, 0.55)
                      : Theme.glassBorder
        Text {
            id: gbTxt
            anchors.centerIn: parent
            text: gb.text
            font.pixelSize: gb.small ? 12 : 13
            font.weight: Font.Light
            color: gb.primary ? "#ffffff" : Theme.textPrimary
        }
        MouseArea {
            id: gbArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: gb.disabled ? Qt.ArrowCursor : Qt.PointingHandCursor
            onClicked: if (!gb.disabled) gb.clicked()
        }
    }

    // A section row: label (fills) + right-aligned value/status, with the
    // mock's hairline under each row (`.statrow` border-bottom; set
    // divider: false on the last row of a group). Used by Health.
    // One provider API key, editable. Until 2026-10-24 both keys were
    // ini-only while the code that reads them shipped, which is the same
    // dishonesty as a control with no reader seen from the other side. Masked
    // by default -- a key is a secret you just typed in front of somebody --
    // with an explicit reveal, and it commits when the field loses focus.
    component ApiKeyField: RowLayout {
        id: keyField
        property string label: ""
        property string placeholder: ""
        property string saved: ""
        property bool revealed: false
        signal committed(string value)
        Layout.fillWidth: true
        spacing: Theme.spaceSm
        Text {
            Layout.preferredWidth: 82
            text: keyField.label
            font.pixelSize: 12
            font.weight: Font.Light
            color: Theme.textDim
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.maximumWidth: 420
            implicitHeight: 34
            radius: Theme.radiusMd
            color: Theme.cardBg
            border.color: Theme.borderLight
            border.width: 1
            TextField {
                id: keyEdit
                anchors.fill: parent
                leftPadding: Theme.spaceMd
                rightPadding: Theme.spaceLg
                background: null
                font.pixelSize: 13
                font.weight: Font.Light
                color: Theme.textPrimary
                placeholderText: keyField.placeholder
                placeholderTextColor: Theme.textGhost
                echoMode: keyField.revealed ? TextField.Normal : TextField.Password
                selectByMouse: true
                text: keyField.saved
                onActiveFocusChanged:
                    if (!activeFocus) keyField.committed(text.trim())
                onEditingFinished: keyField.committed(text.trim())
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                text: keyField.revealed ? qsTr("hide") : qsTr("show")
                font.pixelSize: 11
                color: Theme.textSecondary
                font.underline: keyHover.containsMouse
                MouseArea {
                    id: keyHover
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: keyField.revealed = !keyField.revealed
                }
            }
        }
        Text {
            Layout.preferredWidth: 58
            text: keyField.saved.length > 0 ? qsTr("saved") : qsTr("not set")
            font.pixelSize: 11
            color: keyField.saved.length > 0 ? Theme.pink : Theme.textGhost
        }
    }

    component StatRow: ColumnLayout {
        property string label: ""
        property string value: ""
        property color dot: "transparent"
        property color valueColor: Theme.textDim
        property bool divider: true
        Layout.fillWidth: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: Theme.spaceSm
            spacing: Theme.spaceMd
            Rectangle {
                visible: dot.a > 0
                Layout.alignment: Qt.AlignVCenter
                width: 8; height: 8; radius: 4
                color: dot
            }
            Text {
                Layout.fillWidth: true
                text: label
                font.pixelSize: 13; font.weight: Font.Light
                color: Theme.textPrimary
            }
            Text {
                text: value
                font.pixelSize: 12; font.weight: Font.Light
                color: valueColor
            }
        }
        Rectangle {
            visible: divider
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.separator
        }
    }

    // ═══════════ Page ═══════════

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PageHeaderBand {
            // The app-wide canonical spine — Settings' tabs sit at
            // the same x as every other section's
            Layout.leftMargin: Gallery.spine(root.width)
            Layout.rightMargin: Theme.spaceXl

            PivotBar {
                id: pivots
                Layout.fillWidth: true
                Layout.maximumWidth: 640
                Layout.alignment: Qt.AlignVCenter
                tabs: ["Appearance", "Library", "Device", "Mixtapes", "Health"]
                labels: [qsTr("Appearance"), qsTr("Library"), qsTr("Device"),
                         qsTr("Mixtapes"), qsTr("Health")]
            // QA harness (screenshot runs): `--settings-tab Device`
            Component.onCompleted: {
                const args = Qt.application.arguments
                const i = args.indexOf("--settings-tab")
                if (i >= 0 && i + 1 < args.length
                    && tabs.indexOf(args[i + 1]) >= 0)
                    current = args[i + 1]
            }
            }
            Item { Layout.fillWidth: true }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spaceLg
            contentHeight: stack.implicitHeight + Theme.spaceXxl
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ZuneScrollBar {}

            // A plain visibility stack, NOT a StackLayout. A StackLayout
            // sizes EVERY child to the widest child's implicit width and
            // overflows its own bounds doing so -- the same trap Main.qml
            // documents for the page stack -- and a Layout child cannot opt
            // out of it: `width: stack.width` and `implicitWidth: ...` are
            // both overwritten (measured with qml.exe against this Qt,
            // build/art6/stacktest.qml). On the Chinese Settings > Device
            // tab one unwrapped sentence made every tab 2263 px wide, so
            // the 这台 Zune values were painted at x~2200 -- off any screen
            // -- while their labels stayed put. Anchoring each tab to this
            // Item pins it to the frame the page actually has, in either
            // language and at any window width.
            Item {
                id: stack
                // Settings fills the page's standard frame like every
                // other section (2026-09-06: 760 felt too narrow next
                // to the rest); long prose stays readable via the
                // desc caps in Sblk/CaptionText.
                x: Gallery.spine(root.width)
                width: Math.min(Gallery.frameW(root.width),
                                flick.width - x - Theme.spaceXl)
                property int currentIndex: pivots.tabs.indexOf(pivots.current)
                // What the StackLayout used to report: the tallest tab, so
                // the scroll extent does not jump when the pivot changes.
                implicitHeight: Math.max(tabAppearance.implicitHeight,
                                         tabLibrary.implicitHeight,
                                         tabDevice.implicitHeight,
                                         tabMixtapes.implicitHeight,
                                         tabHealth.implicitHeight)
                height: implicitHeight

                // ── APPEARANCE ──
                ColumnLayout {
                    id: tabAppearance
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: stack.currentIndex === 0
                    spacing: Theme.spaceXl

                    // Language (2026-10-24): the page had no UI-language
                    // control at all -- the only row with "language" in it is
                    // Preferred audio language, which picks an audio *track*.
                    // The choice is read once at launch, so the card says so
                    // instead of pretending to repaint.
                    Sblk {
                        title: qsTr("Language")
                        desc: qsTr("which language zuuned writes its own interface in. Your files, tags and what the Zune itself displays are untouched.")
                        RowLayout {
                            spacing: Theme.spaceLg
                            Seg {
                                Layout.alignment: Qt.AlignVCenter
                                // The two language names stay in their own
                                // script on purpose: a menu that calls Chinese
                                // "中文" is findable whichever language
                                // you happen to read, in either direction.
                                options: [{ label: qsTr("Follow system"), value: "system" },
                                          { label: qsTr("English"), value: "en" },
                                          { label: "简体中文", value: "zh_CN" }]
                                current: AppSettings.uiLanguage
                                onPicked: (v) => AppSettings.uiLanguage = v
                            }
                        }
                        StatRow {
                            label: qsTr("Now showing")
                            value: DiagnosticsService.uiLanguage() === "zh_CN"
                                   ? "简体中文" : "English"
                        }
                        PhaseNote {
                            text: {
                                const wanted = AppSettings.uiLanguage === "system"
                                             ? DiagnosticsService.systemUiLanguage()
                                             : AppSettings.uiLanguage
                                if (wanted === DiagnosticsService.uiLanguage())
                                    return qsTr("Applied — this is what zuuned is showing right now.")
                                return qsTr("Restart zuuned to write its interface in %1 — the language is read once, at launch.")
                                       .arg(wanted === "zh_CN" ? "简体中文" : "English")
                        }
                        }
                        PhaseNote {
                            visible: DiagnosticsService.uiLanguage() === "zh_CN"
                            text: qsTr("Chinese covers the menus, pages and Settings of this build. Wording the language file does not carry yet stays English.")
                        }
                    }

                    Sblk {
                        title: qsTr("Now-playing player")
                        desc: qsTr("how music looks while it spins — everywhere: now-playing, ribbon, mini-player, and video.")
                        RowLayout {
                            spacing: Theme.spaceLg
                            Item {
                                Layout.preferredWidth: 96
                                Layout.preferredHeight: 96
                                clip: true   // contain the seek-head glow — no bleed into the header
                                Loader {
                                    anchors.centerIn: parent
                                    sourceComponent: Prefs.playerStyle === "vinyl" ? sVinyl : sDisc
                                }
                                Component {
                                    id: sDisc
                                    SpinningDiscView {
                                        player: PlayerService
                                        diameter: 78
                                        playGlyphScale: 1.4
                                    }
                                }
                                Component {
                                    id: sVinyl
                                    VinylRecordView {
                                        player: PlayerService
                                        size: 84
                                        chapterCount: 4      // grooves like the ribbon vinyl
                                        labelFraction: 0.24  // small centre label — a real record
                                        playZoneScale: 1.5
                                    }
                                }
                            }
                            Seg {
                                Layout.alignment: Qt.AlignVCenter
                                options: [{ label: qsTr("Disc"), value: "disc" },
                                          { label: qsTr("Vinyl"), value: "vinyl" }]
                                current: Prefs.playerStyle
                                onPicked: (v) => Prefs.playerStyle = v
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Artwork print")
                        desc: qsTr("your art, your ink — choose a look, then make it yours.")
                        ArtworkStyleSettings {
                            sampleSource: {
                                const covers = LibraryService.artPaths
                                const portraits = LibraryService.artistImages
                                const currentKey = LibraryService.artKey(PlayerService.currentArtist,
                                                                         PlayerService.currentAlbum)
                                if (covers[currentKey]) return covers[currentKey]
                                for (const key of Object.keys(covers))
                                    if (covers[key]) return covers[key]
                                for (const key of Object.keys(portraits))
                                    if (portraits[key]) return portraits[key]
                                return ""
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Backdrop")
                        desc: qsTr("behind the whole app — keep it clean, or drop in your own image (dimmed).")
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceLg

                            // Image preview (left)
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: 200
                                Layout.preferredHeight: 120
                                radius: Theme.radiusMd
                                color: Theme.cardBg
                                border.color: Theme.borderLight
                                border.width: 1
                                clip: true
                                Image {
                                    anchors.fill: parent
                                    visible: AppSettings.backgroundImagePath.length > 0
                                    source: AppSettings.backgroundImagePath.length > 0
                                            ? ZPathText.url(AppSettings.backgroundImagePath) : ""
                                    fillMode: Image.PreserveAspectCrop
                                    opacity: AppSettings.backgroundOpacity
                                    asynchronous: true
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: AppSettings.backgroundImagePath.length === 0
                                    text: qsTr("default ambient")
                                    font.pixelSize: 12; font.weight: Font.Light
                                    color: Theme.textDim
                                }
                            }

                            // Controls (beside the image)
                            ColumnLayout {
                                Layout.alignment: Qt.AlignTop
                                spacing: Theme.spaceMd
                                Seg {
                                    options: [{ label: qsTr("None"), value: "none" },
                                              { label: qsTr("Grime"), value: "grime" },
                                              { label: qsTr("Dusk"), value: "dusk" },
                                              { label: qsTr("Your image…"), value: "image" }]
                                    current: AppSettings.backgroundImagePath.length > 0
                                             ? "image" : AppSettings.backdropPreset
                                    onPicked: (v) => {
                                        if (v === "image") {
                                            wallpaperDialog.open()
                                        } else {
                                            AppSettings.backgroundImagePath = ""
                                            AppSettings.backdropPreset = v
                                        }
                                    }
                                }
                                RowLayout {
                                    visible: AppSettings.backgroundImagePath.length > 0
                                    spacing: Theme.spaceMd
                                    Text {
                                        Layout.preferredWidth: 58
                                        text: qsTr("dimness")
                                        font.pixelSize: 12; font.weight: Font.Light
                                        color: Theme.textDim
                                    }
                                    ZSlider {
                                        from: 0.05; to: 0.8
                                        value: AppSettings.backgroundOpacity
                                        onMoved: AppSettings.backgroundOpacity = value
                                    }
                                }
                                RowLayout {
                                    visible: AppSettings.backgroundImagePath.length > 0
                                    spacing: Theme.spaceMd
                                    Text {
                                        Layout.preferredWidth: 58
                                        text: qsTr("blur")
                                        font.pixelSize: 12; font.weight: Font.Light
                                        color: Theme.textDim
                                    }
                                    ZSlider {
                                        from: 0; to: 50
                                        value: AppSettings.backgroundBlurAmount
                                        onMoved: AppSettings.backgroundBlurAmount = value
                                    }
                                }
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Ambient glow")
                        desc: qsTr("the grime particles that bloom behind the app while a Zune is connected.")
                        RowLayout {
                            spacing: Theme.spaceMd
                            Sw {
                                on: AppSettings.ambientSpray
                                onToggled: AppSettings.ambientSpray = !AppSettings.ambientSpray
                            }
                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                text: AppSettings.ambientSpray ? qsTr("on") : qsTr("off")
                                font.pixelSize: 13
                                font.weight: Font.Light
                                color: Theme.textSecondary
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Letter breaks")
                        desc: qsTr("break the grids into A–Z sections with marker headers. Off, the grid stays one clean block and the side rail filters by letter.")
                        RowLayout {
                            spacing: Theme.spaceMd
                            Sw {
                                on: AppSettings.letterBreaks
                                onToggled: AppSettings.letterBreaks = !AppSettings.letterBreaks
                            }
                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                text: AppSettings.letterBreaks ? qsTr("sectioned") : qsTr("clean grid")
                                font.pixelSize: 13
                                font.weight: Font.Light
                                color: Theme.textSecondary
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Display font")
                        divider: false
                        desc: qsTr("the accent face for page titles and identity moments.")
                        RowLayout {
                            spacing: Theme.spaceLg
                            GradientText {
                                Layout.alignment: Qt.AlignVCenter
                                text: "zuuned"
                                font.family: Theme.displayFamily
                                font.pixelSize: 30
                                tracking: 1
                            }
                            Seg {
                                Layout.alignment: Qt.AlignVCenter
                                options: [{ label: "Permanent Marker", value: "permanentMarker" },
                                          { label: "Specimen Alien", value: "specimenAlien" }]
                                current: AppSettings.headerFont
                                onPicked: (v) => AppSettings.headerFont = v
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // ── LIBRARY ──
                ColumnLayout {
                    id: tabLibrary
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: stack.currentIndex === 1
                    spacing: Theme.spaceXl

                    Sblk {
                        visible: (LibraryService.musicProbeFailures || []).length > 0
                        title: qsTr("Songs waiting for another read")
                        desc: qsTr("These files could not be read. Your saved song information stays intact; the next scan will retry.")
                        Repeater {
                            model: (LibraryService.musicProbeFailures || []).slice(0, 5)
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                text: ZPathText.display(modelData.filepath)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.customizeBodySize
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                        CaptionText {
                            visible: (LibraryService.musicProbeFailures || []).length > 5
                            text: qsTr("%1 songs will retry.").arg((LibraryService.musicProbeFailures || []).length)
                        }
                    }

                    Sblk {
                        title: qsTr("Podcasts")
                        PodcastPreferences {
                            Layout.fillWidth: true
                            enabled: !SyncEngine.isSyncing
                            autoQueue: AppSettings.podcastAutoQueue
                            episodeCount: AppSettings.podcastAutoQueueCount
                            autoCleanup: AppSettings.podcastAutoCleanup
                            onQueuePicked: value => AppSettings.podcastAutoQueue = value
                            onCountPicked: value => AppSettings.podcastAutoQueueCount = value
                            onCleanupPicked: value => AppSettings.podcastAutoCleanup = value
                        }
                    }

                    Sblk {
                        title: qsTr("Audiobook folders")
                        desc: qsTr("Import a book folder or a collection of book folders. Review titles and part order before adding them. Run this import again when you add book folders; this is not a watched folder.")
                        SpokenButton {
                            objectName: "settingsImportAudiobooks"
                            text: qsTr("import audiobook folder…")
                            enabled: !SpokenMediaService.busy
                            onClicked: audiobookFolderImport.begin()
                        }
                    }

                    Sblk {
                        title: qsTr("Watch folders")
                        desc: qsTr("where zuuned scans — music · movies · tv · anime · other · photos. Downloads off a Zune land in the default folder for that media; without one, the first folder added is the default.")

                        // D2: health strip — only FAILING checks appear.
                        Repeater {
                            model: {
                                void LibraryService.watchFolders
                                void DeviceService.connected
                                const rows = LibraryService.healthChecks()
                                if (DeviceService.devicePresent
                                    && !DeviceService.connected
                                    && !DeviceService.udevRuleOk
                                    && DeviceService.udevRulesApply)
                                    rows.push({
                                        label: qsTr("USB permission rule missing"),
                                        detail: qsTr("Open Settings → Zune → USB access to install or repair the permission rules.")
                                    })
                                return rows
                            }
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: hcCol.implicitHeight
                                                + Theme.spaceMd * 2
                                radius: Theme.radiusMd
                                color: Qt.rgba(0.95, 0.68, 0.20, 0.07)
                                border.width: 1
                                border.color: Qt.alpha(Theme.warning, 0.45)
                                ColumnLayout {
                                    id: hcCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceMd
                                    spacing: 2
                                    Text {
                                        text: modelData.label
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: Theme.warning
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                    Text {
                                        text: modelData.detail
                                        font.pixelSize: 10
                                        font.weight: Font.Light
                                        color: Theme.textSecondary
                                        wrapMode: Text.WrapAnywhere
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }

                        // Watch-folder rows (mock foldrow): type chip ·
                        // path over count · glass "type"/"remove"
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Repeater {
                                model: LibraryService.watchFolders

                                Rectangle {
                                    id: wfRow
                                    required property var modelData
                                    // Live count subline — the void reads
                                    // re-run this as scans land.
                                    readonly property var fc: {
                                        void LibraryService.trackCount
                                        void LibraryService.videoCount
                                        void LibraryService.photoCount
                                        return LibraryService.folderCounts(modelData.path)
                                    }
                                    readonly property string countLabel: {
                                        const t = modelData.type
                                        if (t === "music") return qsTr("%1 tracks").arg(fc.tracks)
                                        if (t === "movies") return qsTr("%1 movies").arg(fc.videos)
                                        if (t === "tv" || t === "anime")
                                            return qsTr("%1 episodes").arg(fc.videos)
                                        if (t === "photos") return qsTr("%1 photos").arg(fc.photos)
                                        return qsTr("%1 items").arg(fc.tracks + fc.videos + fc.photos)
                                    }
                                    Layout.fillWidth: true
                                    implicitHeight: 50
                                    color: wfArea.containsMouse ? Theme.rowHoverPink : "transparent"

                                    MouseArea {
                                        id: wfArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spaceSm
                                        anchors.rightMargin: Theme.spaceSm
                                        spacing: Theme.spaceMd

                                        Rectangle {
                                            implicitWidth: wfChip.implicitWidth + Theme.spaceMd
                                            implicitHeight: 18
                                            radius: 9
                                            color: Qt.rgba(root.typeColor(wfRow.modelData.type).r,
                                                           root.typeColor(wfRow.modelData.type).g,
                                                           root.typeColor(wfRow.modelData.type).b, 0.15)
                                            Text {
                                                id: wfChip
                                                anchors.centerIn: parent
                                                text: root.typeLabel(wfRow.modelData.type)
                                                font.pixelSize: 10
                                                font.weight: Font.Light
                                                color: root.typeColor(wfRow.modelData.type)
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1
                                            Text {
                                                Layout.fillWidth: true
                                                text: wfRow.modelData.displayPath
                                                font.pixelSize: 13
                                                font.weight: Font.Light
                                                color: Theme.textPrimary
                                                elide: Text.ElideMiddle
                                            }
                                            Text {
                                                text: wfRow.countLabel
                                                font.pixelSize: 11
                                                font.weight: Font.Light
                                                color: Theme.textDim
                                            }
                                        }

                                        // One folder per kind receives a
                                        // download. It is the one marked
                                        // here, and until the user marks one
                                        // it is the earliest added folder of
                                        // that kind — which is a fact about
                                        // the row, so it reads as a status and
                                        // not as a button.
                                        Text {
                                            visible: wfRow.modelData.importDefault === true
                                            Layout.alignment: Qt.AlignVCenter
                                            // C++ names the kinds a row receives and
                                            // stops there: which word means "default"
                                            // and how the kinds are joined is wording,
                                            // so it is composed here out of the language
                                            // file. A folder typed "all" that only
                                            // receives videos still has to say so, or
                                            // the row reads as the destination for songs
                                            // it will never get.
                                            text: {
                                                const kinds = wfRow.modelData.defaultKinds || []
                                                const names = kinds.map(t => root.typeLabel(t))
                                                return names.length > 0
                                                       ? qsTr("default · %1").arg(names.join("+"))
                                                       : qsTr("default")
                                            }
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: Theme.pink
                                        }
                                        GlassButton {
                                            visible: wfRow.modelData.importDefault !== true
                                            small: true
                                            text: qsTr("default")
                                            onClicked: LibraryService.setImportDefault(wfRow.modelData.id)
                                        }
                                        GlassButton {
                                            small: true
                                            text: qsTr("type")
                                            onClicked: {
                                                treatAsPopup.folderId = wfRow.modelData.id
                                                treatAsPopup.parent = wfRow
                                                treatAsPopup.open()
                                            }
                                        }
                                        GlassButton {
                                            small: true
                                            text: qsTr("remove")
                                            onClicked: LibraryService.removeWatchFolder(wfRow.modelData.id)
                                        }
                                    }

                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 1
                                        color: Theme.separator
                                    }
                                }
                            }
                        }

                        // mock's add row: quiet label · type pills · browse
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            spacing: Theme.spaceMd
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("add a folder")
                                font.pixelSize: 13; font.weight: Font.Light
                                color: Theme.textDim
                            }
                            Seg {
                                id: addType
                                options: [{ label: qsTr("music"), value: "music" },
                                          { label: qsTr("movies"), value: "movies" },
                                          { label: qsTr("tv"), value: "tv" },
                                          { label: qsTr("anime"), value: "anime" },
                                          { label: qsTr("photos"), value: "photos" }]
                                current: "music"
                                onPicked: (v) => current = v
                            }
                            GlassButton {
                                text: qsTr("browse…")
                                onClicked: root.pickFolder(addType.current)
                            }
                        }

                        // …or type a path — network mounts (SMB/NFS) a
                        // picker may not surface; uses the type pills above
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 28
                                radius: Theme.radiusSm
                                color: Theme.cardBg
                                border.color: manualPath.activeFocus ? Theme.focusRing : Theme.borderFaint
                                TextInput {
                                    id: manualPath
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spaceSm
                                    anchors.rightMargin: Theme.spaceSm
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: Theme.textPrimary
                                    font.pixelSize: 12
                                    clip: true
                                    selectByMouse: true
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: manualPath.text.length === 0
                                        text: qsTr("…or type a path (e.g. %1)")
                                              .arg(Qt.platform.os === "windows"
                                                   ? "C:\\Users\\you\\Music" : "/mnt/MissionControl/music")
                                        color: Theme.textGhost
                                        font.pixelSize: 12
                                    }
                                }
                            }
                            GlassButton {
                                small: true
                                text: qsTr("add path")
                                disabled: manualPath.text.trim().length <= 1
                                onClicked: {
                                    LibraryService.addWatchFolder(manualPath.text.trim(),
                                                                  addType.current)
                                    manualPath.text = ""
                                }
                            }
                        }

                    }

                    Sblk {
                        title: qsTr("Artwork source")
                        desc: qsTr("movie & tv posters, fan art — applies to new lookups")
                        Seg {
                            options: [{ label: "TMDB", value: "tmdb" },
                                      { label: qsTr("Fanart.tv"), value: "fanart" }]
                            current: AppSettings.posterSource
                            onPicked: (v) => AppSettings.posterSource = v
                        }
                    }

                    Sblk {
                        title: qsTr("Lookup API keys")
                        desc: qsTr("optional. Your own key beats the one this build ships with, for new lookups.")

                        // Both readers shipped long before this card existed:
                        // src/library/TmdbClient.cpp:98 prefers this key over the
                        // bundled one, and src/library/MusicIdentityClient.cpp:580
                        // over the shared Fanart.tv key. Nothing asked for that
                        // key over the network unless a lookup was already opted
                        // into (see Music artwork above), so an empty field here
                        // changes nothing at all.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            ApiKeyField {
                                label: "TMDB"
                                placeholder: qsTr("v3 API key")
                                saved: AppSettings.tmdbApiKey
                                onCommitted: (v) => AppSettings.tmdbApiKey = v
                            }
                            ApiKeyField {
                                label: qsTr("Fanart.tv")
                                placeholder: qsTr("personal API key")
                                saved: AppSettings.fanarttvApiKey
                                onCommitted: (v) => AppSettings.fanarttvApiKey = v
                            }
                        }
                        PhaseNote {
                            text: qsTr("A key is stored in plain text in the settings file next to the library, in your own user profile, and is sent only to the provider it belongs to. Clearing the field and scanning away falls back to the bundled key.")
                        }
                    }

                    // Wired: every scan ends with LibraryDb's resolve
                    // pass; picking here kicks a rescan so it applies
                    // now (and keepBoth resurrects the dropped format).
                    Sblk {
                        title: qsTr("Duplicate formats")
                        desc: qsTr("when an album ships FLAC + MP3 of the same tracks — applies on the spot")
                        Seg {
                            options: [{ label: qsTr("Keep both"), value: "keepBoth" },
                                      { label: qsTr("Prefer FLAC"), value: "flac" },
                                      { label: qsTr("Prefer MP3"), value: "mp3" }]
                            current: AppSettings.duplicateFormats
                            onPicked: (v) => {
                                AppSettings.duplicateFormats = v
                                LibraryService.applyDuplicateFormats()
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Re-lookup")
                        desc: qsTr("%1 videos in library, %2 without a TMDB match")
                              .arg(LibraryService.videoCount)
                              .arg(LibraryService.unmatchedVideoCount)
                        GlassButton {
                            text: qsTr("retry unmatched lookups (%1)")
                                  .arg(LibraryService.unmatchedVideoCount)
                            disabled: LibraryService.unmatchedVideoCount === 0
                            onClicked: LibraryService.requeueAllUnmatched()
                        }
                    }

                    Sblk {
                        title: qsTr("Music artwork")
                                                desc: qsTr("Covers are read from your own files first: an embedded picture or a folder image never needs the network, and art you already have is never replaced. An album that stays blank can ask a music store for its cover - its right-click menu does that for one album, this switch lets the library do it for every blank one, and it starts off. Portraits and tag lookups go to MusicBrainz and stay off until you ask.")
                        RowLayout {
                            spacing: Theme.spaceSm
                            SpokenCheckBox {
                                objectName: "albumCoverLookupToggle"
                                checked: AppSettings.albumCoverLookup
                                Accessible.name: qsTr("look up missing album covers from the music store")
                                onClicked: AppSettings.albumCoverLookup =
                                    !AppSettings.albumCoverLookup
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("look up missing album covers from the music store")
                                color: Theme.textSecondary
                                font.pixelSize: 13
                                font.weight: Font.Light
                                wrapMode: Text.WordWrap
                            }
                        }
                        RowLayout {
                            spacing: Theme.spaceSm
                            SpokenCheckBox {
                                objectName: "autoMetadataLookupToggle"
                                checked: AppSettings.autoMetadataLookup
                                Accessible.name: qsTr("look up artist portraits and tags online")
                                onClicked: AppSettings.autoMetadataLookup =
                                    !AppSettings.autoMetadataLookup
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("look up artist portraits and tags online (MusicBrainz)")
                                color: Theme.textSecondary
                                font.pixelSize: 13
                                font.weight: Font.Light
                                wrapMode: Text.WordWrap
                            }
                        }
                        GlassButton {
                            text: qsTr("find missing artwork")
                            disabled: LibraryService.trackCount === 0
                            onClicked: LibraryService.retryMissingArtwork()
                        }
                    }

                    Sblk {
                        title: qsTr("Rescan")
                        desc: qsTr("re-read every watch folder now")
                        divider: false
                        GlassButton {
                            primary: true
                            text: qsTr("rescan library")
                            disabled: LibraryService.scanning
                                     || LibraryService.watchFolders.length === 0
                            onClicked: LibraryService.rescan()
                        }
                        // live scan progress
                        ColumnLayout {
                            visible: LibraryService.scanning
                            Layout.fillWidth: true
                            Layout.maximumWidth: 470
                            spacing: Theme.spaceXxs
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 4
                                radius: 2
                                color: Theme.cardHover
                                Rectangle {
                                    width: parent.width * (LibraryService.scanTotal > 0
                                           ? LibraryService.scanCurrent / LibraryService.scanTotal : 0)
                                    height: parent.height
                                    radius: 2
                                    color: Theme.pink
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: LibraryService.scanStage + " — "
                                      + LibraryService.scanCurrent + "/" + LibraryService.scanTotal
                                      + "   " + LibraryService.scanCurrentFile
                                font.pixelSize: 11
                                font.weight: Font.Light
                                color: Theme.textDim
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // ── DEVICE ──
                ColumnLayout {
                    id: tabDevice
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: stack.currentIndex === 2
                    spacing: Theme.spaceXl

                    Sblk {
                        title: qsTr("USB access")
                        desc: DeviceService.udevRulesApply
                            ? qsTr("let zuuned own the Zune without sudo each time.")
                            : qsTr("Windows claims the Zune through a WinUSB driver - no rules, no sudo.")
                        // Windows has no udev: name WinUSB instead of offering a rule install.
                        ColumnLayout {
                            id: usbAccessCol
                            visible: !DeviceService.udevRulesApply
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            spacing: 2
                            // "present" and "visible to libusb" are different facts on
                            // Windows: a Zune with no driver bound sits on the bus and
                            // is still nowhere in libusb_get_device_list().
                            readonly property bool blocked:
                                DeviceService.devicePresent && DeviceService.usbAccessProblem
                            Text {
                                text: usbAccessCol.blocked
                                      ? qsTr("Zune plugged in, but not visible to libusb")
                                      : DeviceService.devicePresent
                                        ? qsTr("Zune visible to libusb")
                                        : qsTr("No Zune visible to libusb")
                                font.pixelSize: 14; font.weight: Font.Light
                                color: DeviceService.devicePresent && !usbAccessCol.blocked
                                       ? Theme.textPrimary : Theme.warning
                            }
                            // The device id to pick in Zadig, copyable rather than
                            // retyped: read-only selectable, same as the udev line.
                            TextEdit {
                                visible: usbAccessCol.blocked
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                text: DeviceService.usbAccessCommand
                                font.family: "monospace"
                                font.pixelSize: 9
                                color: Theme.textDim
                                wrapMode: TextEdit.WrapAnywhere
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("zuuned talks to the Zune through libusb, which on Windows needs a WinUSB driver bound to the device. If a plugged-in Zune never shows up here, install WinUSB for it with the Zadig utility, then replug. zuuned never installs a driver.")
                                font.pixelSize: 11; font.weight: Font.Light
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                            }
                        }
                        RowLayout {
                            visible: DeviceService.udevRulesApply
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            spacing: Theme.spaceMd
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: DeviceService.udevRuleOk ? qsTr("Rule installed") : qsTr("Rule missing")
                                    font.pixelSize: 14; font.weight: Font.Light
                                    color: DeviceService.udevRuleOk ? Theme.textPrimary : Theme.warning
                                }
                                Text {
                                    text: qsTr("installs with one click (asks for your password)")
                                    font.pixelSize: 11; font.weight: Font.Light
                                    color: Theme.textDim
                                }
                            }
                            GlassButton {
                                visible: DeviceService.udevRulesApply && !DeviceService.udevRuleOk
                                Layout.alignment: Qt.AlignVCenter
                                primary: true
                                text: qsTr("install rule")
                                onClicked: DeviceService.installUdevRule()
                            }
                            // Read-only state light; flipping it while
                            // missing runs the same one-click install.
                            Sw {
                                Layout.alignment: Qt.AlignVCenter
                                on: DeviceService.udevRuleOk
                                onToggled: if (!DeviceService.udevRuleOk)
                                               DeviceService.installUdevRule()
                            }
                        }
                        // Manual fallback if the pkexec prompt is refused
                        TextEdit {
                            visible: DeviceService.udevRulesApply && !DeviceService.udevRuleOk
                            Layout.fillWidth: true
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            text: DeviceService.udevInstallCommand
                            selectionColor: Theme.pink
                            font.pixelSize: 11; font.weight: Font.Light; color: Theme.textGhost
                        }
                        Connections {
                            target: DeviceService
                            function onUdevRuleInstalled(ok, message) {
                                toastHost.show(message)
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("Transcode acceleration")
                        desc: qsTr("hardware video decode when your GPU supports it — ~4x faster transcodes.")
                        Seg {
                            options: [{ label: qsTr("Auto"), value: "auto" },
                                      { label: qsTr("NVIDIA"), value: "nvenc" },
                                      { label: qsTr("AMD"), value: "amd",
                                        disabled: Qt.platform.os === "windows" },
                                      { label: qsTr("CPU"), value: "cpu" }]
                            current: AppSettings.transcodeAccel
                            onPicked: (v) => AppSettings.transcodeAccel = v
                        }
                        // What the pills actually reach, in the order the encoder
                        // asks. VAAPI is a Linux API: the code path is there and
                        // always fails on Windows, which is why the AMD pill is
                        // inert here instead of promising a decode that cannot
                        // happen. Measured on this machine, auto answers NVDEC.
                        PhaseNote {
                            text: Qt.platform.os === "windows"
                                  ? qsTr("Auto tries NVDEC, then software. VAAPI, the way an AMD or Intel card decodes on Linux, does not exist on Windows, so AMD is inert here and those GPUs transcode on the CPU.")
                                  : qsTr("Auto tries NVDEC, then VAAPI, then software. A missing GPU falls back to software either way.")
                        }
                    }

                    Sblk {
                        id: videoProfileCard
                        title: qsTr("Default video profile")
                        desc: qsTr("quality & size for videos sent to the Zune.")

                        // Family (0xD21A) → profile, resolved live from the
                        // connected Zune. -1 when nothing is plugged in.
                        readonly property int autoProfile: {
                            if (!DeviceService.connected)
                                return -1
                            switch (DeviceService.deviceFamily) {
                            case 0x00: return 0   // Keel — Zune 30
                            case 0x06: return 2   // Pavo — Zune HD
                            case 0x02:            // Scorpius — 4/8/16
                            case 0x03: return 1   // Draco — 80/120
                            default:   return 0   // unknown → WMV (safe)
                            }
                        }
                        // What the radios show: an explicit override wins;
                        // otherwise the detected profile lights up — no
                        // abstract "Auto" row to decode.
                        readonly property int shownProfile:
                            AppSettings.videoProfile >= 0
                                ? AppSettings.videoProfile
                                : autoProfile

                        RowLayout {
                            spacing: Theme.spaceSm
                            Text {
                                text: {
                                    if (AppSettings.videoProfile >= 0)
                                        return qsTr("manual override")
                                    if (!DeviceService.connected)
                                        return qsTr("auto — detects when a zune connects")
                                    return qsTr("auto-detected from this zune")
                                }
                                font.pixelSize: 12; font.weight: Font.Light
                                color: AppSettings.videoProfile >= 0
                                       ? Theme.orange : Theme.pink
                            }
                            Text {
                                visible: AppSettings.videoProfile >= 0
                                text: qsTr("reset to auto")
                                font.pixelSize: 12
                                color: Theme.textSecondary
                                font.underline: true
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: AppSettings.videoProfile = -1
                                }
                            }
                        }

                        // Mock's pills, real profiles: nothing lit while
                        // unplugged with no override (shownProfile -1).
                        Seg {
                            options: [{ label: qsTr("Zune 30 (WMV)"), value: "0" },
                                      { label: qsTr("Classic (H.264)"), value: "1" },
                                      { label: qsTr("HD 480p"), value: "2" },
                                      { label: qsTr("HD 720p"), value: "3" }]
                            current: videoProfileCard.shownProfile >= 0
                                     ? String(videoProfileCard.shownProfile) : ""
                            onPicked: (v) => AppSettings.videoProfile = parseInt(v)
                        }

                    }

                    Sblk {
                        title: qsTr("Video size floor")
                        desc: qsTr("the smallest video the library will index.")

                        // Wired, and long overdue: the scan reads this key on
                        // every pass (src/library/LibraryScanner.cpp:426) and
                        // when it refuses a file it prints "Settings > Device >
                        // ... or lower videoMinSizeMB"
                        // (src/library/LibraryScanner.cpp:876) -- advice about a
                        // control that did not exist on any tab, on either
                        // platform, until now.
                        //
                        // "Auto" is not a number: it is the per-folder-type rule
                        // in VideoNaming::videoSkipReason(), 50 MB only where a
                        // folder declares itself movies and 5 MB for every other
                        // type including "all", because a phone clip in a mixed
                        // folder is routinely 20 MB. Any number here wins over
                        // the folder type, which is the whole point.
                        Seg {
                            options: [{ label: qsTr("Auto (by folder type)"), value: "0" },
                                      { label: qsTr("5 MB"), value: "5" },
                                      { label: qsTr("25 MB"), value: "25" },
                                      { label: qsTr("50 MB"), value: "50" },
                                      { label: qsTr("100 MB"), value: "100" }]
                            current: String(AppSettings.videoMinSizeMB)
                            onPicked: (v) => AppSettings.videoMinSizeMB = parseInt(v)
                        }
                        PhaseNote {
                            text: AppSettings.videoMinSizeMB > 0
                                  ? qsTr("Any video under %1 MB is not indexed, whatever the folder is typed as. Applies on the next scan.")
                                        .arg(AppSettings.videoMinSizeMB)
                                  : qsTr("Right now a folder typed movies needs 50 MB and every other type 5 MB. Applies on the next scan.")
                        }
                        // The floor screens guesses, never requests: whatever the
                        // app pulled off a player into its own download folder is
                        // exempt, because a device-sized transcode is a few MB
                        // and the user asked for that file by right-clicking.
                        PhaseNote {
                            text: qsTr("Nothing this app downloaded from a Zune is ever refused for size, and a refusal is counted and named by the scan rather than kept quiet.")
                        }
                    }

                    // Not in the mock — kept deliberately: transcode needs
                    // a language pick for multi-audio sources.
                    Sblk {
                        title: qsTr("Preferred audio language")
                        desc: qsTr("picked when a video carries more than one audio track.")
                        divider: false

                        ComboBox {
                            id: langBox
                            implicitWidth: 200
                            model: [
                                { text: qsTr("English"), tag: "eng" },
                                { text: qsTr("Japanese"), tag: "jpn" },
                                { text: qsTr("Spanish"), tag: "spa" },
                                { text: qsTr("French"), tag: "fre" },
                                { text: qsTr("German"), tag: "ger" },
                                { text: qsTr("Korean"), tag: "kor" },
                                { text: qsTr("Chinese"), tag: "chi" },
                                { text: qsTr("Portuguese"), tag: "por" },
                                { text: qsTr("Italian"), tag: "ita" },
                                { text: qsTr("Russian"), tag: "rus" }
                            ]
                            textRole: "text"
                            currentIndex: {
                                for (let i = 0; i < model.length; i++)
                                    if (model[i].tag === AppSettings.preferredAudioLang)
                                        return i
                                return 0
                            }
                            onActivated: AppSettings.preferredAudioLang = model[currentIndex].tag

                            background: Rectangle {
                                radius: Theme.radiusMd
                                color: Theme.cardBg
                                border.color: Theme.borderLight
                                border.width: 1
                            }
                            contentItem: Text {
                                leftPadding: Theme.spaceMd
                                verticalAlignment: Text.AlignVCenter
                                text: langBox.displayText
                                font.pixelSize: 13
                                font.weight: Font.Light
                                color: Theme.textPrimary
                            }
                            popup: Popup {
                                y: langBox.height + 2
                                width: langBox.width
                                padding: 4
                                background: Rectangle {
                                    radius: Theme.radiusMd
                                    color: Theme.surfaceBg
                                    border.color: Theme.borderLight
                                    border.width: 1
                                }
                                contentItem: ListView {
                                    implicitHeight: Math.min(contentHeight, 280)
                                    clip: true
                                    model: langBox.model
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: ListView.view.width
                                        height: 30
                                        radius: Theme.radiusSm
                                        color: langArea.containsMouse ? Theme.rowHoverPink : "transparent"
                                        Text {
                                            anchors.left: parent.left
                                            anchors.leftMargin: Theme.spaceSm
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: modelData.text
                                            font.pixelSize: 13
                                            font.weight: Font.Light
                                            color: Theme.textPrimary
                                        }
                                        MouseArea {
                                            id: langArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: {
                                                langBox.currentIndex = index
                                                AppSettings.preferredAudioLang = modelData.tag
                                                langBox.popup.close()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Sblk {
                        id: thisZuneCard
                        title: qsTr("This Zune")
                        // Live values first, then the player this app last talked
                        // to.  Left to `visible: DeviceService.connected` this card
                        // was empty on every screen the user actually opened it
                        // from, and blank beside a remembered serial asked the one
                        // question nobody is asking.  What is kept from the last
                        // session is said to be a memory, not a present reading:
                        // only the identity survives a cable coming out, and the
                        // card never invents the bytes it cannot ask for.
                        readonly property var remembered: DeviceService.deviceHistory.latest
                        readonly property bool live: DeviceService.connected
                        function rememberedText(key) {
                            const m = thisZuneCard.remembered
                            if (!m)
                                return ""
                            const v = m[key]
                            return (v === undefined || v === null) ? "" : String(v)
                        }
                        readonly property string rememberedName: rememberedText("name")
                        readonly property string rememberedSerial: rememberedText("serial")
                        readonly property string rememberedModel: rememberedText("model")
                        readonly property string rememberedSeen: rememberedText("lastSeenDate")
                        readonly property bool rememberedKnown: rememberedSerial.length > 0
                        readonly property double rememberedCapacity: {
                            const m = thisZuneCard.remembered
                            return m && m["capacityGB"] ? Number(m["capacityGB"]) : 0
                        }
                        desc: live
                              ? qsTr("read from the player on this connection.")
                              : rememberedKnown
                                    ? (rememberedSeen.length > 0
                                       ? qsTr("nothing is connected right now. What follows is what the last session read, seen %1.")
                                             .arg(rememberedSeen)
                                       : qsTr("nothing is connected right now. What follows is what the last session read."))
                                    : qsTr("nothing is connected right now.")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            StatRow {
                                label: qsTr("Name")
                                value: thisZuneCard.live
                                       ? DeviceService.name
                                       : (thisZuneCard.rememberedName.length > 0
                                          ? thisZuneCard.rememberedName
                                          : qsTr("not reported"))
                            }
                            StatRow {
                                label: qsTr("Model")
                                value: thisZuneCard.live
                                       ? (DeviceService.modelName.length > 0
                                          ? DeviceService.modelName : DeviceService.model)
                                       : (thisZuneCard.rememberedModel.length > 0
                                          ? thisZuneCard.rememberedModel
                                          : qsTr("not reported"))
                            }
                            StatRow {
                                label: qsTr("Serial")
                                value: thisZuneCard.live
                                       ? DeviceService.deviceSerial
                                       : (thisZuneCard.rememberedKnown
                                          ? thisZuneCard.rememberedSerial
                                          : qsTr("not reported"))
                            }
                            StatRow {
                                label: qsTr("Storage")
                                value: {
                                    if (thisZuneCard.live && DeviceService.capacityGB > 0)
                                        return qsTr("%1 GB free of %2 GB")
                                                   .arg(DeviceService.freeGB.toFixed(1))
                                                   .arg(DeviceService.capacityGB.toFixed(1))
                                    if (thisZuneCard.rememberedCapacity > 0)
                                        return qsTr("%1 GB total, read on the last session")
                                                   .arg(thisZuneCard.rememberedCapacity.toFixed(0))
                                    return qsTr("not reported")
                                }
                            }
                            StatRow {
                                label: qsTr("Music / video / photos")
                                value: thisZuneCard.live
                                       ? DeviceService.musicGB.toFixed(2) + " / "
                                         + DeviceService.videoGB.toFixed(2) + " / "
                                         + DeviceService.photoGB.toFixed(2) + " GB"
                                       : qsTr("read while connected")
                            }
                            StatRow {
                                label: qsTr("Contents")
                                visible: thisZuneCard.live
                                value: qsTr("%1 tracks · %2 albums · %3 artists")
                                           .arg(DeviceService.trackCount)
                                           .arg(DeviceService.albumCount)
                                           .arg(DeviceService.artistCount)
                            }
                            StatRow {
                                label: qsTr("Battery")
                                visible: thisZuneCard.live
                                value: DeviceService.battery >= 0
                                       ? DeviceService.battery + "%" : qsTr("not reported")
                                divider: false
                            }
                        }
                        // No firmware row, on purpose: libzune exposes a serial and a
                        // model and nothing else, so a firmware number here would be
                        // invented rather than read.
                        PhaseNote {
                            visible: !DeviceService.connected && !thisZuneCard.rememberedKnown
                            text: qsTr("Plug a Zune in and this card fills itself in from the device.")
                        }
                    }

                    Sblk {
                        title: qsTr("Previously connected")
                        desc: qsTr("every Zune this app has talked to, so a player you plug in next month is still recognised.")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            visible: DeviceService.deviceHistory.count > 0
                            Repeater {
                                model: DeviceService.deviceHistory.devices
                                delegate: ColumnLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 5
                                        Layout.bottomMargin: 5
                                        spacing: Theme.spaceMd
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1
                                            Text {
                                                text: modelData.name.length > 0
                                                      ? modelData.name : qsTr("Unnamed Zune")
                                                font.pixelSize: 13; font.weight: Font.Medium
                                                color: Theme.textPrimary
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: [modelData.model,
                                                       modelData.capacityGB > 0
                                                           ? modelData.capacityGB.toFixed(0) + " GB" : "",
                                                       qsTr("serial %1").arg(modelData.serial),
                                                       (modelData.connects === 1 ? qsTr("%1 visit") : qsTr("%1 visits")).arg(modelData.connects),
                                                       qsTr("last seen %1").arg(modelData.lastSeenDate)]
                                                      .filter(function (s) { return s.length > 0 })
                                                      .join("  ·  ")
                                                font.pixelSize: 11; font.weight: Font.Light
                                                color: Theme.textDim
                                                elide: Text.ElideRight
                                            }
                                        }
                                        GlassButton {
                                            text: qsTr("forget")
                                            small: true
                                            onClicked: DeviceService.deviceHistory.forget(modelData.key)
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 1
                                        color: Theme.separator
                                    }
                                }
                            }
                        }
                        RowLayout {
                            visible: DeviceService.deviceHistory.count > 0
                            spacing: Theme.spaceMd
                            GlassButton {
                                text: qsTr("clear all")
                                small: true
                                onClicked: DeviceService.deviceHistory.clearAll()
                            }
                            Text {
                                Layout.fillWidth: true
                                text: DeviceService.deviceHistory.storePath
                                font.pixelSize: 10; font.weight: Font.Light
                                color: Theme.textGhost
                                elide: Text.ElideMiddle
                            }
                        }
                        PhaseNote {
                            visible: DeviceService.deviceHistory.count === 0
                            text: qsTr("Nothing yet. This list is written the first time a Zune connects.")
                        }
                    }

                    Sblk {
                        id: audioConversionCard
                        title: qsTr("Audio conversion")
                        desc: qsTr("what happens to a file that a Zune cannot play as it stands.")
                        // Asked of the running program, not of a table, and asked
                        // again the moment the user points the app at a folder —
                        // "is ffmpeg there?" has a different answer one second after
                        // somebody picks one. probeRevision is the nudge, the ini
                        // value is the one that actually changed.
                        property int probeRevision: 0
                        property string selfTestText: ""
                        readonly property var tools: {
                            audioConversionCard.probeRevision
                            AppSettings.ffmpegDir
                            return DiagnosticsService.conversionTools()
                        }

                        Seg {
                            options: [{ label: "MP3", value: "mp3" },
                                      { label: "WMA", value: "wma",
                                        disabled: !audioConversionCard.tools.wmaEncoder },
                                      // Its own availability because it is its own
                                      // encoder: the one inside Windows, not the
                                      // FFmpeg this build links.  It is absent on
                                      // Linux rather than shown and failing, and
                                      // the caution about which players actually
                                      // play the result is said below, not hidden
                                      // behind a third button.
                                      { label: qsTr("WMA Lossless"),
                                        value: "wma-lossless",
                                        disabled: !audioConversionCard.tools
                                                  .wmaLosslessEncoder }]
                            current: AppSettings.syncAudioFormat
                            onPicked: (v) => AppSettings.syncAudioFormat = v
                        }
                        // One decision, which is what the original client
                        // shipped (a quality preset, not a codec matrix). These
                        // four are the same constant bit rates the card used to
                        // list as bare numbers and the same stored key
                        // underneath, so a preset is a name over a value rather
                        // than a second source of truth. Both lossy encoders
                        // honour it: LAME for MP3, and for WMA wmav2, which takes
                        // 32-320 kbps and has no VBR mode at all (measured with
                        // `ffmpeg -h encoder=wmav2` on this build).  Lossless has
                        // no bit rate to choose -- it spends what the recording is
                        // worth -- so the row hides itself there instead of
                        // showing four presets that do nothing.
                        Seg {
                            visible: AppSettings.syncAudioFormat !== "wma-lossless"
                            options: [{ label: qsTr("Space saving 128"), value: "128" },
                                      { label: qsTr("Standard 192"), value: "192" },
                                      { label: qsTr("High 256"), value: "256" },
                                      { label: qsTr("Best 320"), value: "320" }]
                            current: String(AppSettings.syncAudioBitrate)
                            onPicked: (v) => AppSettings.syncAudioBitrate = parseInt(v)
                        }
                        // The second knob LAME has and this card never exposed: how
                        // hard the encoder works for the bits the preset above
                        // allows. -q0 is the slowest and best MP3 LAME can write,
                        // -q9 the fastest and worst; 2 is LAME's own default, which
                        // is the number this build always used, so the default here
                        // is the behaviour a user already has rather than a new
                        // guess. Only MP3 has it — wmav2 takes a bit rate and nothing
                        // else, which is why the pill hides itself on WMA.
                        Seg {
                            visible: AppSettings.syncAudioFormat === "mp3"
                            options: [{ label: qsTr("Detail 0 (slowest)"), value: "0" },
                                      { label: qsTr("Detail 2 (default)"), value: "2" },
                                      { label: qsTr("Detail 4"), value: "4" },
                                      { label: qsTr("Detail 6 (fastest)"), value: "6" }]
                            current: String(AppSettings.mp3Quality)
                            onPicked: (v) => AppSettings.mp3Quality = parseInt(v)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            StatRow {
                                label: "MP3"
                                value: audioConversionCard.tools.mp3Encoder
                                       ? qsTr("LAME %1, constant bit rate, ID3v2.3, detail %2")
                                             .arg(audioConversionCard.tools.mp3EncoderVersion)
                                             .arg(audioConversionCard.tools.mp3Quality)
                                       : qsTr("no encoder in this build")
                                valueColor: audioConversionCard.tools.mp3Encoder
                                            ? Theme.textDim : Theme.warning
                            }
                            StatRow {
                                label: "WMA"
                                value: audioConversionCard.tools.wmaEncoder
                                       ? qsTr("libavcodec wmav2 in ASF — lossy")
                                       : qsTr("no encoder in this build")
                                valueColor: audioConversionCard.tools.wmaEncoder
                                            ? Theme.textDim : Theme.warning
                            }
                            StatRow {
                                label: qsTr("WMA Lossless")
                                value: audioConversionCard.tools.wmaLosslessEncoder
                                       ? qsTr("Windows Media Audio 9.2 Lossless (in Windows)")
                                       : qsTr("not on this platform")
                                valueColor: audioConversionCard.tools.wmaLosslessEncoder
                                            ? Theme.textDim : Theme.warning
                            }
                            StatRow {
                                label: qsTr("Conversion library")
                                value: qsTr("FFmpeg %1 (built in)")
                                           .arg(audioConversionCard.tools.bundledLibav)
                            }
                            StatRow {
                                label: qsTr("Already the right format")
                                value: AppSettings.copyNativeAudio
                                       && (AppSettings.syncAudioFormat === "wma"
                                           || AppSettings.syncAudioFormat === "wma-lossless")
                                       ? qsTr("sent as-is") : qsTr("converted")
                                divider: false
                            }
                        }

                        RowLayout {
                            spacing: Theme.spaceMd
                            Sw {
                                Layout.alignment: Qt.AlignVCenter
                                on: AppSettings.copyNativeAudio
                                onToggled: AppSettings.copyNativeAudio = !AppSettings.copyNativeAudio
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("keep a WMA already on the computer byte for byte")
                                font.pixelSize: 13; font.weight: Font.Light
                                color: Theme.textPrimary
                            }
                        }

                        // The honest half, said out loud rather than left to the
                        // user to discover through a silent quality loss.
                        PhaseNote {
                            text: qsTr("WMA Lossless is written by the encoder that is already inside Windows - Windows Media Audio 9.2 Lossless, reached through the Windows Media Format runtime. The FFmpeg this build links carries wmav1 and wmav2 as its only WMA encoders (measured: `ffmpeg -encoders`), so this is the one destination that does not come from it, and no codec is bundled, vendored or forked to get it. It is Windows-only, which is also the good news: on Windows a FLAC, WAV or APE now has somewhere lossless to go instead of being converted down.")
                        }
                        PhaseNote {
                            text: qsTr("It has eight formats and this build refuses a file that fits none of them rather than resampling it into something that is no longer the recording: stereo or 5.1 surround, 44.1 to 96 kHz, 16-bit only at 44.1 kHz and 24-bit above it, and no mono at all. A 16-bit file at 48 kHz or higher is widened to 24 bits at the same rate and channel count, which costs nothing; anything else is refused and the queue says why.")
                        }
                        PhaseNote {
                            text: qsTr("What a player does with the result is the part nobody can promise. WMA Lossless is documented for the Zune HD and documented neither way for every other model, so on a Classic or a Zune 30 the likely answer is a track that stays silent rather than one that plays worse. Nothing this build has written has been heard on any Zune. If a lossless track goes quiet on the player, switch the format above back to MP3 or WMA and sync again - nothing on the computer is touched.")
                        }
                        PhaseNote {
                            text: qsTr("Bit rate is constant only. A variable-bitrate MP3 header is not written by this build - measured, the LAME/Xing frame is absent and the player then reports a track three minutes shorter than it is - so VBR is not offered.")
                        }

                        // Where the two external programs come from, because
                        // "found on PATH" was an answer to a question the user has
                        // no way to influence: a downloaded FFmpeg is one folder,
                        // and being told to edit PATH is being told to leave the app.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceXs
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceMd
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        text: qsTr("FFmpeg command-line tools")
                                        font.pixelSize: 13; font.weight: Font.Light
                                        color: Theme.textPrimary
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: {
                                            const t = audioConversionCard.tools
                                            if (t.commandLineTools)
                                                return t.ffmpegPath
                                            if (t.ffmpegDirGiven)
                                                return qsTr("nothing usable inside the folder you chose")
                                            return qsTr("not found on PATH")
                                        }
                                        font.pixelSize: 11; font.weight: Font.Light
                                        color: audioConversionCard.tools.commandLineTools
                                               ? Theme.textDim : Theme.warning
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: qsTr("found on %1").arg(audioConversionCard.tools.ffmpegSource)
                                        visible: audioConversionCard.tools.commandLineTools
                                                 && audioConversionCard.tools.ffmpegSource !== "none"
                                        font.pixelSize: 10; font.weight: Font.Light
                                        color: Theme.textGhost
                                    }
                                }
                                GlassButton {
                                    text: qsTr("choose folder")
                                    small: true
                                    onClicked: ffmpegFolderDialog.open()
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceMd
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: qsTr("Converting audio inside the app needs nothing else installed. The command-line ffmpeg and ffprobe are used for two jobs only: video thumbnails for the device page, and reading a file the built-in library cannot open. Without them those two things quietly do not happen. A folder chosen here is tried before PATH; a program already running keeps the PATH it started with, so restart Zuuned if a thumbnail is still missing.")
                                    font.pixelSize: 12; font.weight: Font.Light
                                    color: Theme.textDim
                                }
                                GlassButton {
                                    text: qsTr("check again")
                                    small: true
                                    onClicked: audioConversionCard.probeRevision++
                                }
                                GlassButton {
                                    text: qsTr("clear")
                                    small: true
                                    visible: audioConversionCard.tools.ffmpegDirGiven
                                    onClicked: AppSettings.ffmpegDir = ""
                                }
                                GlassButton {
                                    text: qsTr("download FFmpeg ↗")
                                    small: true
                                    onClicked: Qt.openUrlExternally(audioConversionCard.tools.downloadUrl)
                                }
                            }
                        }

                        // The question actually asked: "is conversion really
                        // working?" is answered by converting something and reading
                        // it back, not by a list of linked libraries.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            GlassButton {
                                text: DiagnosticsService.audioSelfTestRunning
                                      ? qsTr("testing…") : qsTr("run encoder self-test")
                                small: true
                                enabled: !DiagnosticsService.audioSelfTestRunning
                                onClicked: DiagnosticsService.runAudioSelfTest()
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: audioConversionCard.selfTestText.length > 0
                                      ? audioConversionCard.selfTestText
                                      : qsTr("Writes a two-second tone, sends it through every encoder this build can reach at the settings above, and reports what came back. Nothing is stored and nothing is sent to a player.")
                                font.pixelSize: 12; font.weight: Font.Light
                                color: Theme.textDim
                            }
                        }
                        Connections {
                            target: DiagnosticsService
                            function onAudioSelfTestFinished(result) {
                                audioConversionCard.selfTestText =
                                    audioConversionCard.describeSelfTest(result)
                            }
                        }
                        // The worker thread answers in codes; the page owns the
                        // wording, so a self-test reads in whatever language the
                        // user is reading rather than in whatever the worker had.
                        function describeSelfTest(result) {
                            if (!result)
                                return qsTr("The test returned nothing.")
                            const code = String(result.code === undefined ? "" : result.code)
                            if (code === "busy")
                                return qsTr("A transfer or a device job is running. The encoders take one job at a time, so try again when the queue is empty.")
                            if (code === "tone")
                                return qsTr("Could not write the test tone: %1")
                                           .arg(String(result.detail === undefined ? "" : result.detail))
                            const mp3 = result.mp3 === undefined ? ({}) : result.mp3
                            const wma = result.wma === undefined ? ({}) : result.wma
                            const ls = result.lossless === undefined ? ({}) : result.lossless
                            return qsTr("At %1 kbps, detail %2 — %3")
                                       .arg(Number(result.bitrate === undefined ? 0 : result.bitrate))
                                       .arg(Number(result.quality === undefined ? 0 : result.quality))
                                       .arg(encoderLine("MP3", mp3) + "   ·   "
                                            + encoderLine("WMA", wma) + "   ·   "
                                            + encoderLine(qsTr("Lossless"), ls))
                        }
                        function encoderLine(name, r) {
                            if (!r || r.available !== true)
                                return name + ": " + qsTr("no encoder in this build")
                            const c = String(r.code === undefined ? "" : r.code)
                            if (c === "ok")
                                return name + ": " + qsTr("%1 bytes, %2 s, %3")
                                                 .arg(Number(r.bytes === undefined ? 0 : r.bytes))
                                                 .arg(Number(r.duration === undefined ? 0 : r.duration).toFixed(2))
                                                 .arg(String(r.container === undefined ? "" : r.container))
                            return name + ": " + selfTestCodeText(c)
                        }
                        function selfTestCodeText(code) {
                            if (code === "refused") return qsTr("the encoder refused the file")
                            if (code === "cancelled") return qsTr("cancelled before it finished")
                            if (code === "container") return qsTr("wrote a container this build does not recognise")
                            if (code === "unprobeable") return qsTr("wrote a file that cannot be read back")
                            if (code === "duration") return qsTr("the length read back is wrong")
                            if (code === "size") return qsTr("the file is far too small for its bit rate")
                            return qsTr("failed")
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // ── MIXTAPES ──
                ColumnLayout {
                    id: tabMixtapes
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: stack.currentIndex === 3
                    spacing: Theme.spaceXl

                    Sblk {
                        title: qsTr("Your mixtapes")
                        desc: qsTr("built here, synced to the Zune.")
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            spacing: 0
                            Repeater {
                                model: LibraryService.playlists
                                delegate: ColumnLayout {
                                    id: mixRow
                                    required property var modelData
                                    required property int index
                                    // Cover art + accents — the playlists
                                    // shelf's recipe (MusicPage), shrunk
                                    // to a row icon. First cover wears
                                    // the label (stable, no re-roll).
                                    readonly property var artUrls: {
                                        void LibraryService.artPaths
                                        const rows = LibraryService
                                            .playlistTracks(mixRow.modelData.id)
                                        const seen = ({})
                                        const urls = []
                                        for (let i = 0; i < rows.length
                                                 && urls.length < 8; i++) {
                                            const t = rows[i]
                                            // The album's owner names the cover,
                                            // not the track credit: same rule as
                                            // the grid, the drill-down and the
                                            // player, and the row carries both.
                                            const key = LibraryService.artKey(
                                                t.albumartist || t.artist, t.album)
                                            if (seen[key])
                                                continue
                                            seen[key] = true
                                            const u = LibraryService.artPaths[key] ?? ""
                                            if (u !== "")
                                                urls.push(u)
                                        }
                                        return urls
                                    }
                                    readonly property var accents: {
                                        if (mixRow.artUrls.length === 0)
                                            return []
                                        const cols = mixRow.artUrls.map(
                                            u => LibraryService.dominantColor(u))
                                        cols.sort((a, b) => a.hslHue - b.hslHue)
                                        return [cols[Math.floor(cols.length / 2)],
                                                cols[Math.floor(cols.length / 4)]]
                                    }
                                    // "on zune" = a device playlist with the
                                    // same name; only knowable while connected
                                    readonly property bool onZune: {
                                        if (!DeviceService.connected)
                                            return false
                                        const dev = DeviceService.playlistsList
                                        for (let i = 0; i < dev.length; ++i)
                                            if (dev[i].name === modelData.name)
                                                return true
                                        return false
                                    }
                                    // Imported tracks still being pulled/
                                    // scanned in (rides playlistsChanged)
                                    readonly property int missing: {
                                        void LibraryService.playlists
                                        return LibraryService
                                            .pendingPlaylistAddCount(modelData.id)
                                    }
                                    readonly property bool pullingNow:
                                        missing > 0 && DeviceService.pulling
                                    Layout.fillWidth: true
                                    spacing: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.spaceSm
                                        Layout.bottomMargin: Theme.spaceSm
                                        spacing: Theme.spaceMd
                                        // the REAL cassette (playlists
                                        // shelf), rendered native and
                                        // scaled down to a row icon
                                        Item {
                                            Layout.alignment: Qt.AlignVCenter
                                            implicitWidth: 60
                                            implicitHeight: 38
                                            CassetteTile {
                                                width: 176; height: 112
                                                scale: 60 / 176
                                                transformOrigin: Item.TopLeft
                                                name: mixRow.modelData.name !== undefined
                                                      ? mixRow.modelData.name : ""
                                                accents: mixRow.accents
                                                labelArt: mixRow.artUrls.length > 0
                                                          ? mixRow.artUrls[0] : ""
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignVCenter
                                            spacing: 1
                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.name !== undefined ? modelData.name : ""
                                                font.pixelSize: 13
                                                color: Theme.textPrimary
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: qsTr("%1 tracks").arg(
                                                      modelData.count !== undefined ? modelData.count : 0)
                                                      + (mixRow.missing > 0
                                                         ? qsTr(" · %1 not in library").arg(mixRow.missing)
                                                         : "")
                                                font.pixelSize: 11; font.weight: Font.Light
                                                color: Theme.textDim
                                            }
                                        }
                                        // mock ".sy"/".ns" — pulling while an
                                        // import fills in; blank when unplugged
                                        Text {
                                            text: mixRow.pullingNow ? qsTr("pulling…")
                                                  : !DeviceService.connected ? ""
                                                  : onZune ? qsTr("on zune") : qsTr("not synced")
                                            font.pixelSize: 11; font.weight: Font.Light
                                            color: onZune && !mixRow.pullingNow
                                                   ? Theme.green : Theme.textGhost
                                        }
                                    }
                                    Rectangle {
                                        visible: index < LibraryService.playlists.length - 1
                                        Layout.fillWidth: true
                                        implicitHeight: 1
                                        color: Theme.separator
                                    }
                                }
                            }
                        }
                        Text {
                            visible: LibraryService.playlists.length === 0
                            Layout.topMargin: Theme.spaceXs
                            text: qsTr("no mixtapes yet — build one from the tray")
                            font.pixelSize: 14; font.weight: Font.Light
                            color: Theme.textSecondary
                        }
                    }

                    Sblk {
                        title: qsTr("Import & export")
                        desc: qsTr("move mixtapes as .m3u — the escape hatch from our schema.")
                        divider: false
                        RowLayout {
                            spacing: Theme.spaceMd
                            GlassButton {
                                text: qsTr("export all (.m3u)")
                                disabled: LibraryService.playlists.length === 0
                                onClicked: {
                                    const dir = LibraryService.exportPlaylistsM3U()
                                    toastHost.show("mixtapes exported to " + dir)
                                }
                            }
                            GlassButton {
                                text: qsTr("import a playlist…")
                                onClicked: m3uImportDialog.open()
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // ── HEALTH ──
                ColumnLayout {
                    id: tabHealth
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: stack.currentIndex === 4
                    spacing: Theme.spaceXl

                    DiagnosticsSettings {
                        Layout.fillWidth: true
                        Layout.maximumWidth: Theme.artworkSettingsMaxWidth
                        onNotification: function(message) { toastHost.show(message) }
                    }

                    Sblk {
                        title: qsTr("Status")
                        desc: qsTr("everything zuuned needs to run right.")

                        // Watch folders reachable — total minus any failing
                        // folder checks the scanner reports.
                        StatRow {
                            readonly property int total: LibraryService.watchFolders.length
                            readonly property int failing: {
                                void LibraryService.watchFolders
                                return LibraryService.healthChecks()
                                    .filter(c => c.id === "folder").length
                            }
                            label: qsTr("Watch folders reachable")
                            value: qsTr("%1 of %2").arg(total - failing).arg(total)
                            dot: failing === 0 ? Theme.green : Theme.warning
                        }
                        StatRow {
                            readonly property double gb: LibraryService.tempFreeGB()
                            label: qsTr("Temp space for transcodes")
                            value: gb < 0 ? qsTr("unknown")
                                          : qsTr("%1 GB free").arg(gb.toFixed(gb < 100 ? 1 : 0))
                            dot: gb < 0 ? Theme.textDim
                                        : (gb >= 2 ? Theme.green : Theme.warning)
                        }
                        StatRow {
                            visible: DeviceService.udevRulesApply
                            label: qsTr("USB rule installed")
                            value: DeviceService.udevRuleOk ? qsTr("installed") : qsTr("missing or outdated")
                            dot: DeviceService.udevRuleOk ? Theme.green : Theme.warning
                        }
                        // Files a folder holds that its declared type will not
                        // index. The scan counted them once per root and the
                        // point of showing them is the setting that fixes it:
                        // a Videos page with one of five videos in it is
                        // otherwise indistinguishable from a Videos page with
                        // the wrong folder in it.
                        Repeater {
                            model: LibraryService.mediaTypeNotes
                            delegate: StatRow {
                                required property var modelData
                                label: ZPathText.display(modelData.folder)
                                readonly property string wrongKind:
                                    (modelData.videos > 0 ? qsTr("%1 video ").arg(modelData.videos) : "")
                                    + (modelData.photos > 0 ? qsTr("%1 photo ").arg(modelData.photos) : "")
                                    + (modelData.audio > 0 ? qsTr("%1 audio ").arg(modelData.audio) : "")
                                readonly property string underFloor:
                                    modelData.sizeSkipped > 0
                                    ? qsTr("%1 video under the size floor — set as download folder")
                                          .arg(modelData.sizeSkipped)
                                    : ""
                                // Two complaints, because a folder that holds the
                                // wrong kind of media and a folder whose size
                                // guess said no are fixed by two different things.
                                value: wrongKind
                                     ? wrongKind + qsTr("held — type \"%1\" skips them").arg(root.typeLabel(modelData.type))
                                       + (underFloor ? " · " + underFloor : "")
                                     : underFloor
                                valueColor: Theme.warning
                                dot: Theme.warning
                            }
                        }
                        StatRow {
                            label: qsTr("Zune connected")
                            value: DeviceService.connected ? qsTr("ready") : qsTr("unplugged")
                            dot: DeviceService.connected ? Theme.green : Theme.warning
                            divider: false
                        }
                    }

                    Sblk {
                        title: qsTr("Library")
                        desc: qsTr("what's indexed right now.")
                        StatRow { label: qsTr("Tracks"); value: LibraryService.trackCount
                                  valueColor: Theme.textPrimary }
                        StatRow { label: qsTr("Videos"); value: LibraryService.videoCount
                                  valueColor: Theme.textPrimary }
                        StatRow { label: qsTr("Photos"); value: LibraryService.photoCount
                                  valueColor: Theme.textPrimary; divider: false }
                    }

                    Sblk {
                        title: qsTr("Backups & tour")
                        desc: qsTr("keep your library safe; see the tour again.")

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            spacing: Theme.spaceMd
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: qsTr("Back up library DB")
                                    font.pixelSize: 14; font.weight: Font.Light
                                    color: Theme.textPrimary
                                }
                                Text {
                                    id: backupWhen
                                    property string label: LibraryService.lastBackupLabel()
                                    text: label.length > 0
                                      ? qsTr("last backup: %1").arg(label) : qsTr("never backed up")
                                    font.pixelSize: 11; font.weight: Font.Light
                                    color: Theme.textDim
                                }
                            }
                            GlassButton {
                                text: qsTr("back up now")
                                onClicked: {
                                    const when = LibraryService.backupDatabase()
                                    if (when.length > 0) {
                                        backupWhen.label = when
                                        toastHost.show("library backed up")
                                    } else {
                                        toastHost.show("backup failed")
                                    }
                                }
                            }
                        }

                        // mock ".foldrow" hairline between the two rows
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: Theme.separator
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: qsTr("Replay the walkthrough")
                                    font.pixelSize: 14; font.weight: Font.Light
                                    color: Theme.textPrimary
                                }
                                Text {
                                    text: qsTr("the first-run tour")
                                    font.pixelSize: 11; font.weight: Font.Light
                                    color: Theme.textDim
                                }
                            }
                            GlassButton {
                                text: qsTr("show me")
                                onClicked: Window.window.showOnboarding = true
                            }
                        }
                    }

                    Sblk {
                        title: qsTr("About")
                        divider: true
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Zuuned %1 · %2 · your data stays local.")
                                      .arg(Qt.application.version || qsTr("development build"))
                                      .arg(Qt.platform.os === "windows" ? "windows" : "linux")
                            font.pixelSize: 13; font.weight: Font.Light
                            color: Theme.textDim
                        }
                    }

                    Sblk {
                        title: qsTr("Powered by")
                        divider: false
                        RowLayout {
                            spacing: Theme.spaceXxl
                            Text {
                                text: "TMDB"
                                font.pixelSize: 14; font.weight: Font.Bold
                                color: Qt.rgba(0.0, 0.71, 0.89, 1)
                            }
                            Text {
                                text: "MusicBrainz"
                                font.pixelSize: 14; font.weight: Font.Bold
                                color: Qt.rgba(0.73, 0.28, 0.56, 1)
                            }
                            Text {
                                text: qsTr("fanart.tv")
                                font.pixelSize: 14; font.weight: Font.Bold
                                color: Qt.rgba(0.04, 0.55, 1.0, 1)
                            }
                        }
                        CaptionText {
                            text: qsTr("This product uses the TMDB API but is not endorsed or certified by TMDB. Music metadata powered by MusicBrainz and AcoustID. Art powered by Fanart.tv and the Cover Art Archive.")
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    SpokenFolderImport { id: audiobookFolderImport }

    FolderDialog {
        id: mediaFolderDialog
        property string pendingType: "music"
        title: qsTr("Choose a %1 folder").arg(root.typeLabel(pendingType))
        onAccepted: {
            console.warn("[settings] folder accepted:", selectedFolder)
            // canonical(), not a hand-stripped scheme: file:///E:/Music has to
            // become E:/Music.  Chopping "file://" leaves "/E:/Music", which is
            // a valid absolute path on Linux and nothing at all on Windows.
            const p = ZPathText.canonical(selectedFolder.toString())
            if (p.length > 1)
                LibraryService.addWatchFolder(p, pendingType)
        }
        onRejected: console.warn("[settings] folder dialog rejected/cancelled")

    }
    // The FFmpeg folder picked in Settings → Device.  canonical() for the same
    // reason the watch-folder dialog needs it: file:///E:/Tools/ffmpeg/bin is a
    // URL, and the value that has to land in the ini is a path.
    FolderDialog {
        id: ffmpegFolderDialog
        title: qsTr("Choose the folder holding ffmpeg and ffprobe")
        onAccepted: {
            const p = ZPathText.canonical(selectedFolder.toString())
            if (p.length > 1)
                AppSettings.ffmpegDir = p
            audioConversionCard.probeRevision++
        }
        onRejected: console.warn("[settings] ffmpeg folder dialog cancelled")
    }

    // Treat-As menu (shared across watch-folder rows)
    Popup {
        id: treatAsPopup
        property double folderId: -1
        padding: 4
        background: Rectangle {
            radius: Theme.radiusMd
            color: Theme.surfaceBg
            border.color: Theme.borderLight
            border.width: 1
        }
        contentItem: Column {
            spacing: 2
            Repeater {
                model: ["music", "movies", "tv", "anime", "photos", "all"]
                Rectangle {
                    required property string modelData
                    width: 120
                    height: 26
                    radius: Theme.radiusSm
                    color: taOptArea.containsMouse ? Theme.rowHoverPink : "transparent"
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        // The Treat-As menu offers folder keys and must show
                        // words: the key is still what gets sent, the label is
                        // the language file's.
                        text: root.typeLabel(modelData)
                        font.pixelSize: 12
                        font.weight: Font.Light
                        color: root.typeColor(modelData)
                    }
                    MouseArea {
                        id: taOptArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            LibraryService.setWatchFolderType(treatAsPopup.folderId, modelData)
                            treatAsPopup.close()
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: wallpaperDialog
        title: qsTr("Choose a background image")
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.bmp)"]
        onAccepted: {
            // Store a plain local path (mac stores url.path); the mac also
            // resets opacity to 0.3 on every new pick — match that.
            const path = ZPathText.canonical(selectedFile.toString())
            AppSettings.backgroundImagePath = path
            AppSettings.backgroundOpacity = 0.3
        }
    }

    // Mixtapes: import a plain .m3u playlist
    FileDialog {
        id: m3uImportDialog
        title: qsTr("Import a playlist (.m3u)")
        nameFilters: ["Playlists (*.m3u *.m3u8)"]
        onAccepted: {
            const res = LibraryService.importPlaylistM3U(selectedFile.toString())
            if (res && res.name !== undefined)
                toastHost.show("imported “" + res.name + "”")
            else
                toastHost.show("playlist imported")
        }
    }
}
