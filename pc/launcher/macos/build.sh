#!/bin/sh
# Builds "Mirror's Edge Launcher.app" (arm64) into the directory given (default: ./work). Needs swiftc (Xcode CLT).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-work}
app="$out/MirrorsEdgeLauncher.app"
mkdir -p "$app/Contents/MacOS"
swiftc -O -parse-as-library "$here/MELauncher.swift" -o "$app/Contents/MacOS/MELauncher"
cat > "$app/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>MELauncher</string>
<key>CFBundleIdentifier</key><string>org.runnerrecompiled.launcher</string>
<key>CFBundleName</key><string>Mirror's Edge Launcher</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PL
echo "built $app"
