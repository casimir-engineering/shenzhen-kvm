#!/bin/bash
# Builds "Shenzhen KVM.app" into build/, signed with the Developer ID (hardened runtime) so macOS keeps the
# Accessibility permission across rebuilds. With --release: secure timestamp, notarization, staple, and the
# disk image build/ShenzhenKVM.dmg, signed and notarized as well.
# VERSION=1.0.1 ./build.sh --release  builds a test version: only the built copy of Info.plist is changed.
set -euo pipefail
cd "$(dirname "$0")"
APP="build/Shenzhen KVM.app"
IDENTITY="Developer ID Application: INTUITION Robotique & Technologies (66LJ4BV7Q3)"
rm -rf build && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
xcrun swiftc -O -target arm64-apple-macos14 -o "$APP/Contents/MacOS/shenzhen-kvm" mac/Sources/*.swift
cp mac/Info.plist "$APP/Contents/Info.plist"
if [[ -n "${VERSION:-}" ]]; then
  PB=/usr/libexec/PlistBuddy
  BUILD=$("$PB" -c "Print :CFBundleVersion" "$APP/Contents/Info.plist")
  "$PB" -c "Set :CFBundleShortVersionString $VERSION" "$APP/Contents/Info.plist"
  "$PB" -c "Set :CFBundleVersion $((BUILD + 1))" "$APP/Contents/Info.plist"
fi
cp mac/icon/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cp mac/install-smooth.sh "$APP/Contents/Resources/install-smooth.sh"
if [[ -f mac/LaunchAgent.plist ]]; then
  mkdir -p "$APP/Contents/Library/LaunchAgents"
  cp mac/LaunchAgent.plist "$APP/Contents/Library/LaunchAgents/com.intuition.shenzhenkvm.agent.plist"
fi
if [[ "${1:-}" == "--release" ]]; then
  codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
  ditto -c -k --keepParent "$APP" build/ShenzhenKVM.zip
  xcrun notarytool submit build/ShenzhenKVM.zip --keychain-profile shenzhenpdf-notary --wait
  xcrun stapler staple "$APP"
  spctl -a -vv "$APP"
  # The installer disk image, signed and notarized as well.
  # dmgbuild writes the window layout without Finder; electron-builder's bundled copy (Shenzhen 3D's builds fetch it).
  DMGPYTHON="${DMGPYTHON:-$HOME/Library/Caches/electron-builder/dmg-builder@1.2.5/dmgbuild-bundle-arm64-75c8a6c-9epj3/python/bin/python3}"
  tiffutil -cathidpicheck dmg/dmg-background.png dmg/dmg-background@2x.png -out build/dmg-background.tiff >/dev/null
  "$DMGPYTHON" -I -m dmgbuild -s dmg/settings.py -D app="$APP" -D background=build/dmg-background.tiff "Shenzhen KVM" build/ShenzhenKVM.dmg
  codesign --force --timestamp --sign "$IDENTITY" build/ShenzhenKVM.dmg
  xcrun notarytool submit build/ShenzhenKVM.dmg --keychain-profile shenzhenpdf-notary --wait
  xcrun stapler staple build/ShenzhenKVM.dmg
  spctl -a -vv -t open --context context:primary-signature build/ShenzhenKVM.dmg
else
  codesign --force --options runtime --timestamp=none --sign "$IDENTITY" "$APP"
fi
