#!/usr/bin/env bash
# Make the build-tree bundle a self-contained application a tester can carry, and
# build the two archives that carry it.
#
# macOS has no install rules in `CMakeLists.txt` -- a .desktop entry, an hicolor
# tree and a udev rule have no counterpart here -- so what the build produces is a
# bundle whose binary still resolves QtCore, libusb, libmpv and every FFmpeg
# component through `/opt/homebrew`. That bundle runs on the machine that built it
# and nowhere else, which is not a candidate. This script is the counterpart:
# stage, copy the Qt and Homebrew Mach-O files inside the bundle, prune what
# cannot load here, say each Mach-O its real name, sign what was rewritten, and
# prove the result with a launch that fails if dyld maps anything outside.
#
# Artifacts land in `_tmp` (gitignored): `zuuned.app`, a `.dmg` and a `.zip`.
#
# Usage: packaging/macos/deploy.sh [--no-smoke]
set -euo pipefail
# Two levels, not one: this script sits in `packaging/macos/`, one deeper than the
# AppImage builder it takes its shape from, and `packaging/macos/..` is `packaging`
# -- which is where a bundle named relative to the repository root does not exist.
cd "$(dirname "$0")/../.."

zuuned_build=${ZUUNED_MAC_BUILD:-build-macos-release}
zuuned_out=${ZUUNED_MAC_OUT:-_tmp}
zuuned_smoke=1
for zuuned_arg in "$@"; do
    case "$zuuned_arg" in
        --no-smoke) zuuned_smoke=0 ;;
        --help | -h) echo "usage: $0 [--no-smoke]"; exit 0 ;;
        *) echo "unknown argument: $zuuned_arg (try --no-smoke)" >&2; exit 2 ;;
    esac
done

[[ $(uname -s) == Darwin ]] || { echo "this is the macOS packager; $(uname -s) is not macOS" >&2; exit 1; }
[[ -d $zuuned_build/zuuned.app ]] || {
    echo "no bundle at $zuuned_build/zuuned.app -- build it first (BUILDING.md, Build on macOS)" >&2; exit 1; }

zuuned_macdeployqt=""
for zuuned_candidate in \
        "$(/opt/homebrew/bin/brew --prefix qtbase 2>/dev/null || true)/bin/macdeployqt" \
        "$(/usr/local/bin/brew --prefix qtbase 2>/dev/null || true)/bin/macdeployqt" \
        /opt/homebrew/opt/qtbase/bin/macdeployqt \
        /usr/local/opt/qtbase/bin/macdeployqt; do
    if [[ -x $zuuned_candidate ]]; then zuuned_macdeployqt=$zuuned_candidate; break; fi
done
[[ -n $zuuned_macdeployqt ]] || { echo "macdeployqt not found; brew install qtbase" >&2; exit 1; }
zuuned_qt_prefix=$(dirname "$(dirname "$zuuned_macdeployqt")")

