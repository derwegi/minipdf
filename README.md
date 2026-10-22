# MiniPDF

Schlanker PDF-Betrachter im Stil von SumatraPDF (Qt6 + Poppler).

## Bauen unter Debian 13 (Trixie)

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libpoppler-qt6-dev
cd minipdf
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/minipdf datei.pdf          # oder: sudo cmake --install build
```

Das Drucken nutzt CUPS (unter Debian meist schon installiert; sonst `sudo apt install cups`).

## Tastenkürzel

| Taste | Funktion |
|---|---|
| Strg+O | Öffnen (auch Drag & Drop) |
| Strg+P | Drucken (Seitenbereich, aktuelle Seite, Kopien, in Datei) |
| Strg+W / Strg+Tab | Tab schließen / wechseln |
| N, P, ←, → | nächste / vorherige Seite |
| Leertaste, J, K | scrollen |
| Strg+G | Seitenzahl eingeben |
| Strg++ / Strg+- / Strg+Mausrad | Zoom |
| Strg+0, Strg+2 / Strg+3 / Strg+1 | Seite einpassen / Breite einpassen / 100 % |
| Strg+7 | Doppelseitenansicht |
| Strg+F, F3, Shift+F3 | Suchfeld anspringen, nächster / vorheriger Treffer (auch die Pfeile neben dem Feld, Enter / Shift+Enter) |
| F12 | Inhaltsverzeichnis |
| F5 | Neu laden (passiert auch automatisch bei Dateiänderung) |
| F8 / F9 / F11 | Werkzeugleiste / Menü / Vollbild |
| Shift + Maus ziehen | Text markieren und kopieren |
| Maus ziehen | Seite verschieben |
