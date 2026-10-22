#!/usr/bin/env bash
# Baut MiniPDF für Windows und erzeugt:
#   minipdf-<Version>-setup.exe   (Installer, eine einzelne Datei)
#   minipdf-portable.zip          (ohne Installation lauffähig)
#
# Aufruf im Terminal "MSYS2 UCRT64":   bash windows/build-windows.sh
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION="${VERSION:-1.0.0}"

echo "== Pakete installieren =="
pacman -S --needed --noconfirm zip \
    mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,qt6-base,qt6-tools,poppler-qt6,nsis}

echo "== Bauen =="
cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win

echo "== Programmordner 'dist' zusammenstellen =="
rm -rf dist && mkdir dist
cp build-win/minipdf.exe dist/
WDQ="$(command -v windeployqt6 || command -v windeployqt-qt6 || command -v windeployqt)"
"$WDQ" --no-translations dist/minipdf.exe
# Alle übrigen DLL-Abhängigkeiten (Poppler, Freetype, ...) für exe und Qt-Plugins einsammeln
find dist \( -name '*.exe' -o -name '*.dll' \) -print0 \
  | xargs -0 -n1 ldd 2>/dev/null \
  | grep -i '/ucrt64/' | awk '{print $3}' | sort -u \
  | xargs -I{} cp -n {} dist/

echo "== ZIP und Installer =="
rm -f minipdf-portable.zip
(cd dist && zip -qr ../minipdf-portable.zip .)
(cd windows && makensis -V2 -DVERSION="$VERSION" minipdf.nsi)

echo
echo "Fertig:"
ls -lh minipdf-*-setup.exe minipdf-portable.zip