zuuned_version=$(sed -n 's/^project(zuuned VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
zuuned_arch=$(uname -m)
zuuned_stage="$zuuned_out/macos"
zuuned_log="$zuuned_stage/logs"
zuuned_published="$zuuned_stage/zuuned.app"
# The bundle is assembled, rewritten and signed in a scratch directory outside this
# checkout, and copied in when it is finished. That is measured necessity, not
# tidiness. This repository sits under `~/Documents`, whose Desktop-and-Documents
# sync is owned by iCloud's file provider, and that provider stamps
# `com.apple.FinderInfo` and `com.apple.fileprovider.fpfs#P` onto every *package*
# directory -- `.app`, `.framework`, `.bundle` -- within about four seconds of one
# appearing, and writes `FinderInfo` back within two seconds of its deletion.
# `codesign` refuses to sign a bundle wearing that attribute, so signing in place is
# a race against a background daemon, and three runs of this script lost it the same
# way: 169 Mach-O files signed, then a `QtQuickControls2.framework` directory said
# no, and clearing first could not be made to stick. A fresh `.framework`,
# `.bundle` and plain directory created here next to identical ones under `$TMPDIR`
# acquired the attribute in the tree this repository lives in and never acquired it
# there. So everything that seals a byte -- the deploy, the id rewrite, the
# signatures, the two archives -- happens in that scratch tree; `_tmp` keeps the
# logs, the finished bundle and the artifacts, and the launch below reads the copy
# in `_tmp` because that is the copy a tester runs.
zuuned_work=$(mktemp -d "${TMPDIR:-/tmp}/zuuned-deploy.XXXXXX") || {
    echo "could not make a scratch directory under ${TMPDIR:-/tmp}" >&2; exit 1; }
trap 'rm -rf "$zuuned_work"' EXIT
zuuned_app="$zuuned_work/zuuned.app"
# The load commands are read from `otool` output below and counted by columns. A
# bundle path with a space in it makes every column wrong and every verdict a lie,
# so refuse it here rather than sign a bundle that was never actually read.
[[ $zuuned_app != *[[:space:]]* && $zuuned_published != *[[:space:]]* ]] || {
    echo "refusing to package from a path containing spaces: $zuuned_app" >&2; exit 1; }

echo "== stage: $zuuned_build/zuuned.app -> $zuuned_app"
echo "scratch=$zuuned_work"
rm -rf "$zuuned_stage"
mkdir -p "$zuuned_app" "$zuuned_log"
# `rsync -a` and not `ditto`: ditto preserves the extended attributes a checkout
# accumulates, and `codesign` refuses to sign a bundle carrying them ("resource
# fork, Finder information, or similar detritus not allowed"). Measured here.
rsync -a --delete "$zuuned_build/zuuned.app/" "$zuuned_app/"

# The build-tree `qt.conf` says `QmlImports = <absolute build directory>`, which is
# a convenience for the developer who built it and a broken path for everyone else.
# It can go rather than be rewritten because `qt_add_qml_module(zuuned URI Zuuned)`
# compiles this application's QML into the executable: a copy of the bundle with
# the file deleted still paints every page, and the Qt modules deployed below land
# in Contents/Resources/qml, which is on Qt's default import path for a bundle.
rm -f "$zuuned_app/Contents/Resources/qt.conf"

echo "== deploy: $zuuned_macdeployqt"
# -qmldir scans the sources, because the imports the QML makes are the only
# description of which Qt Quick modules are needed at run time.
# -no-strip keeps the symbols a tester's crash report would otherwise be missing.
# -no-codesign because signing happens below, after the prune and the icon have
# changed the bundle; macdeployqt also signs before it is finished, and fails.
if ! "$zuuned_macdeployqt" "$zuuned_app" -qmldir="$PWD/qml" -no-strip -no-codesign \
        > "$zuuned_log/macdeployqt.log" 2>&1; then
    echo "macdeployqt failed; see $zuuned_log/macdeployqt.log" >&2
    rg '^ERROR' "$zuuned_log/macdeployqt.log" | head -20 >&2 || true
    exit 1
fi
rg '^ERROR' "$zuuned_log/macdeployqt.log" | head -6 || true

# Which files in the bundle can be Mach-O at all, said once so that every pass
# below reads the same set. A framework binary is `Name.framework/Versions/A/Name`,
# everything else this project ships that is code is a `.dylib`, and the executable
# lives in `Contents/MacOS`. Of the ~1,900 files in a deployed bundle only 169 are
# candidates; the rest is QML, `.qmldir`, translations and `PrivacyInfo.xcprivacy`,
# and asking `otool` or `codesign` about those costs a minute and prints nothing.
zuuned_machos() {
    find "$zuuned_app" -type f \
        \( -path '*/Contents/MacOS/*' -o -path '*/Versions/A/*' -o -name '*.dylib' \) \
        ! -name '*.plist' ! -name '*.xcprivacy' ! -name '*.json' -print0 2>/dev/null || true
}

# A plugin whose Qt framework is absent cannot load on any machine, and Qt tries it
# anyway and logs a failure for it. Drop those and name them: the Homebrew Qt here
# ships no QtPdf, and `qmlimportscanner` hands back the two plugins that import it.
# The reference counts only where it names a Qt framework, because the same list
# carries plain dylibs, `@executable_path/../Frameworks/...` and system frameworks,
# none of which say anything about a missing Qt module. The first run of this script
# asked only "is this path there" and deleted the cocoa platform plugin over a
# reference to `/System/Library/Frameworks/Foundation`, which is the difference
# between a bundle that prunes two files and a bundle that cannot open a window.
zuuned_pruned=""
while IFS= read -r -d '' zuuned_plugin; do
    for zuuned_ref in $(otool -L "$zuuned_plugin" 2>/dev/null | awk 'NR > 1 { print $1 }'); do
        [[ $zuuned_ref =~ ([Qq]t[A-Za-z0-9_]*\.framework/Versions/[^[:space:]]+) ]] || continue
        zuuned_framework=${BASH_REMATCH[1]}
        if [[ ! -e "$zuuned_app/Contents/Frameworks/$zuuned_framework" ]] \
           && [[ ! -e "$zuuned_qt_prefix/lib/$zuuned_framework" ]]; then
            rm -f "$zuuned_plugin"
            zuuned_pruned="$zuuned_pruned $(basename "$zuuned_plugin")($zuuned_framework)"
            break
        fi
    done
done < <(find "$zuuned_app/Contents/PlugIns" -type f -name '*.dylib' -print0 2>/dev/null || true)
echo "pruned:${zuuned_pruned:- nothing}"

# Pruning names a file under `Contents/PlugIns`, and for a QML plugin that file is
# the half of a symlink: `macdeployqt` leaves a link to it inside the module
# directory under `Contents/Resources/qml`. A dangling link stops `xattr -cr` outright
# -- the first run died there with
# `xattr: No such file: .../Resources/qml/QtQuick/Pdf/libpdfquickplugin.dylib` --
# and it would survive into a copy of the bundle for a tester to find. So every link
# in the bundle that no longer resolves goes, by whenever it was stranded.
zuuned_dangling=""
while IFS= read -r -d '' zuuned_link; do
    rm -f "$zuuned_link"
    zuuned_dangling="$zuuned_dangling ${zuuned_link#"$zuuned_app/"}"
done < <(find "$zuuned_app" -type l ! -exec test -e {} ';' -print0 2>/dev/null || true)
echo "dangling_links_removed:${zuuned_dangling:- nothing}"

# What a file calls itself, as distinct from the paths that point at it. Measured
# here: `macdeployqt` rewrites every *reference* -- zero `/opt/homebrew` references
# survive it -- and leaves the 42 frameworks it copied out of `qtdeclarative` still
# naming themselves `/opt/homebrew/opt/qtdeclarative/lib/...`. dyld resolves a
# dependency by the path in the load command plus the rpath list, not by the
# dependency's own id, so this is not why the bundle runs; it is why `otool` could
# not tell a carried bundle from a Homebrew install, and a stale id is what makes a
# later `-change` silently miss. So give each file the name it now answers to, and
# drop the `/opt/homebrew/lib` rpath CMake left on the executable, which is an
# escape hatch that would let one missed reference load the developer's copy.
echo "== install names: ids inside the bundle, Homebrew rpaths out"
zuuned_rewritten=0
zuuned_rpaths_dropped=0
while IFS= read -r -d '' zuuned_macho; do
    zuuned_loads=$(otool -l "$zuuned_macho" 2>/dev/null) || continue
    zuuned_self=$(printf '%s\n' "$zuuned_loads" | awk '
        /cmd LC_ID_DYLIB/ { want = 1; next } want && $1 == "name" { print $2; exit }')
    case "$zuuned_self" in
        /opt/homebrew/* | /usr/local/*)
            if [[ $(dirname "$zuuned_macho") == "$zuuned_app/Contents/Frameworks"* ]]; then
                zuuned_new="@rpath/${zuuned_macho#"$zuuned_app/Contents/Frameworks/"}"
            else
                zuuned_new="@loader_path/$(basename "$zuuned_macho")"
            fi
            install_name_tool -id "$zuuned_new" "$zuuned_macho"
            zuuned_rewritten=$((zuuned_rewritten + 1)) ;;
    esac
    # Only where a path inside the bundle is already on the rpath list, so this can
    # never strand a reference that had no other way to resolve; the check below and
    # the launch after it are both watching for that anyway. A framework carries
    # `@loader_path/../../../`, which is `Contents/Frameworks` seen from inside the
    # version directory, and the executable carries
    # `@executable_path/../Frameworks` -- either one qualifies.
    zuuned_rpaths=$(printf '%s\n' "$zuuned_loads" | awk '
        /cmd LC_RPATH/ { want = 1; next } want && $1 == "path" { print $2; want = 0 }')
    if printf '%s\n' "$zuuned_rpaths" | grep -qE '^@(executable_path|loader_path)/'; then
        while IFS= read -r zuuned_rpath; do
            case "$zuuned_rpath" in
                /opt/homebrew/* | /usr/local/*)
                    install_name_tool -delete_rpath "$zuuned_rpath" "$zuuned_macho" || continue
                    zuuned_rpaths_dropped=$((zuuned_rpaths_dropped + 1)) ;;
            esac
        done <<< "$zuuned_rpaths"
    fi
done < <(zuuned_machos)
echo "install_names_rewritten=$zuuned_rewritten homebrew_rpaths_dropped=$zuuned_rpaths_dropped"

# The structural gate. Every Mach-O in the bundle has to reach every other one
# through `@executable_path`/`@loader_path`/`@rpath`, and every one of those has to
# land on a file that is really in the bundle. One `/opt/homebrew` load command left
# behind means an archive that works on the build host and fails on a tester's Mac.
# `otool -L` prints the file's own id as its first row, which is not a dependency,
# and the pass above deliberately leaves ids that are no longer even a load path --
# so the ids are read separately with `otool -D` and subtracted, or the gate reports
# 42 files it has just renamed. That false alarm is how this gate first failed.
echo "== check: load commands"
zuuned_ids="$zuuned_log/ids.tsv"
zuuned_refs="$zuuned_log/references.tsv"
zuuned_machos | xargs -0 -n 40 otool -D 2>/dev/null | awk '
    /:$/ { f = $1; sub(/:$/, "", f); next } NF { print f "\t" $1 }' \
    > "$zuuned_ids" || true
zuuned_machos | xargs -0 -n 40 otool -L 2>/dev/null | awk -v ids="$zuuned_ids" '
    FILENAME == ids { self[$1] = $2; next }
    /^[^ \t]/ { f = $1; sub(/:$/, "", f); next }
    NF { if ($1 != self[f]) print f "\t" $1 }' "$zuuned_ids" - > "$zuuned_refs" || true
zuuned_checked=0
zuuned_bad=0
while IFS=$'\t' read -r zuuned_macho zuuned_ref; do
    [[ -n $zuuned_ref ]] || continue
    zuuned_checked=$((zuuned_checked + 1))
    zuuned_target=""
    case "$zuuned_ref" in
        # @executable_path is the directory of the main executable even when the
        # asking file is a plugin, and @loader_path is the directory of the asker.
        @executable_path/*) zuuned_target="$zuuned_app/Contents/MacOS/${zuuned_ref#@executable_path/}" ;;
        @loader_path/*)     zuuned_target="$(dirname "$zuuned_macho")/${zuuned_ref#@loader_path/}" ;;
        @rpath/*)           zuuned_target="$zuuned_app/Contents/Frameworks/${zuuned_ref#@rpath/}" ;;
        /usr/* | /System/* | /Library/Apple/*) continue ;;
        /*) echo "outside the bundle: $zuuned_ref in $zuuned_macho" >&2; zuuned_bad=1; continue ;;
        *) echo "not a load path: $zuuned_ref in $zuuned_macho" >&2; zuuned_bad=1; continue ;;
    esac
    if [[ ! -e $zuuned_target ]]; then
        # An @rpath reference is searched against every rpath the binary carries,
        # and a Qt plugin is deployed with Contents/PlugIns on that list too.
        [[ $zuuned_ref == @rpath/* && -e "$zuuned_app/Contents/PlugIns/${zuuned_ref#@rpath/}" ]] && continue
        echo "unresolved: $zuuned_ref in $zuuned_macho" >&2
        zuuned_bad=1
    fi
done < "$zuuned_refs"
# A gate that read nothing has proved nothing. If `otool` failed outright the loop
# above would have zero rows to disagree with, so say how many there were.
[[ $zuuned_checked -ge 100 ]] || {
    echo "only $zuuned_checked load commands read from $(wc -l < "$zuuned_ids") Mach-O ids; otool failed" >&2
    exit 1; }
[[ $zuuned_bad == 0 ]] || { echo "the bundle is not self-contained" >&2; exit 1; }
echo "load_commands_checked=$zuuned_checked"
echo "self-contained=yes"

# The bundle icon, from the hicolor PNGs the Linux package already installs. The
# iconset wants a 1024-pixel tile; the deepest committed tile is 512, so that one
# is enlarged and every other tile is the file as authored.
echo "== icon: packaging/icons/hicolor -> Zuuned.icns"
zuuned_iconset="$zuuned_stage/zuuned.iconset"
rm -rf "$zuuned_iconset"; mkdir -p "$zuuned_iconset"
for zuuned_size in 16 32 128 256 512; do
    zuuned_src="packaging/icons/hicolor/${zuuned_size}x${zuuned_size}/apps/zuuned.png"
    [[ -f $zuuned_src ]] || { echo "missing $zuuned_src" >&2; exit 1; }
    cp "$zuuned_src" "$zuuned_iconset/icon_${zuuned_size}x${zuuned_size}.png"
    zuuned_double=$((zuuned_size * 2))
    if [[ $zuuned_double -le 512 ]]; then
        cp "packaging/icons/hicolor/${zuuned_double}x${zuuned_double}/apps/zuuned.png" \
           "$zuuned_iconset/icon_${zuuned_size}x${zuuned_size}@2x.png"
    else
        sips -z 1024 1024 "$zuuned_src" --out "$zuuned_iconset/icon_512x512@2x.png" >/dev/null
    fi
done
iconutil -c icns "$zuuned_iconset" -o "$zuuned_app/Contents/Resources/Zuuned.icns"
/usr/libexec/PlistBuddy -c "Delete :CFBundleIconFile" "$zuuned_app/Contents/Info.plist" >/dev/null 2>&1 || true
/usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string Zuuned.icns" "$zuuned_app/Contents/Info.plist"

# Apple silicon refuses to run an unsigned Mach-O, and `install_name_tool` -- what
# the deploy step used to rewrite every path -- invalidates the signature of what it
# edited. So sign, inside-out, ending with the bundle. Ad-hoc (`-s -`) and not
# hardened: there is no Apple Developer account behind this project, which is the
# same reason there is no DriverKit backend, and an ad-hoc signature buys nothing
# across a download. What a tester does about Gatekeeper is in BUILDING.md.
# Extended attributes come off first, because `codesign` refuses to sign anything
# wearing them -- "resource fork, Finder information, or similar detritus not
# allowed" -- and a checkout, `macdeployqt` and a Finder that has glanced at the
# folder leave plenty. Two things are measured here rather than assumed.
# `com.apple.provenance` lands on every file, cannot be deleted, and does not bother
# `codesign`, so the check names only the two kinds that do. And a recursive clear is
# not sufficient on its own: with `CFBundleIconFile` freshly written into the bundle's
# `Info.plist`, the `zuuned.app` directory was found carrying `com.apple.FinderInfo`
# a second after `xattr -cr` had cleared it, and `codesign --verbose=4` named the
# bundle root -- "Disallowed xattr com.apple.FinderInfo found on .../zuuned.app" --
# not the file it was asked to sign. Whatever stamps that attribute does it
# asynchronously, so a clear can be stale before the first signature lands, which is
# why the bundle is signed in the scratch tree named above -- where nothing stamps
# it -- and not because clearing was ever made reliable. Clear, then read the bundle
# back; if something survives, say so rather than sign 200 files and lose them all to
# one directory's attribute.
zuuned_clear_detritus() {
    xattr -cr "$zuuned_app" 2>/dev/null || true
    find "$zuuned_app" -mindepth 0 -exec xattr -d com.apple.FinderInfo {} ';' 2>/dev/null || true
    find "$zuuned_app" -mindepth 0 -exec xattr -d com.apple.ResourceFork {} ';' 2>/dev/null || true
    local zuuned_left
    zuuned_left=$(xattr -r "$zuuned_app" 2>/dev/null \
        | awk -F': ' '$2 ~ /^com\.apple\.(FinderInfo|ResourceFork)$/ { print $1 }' | sort -u)
    [[ ${zuuned_left//[[:space:]]/} ]] || return 0
    echo "still carrying FinderInfo or a resource fork:" >&2
    printf '%s\n' "$zuuned_left" >&2
    exit 1
}

# One path, with one retry aimed at the race above and nothing else: a signature that
# failed for detritus clears the bundle and tries that same path again. Anything else,
# or a second detritus failure, prints what `codesign` said and stops the run -- the
# `--verify` below is not asked to be the only witness that signing worked.
zuuned_sign() {
    if codesign --force --sign - "$1" > "$zuuned_log/last-sign.log" 2>&1; then
        return 0
    fi
    if rg -q 'detritus|FinderInfo' "$zuuned_log/last-sign.log"; then
        zuuned_clear_detritus
        if codesign --force --sign - "$1" >> "$zuuned_log/last-sign.log" 2>&1; then
            return 0
        fi
    fi
    cat "$zuuned_log/last-sign.log" >&2
    return 1
}

echo "== sign: ad-hoc, inside-out"
zuuned_clear_detritus
zuuned_signed=0
while IFS= read -r -d '' zuuned_macho; do
    if file -bN "$zuuned_macho" 2>/dev/null | rg -q 'Mach-O'; then
        zuuned_sign "$zuuned_macho"
        zuuned_signed=$((zuuned_signed + 1))
    fi
done < <(zuuned_machos)
while IFS= read -r -d '' zuuned_framework; do
    zuuned_sign "$zuuned_framework"
done < <(find "$zuuned_app/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0 2>/dev/null || true)
zuuned_sign "$zuuned_app"
echo "signed_macho=$zuuned_signed"
if codesign --verify --verbose=2 "$zuuned_app" > "$zuuned_log/codesign.log" 2>&1; then
    echo "signature=ok"
else
    echo "signature=fail" >&2; tail -20 "$zuuned_log/codesign.log" >&2; exit 1
fi
# Recorded, never a failure: an ad-hoc bundle is expected to be refused, and a
# tester follows the Gatekeeper instruction rather than this line.
if spctl -a -t exec -vv "$zuuned_app" > "$zuuned_log/gatekeeper.log" 2>&1; then
    echo "gatekeeper=accepted"
else
    echo "gatekeeper=rejected (expected for an ad-hoc, unnotarised build)"
fi

# Everything above sealed its bytes in the scratch tree. Copy the finished bundle
# into `_tmp`, because that is the path BUILDING.md gives a developer and the path a
# tester's report names. `rsync -a` carries no extended attributes, so the copy is
# the signed bytes and nothing else; the attribute the file provider may put back on
# the copy belongs to this machine's Finder, is not part of any seal, and does not
# stop dyld, which validates Mach-O and not directory metadata.
echo "== publish: $zuuned_app -> $zuuned_published"
mkdir -p "$(dirname "$zuuned_published")"
rsync -a --delete "$zuuned_app/" "$zuuned_published/"

# The launch that proves the structure. `DYLD_PRINT_LIBRARIES` names every image
# dyld maps, so a framework that quietly resolved back to Homebrew is caught here
# even when the load commands looked clean, and `--qa-probe` is the application
# reporting the page it actually rendered rather than the one it was asked for.
if [[ $zuuned_smoke == 1 ]]; then
    echo "== smoke: launch the published bundle and read what it painted"
    zuuned_profile="$zuuned_stage/profile"
    rm -rf "$zuuned_profile"; mkdir -p "$zuuned_profile/Zuuned"
    printf '[General]\nonboardingComplete=true\n' > "$zuuned_profile/Zuuned/Zuuned.ini"
    zuuned_smoke_log="$zuuned_log/smoke.log"
    : > "$zuuned_smoke_log"
    ZUUNED_PROFILE_DIR="$zuuned_profile" DYLD_PRINT_LIBRARIES=1 \
        "$zuuned_published/Contents/MacOS/zuuned" --page music --qa-probe \
        > "$zuuned_smoke_log" 2>&1 &
    zuuned_smoke_pid=$!
    zuuned_seen=""
    for _ in $(seq 1 60); do
        if rg -q '\[qa\] tick1 ' "$zuuned_smoke_log"; then zuuned_seen=tick1; break; fi
        kill -0 "$zuuned_smoke_pid" 2>/dev/null || { zuuned_seen=exited; break; }
        sleep 1
    done
    kill -TERM "$zuuned_smoke_pid" 2>/dev/null || true
    sleep 2
    kill -KILL "$zuuned_smoke_pid" 2>/dev/null || true
    # Measured on this machine, dyld answers `DYLD_PRINT_LIBRARIES` with one
    # `dyld[pid]: <UUID> /path` line per image. Requiring those lines to be there at
    # all is what turns "no Homebrew path" from a silence into a fact: were the
    # variable ever ignored or stripped, the check below would pass having mapped
    # nothing, and a bundle that only runs because Homebrew is installed would ship.
    if [[ $zuuned_seen != tick1 ]]; then
        echo "the published bundle never reported a rendered page ($zuuned_seen); see $zuuned_smoke_log" >&2
        tail -25 "$zuuned_smoke_log" >&2
        exit 1
    fi
    rg -o '\[qa\] tick1 .*' "$zuuned_smoke_log" | head -1
    zuuned_mapped=$(rg -c '^dyld\[[0-9]+\]: <[0-9A-Fa-f-]+> /' "$zuuned_smoke_log" || true)
    [[ ${zuuned_mapped:-0} -ge 50 ]] || {
        echo "dyld named ${zuuned_mapped:-0} images; DYLD_PRINT_LIBRARIES did not run," >&2
        echo "so nothing below could have proved where they came from" >&2
        exit 1; }
    if rg -q '^dyld\[[0-9]+\]: <[^>]*> (/opt/homebrew|/usr/local)/' "$zuuned_smoke_log"; then
        echo "dyld mapped a library outside the bundle:" >&2
        rg -o '^dyld\[[0-9]+\]: <[^>]*> (/opt/homebrew|/usr/local)/.*' "$zuuned_smoke_log" \
            | sed -n 's/^dyld\[[0-9]*\]: <[^>]*> //p' | head -5 >&2
        exit 1
    fi
    echo "smoke=rendered; $zuuned_mapped images mapped and none of them outside the bundle"
fi

# The two archives, because the browser path and the disk-image path are different
# Gatekeeper experiences; the zip also survives transport tools that flatten a dmg.
echo "== archive"
zuuned_vol="Zuuned-$zuuned_version-$zuuned_arch"
# The volume's contents come from the scratch tree and not from the copy in `_tmp`,
# for the reason that made this script use a scratch tree at all: `hdiutil` and
# `ditto` copy extended attributes the way `rsync -a` does not, so staging the
# volume inside a file-provider directory would hand every tester the
# `com.apple.FinderInfo` that `codesign` refuses, on its way to a machine that may
# not be able to clear it. The image is therefore sealed from the tree that
# `codesign --verify` just accepted.
zuuned_dmg_stage="$zuuned_work/dmg"
rm -rf "$zuuned_dmg_stage"; mkdir -p "$zuuned_dmg_stage"
rsync -a "$zuuned_app" "$zuuned_dmg_stage/"
ln -s /Applications "$zuuned_dmg_stage/Applications"
hdiutil create -quiet -volname "Zuuned $zuuned_version" -srcfolder "$zuuned_dmg_stage" \
    -ov -format UDZO "$zuuned_out/$zuuned_vol.dmg"
# `--norsrc` because the metadata fork `ditto` writes by default is a second copy of
# every extended attribute, stored as an in-place AppleDouble file beside the file it
# describes: the first zip this script built carried 2,526 `._*` entries, every one of
# them nothing but `com.apple.provenance` -- this machine's own bookkeeping, which
# means nothing on a tester's Mac and cannot survive the archive anyway. `ditto -x -k`
# folds them back into attributes; `unzip`, and whatever a browser hands a tester,
# leaves them as files inside a signed bundle. With the fork off the archive is the
# bundle. Measured on the mounted volume of that same run: a dmg carries zero, because
# a filesystem stores attributes natively where a zip has to fake them.
ditto -c -k --keepParent --norsrc "$zuuned_app" "$zuuned_out/$zuuned_vol.zip"
# Read it back rather than trust the flag: an archive of a bundle is only as clean as
# what was added to it.
zuuned_appledouble=$(unzip -Z1 "$zuuned_out/$zuuned_vol.zip" 2>/dev/null \
    | awk -F/ '{ f = $NF } f ~ /^\._/ { n++ } END { print n + 0 }')
[[ ${zuuned_appledouble:-1} == 0 ]] || {
    echo "$zuuned_appledouble AppleDouble entries inside the zip" >&2; exit 1; }
echo "zip_appledouble_entries=$zuuned_appledouble"

for zuuned_artifact in "$zuuned_out/$zuuned_vol.dmg" "$zuuned_out/$zuuned_vol.zip"; do
    printf '%s  %s  %s\n' "$(shasum -a 256 "$zuuned_artifact" | cut -d' ' -f1)" \
        "$(du -h "$zuuned_artifact" | cut -f1 | tr -d ' ')" "$zuuned_artifact"
done
echo "bundle=$zuuned_published"
echo "deploy=ok"
