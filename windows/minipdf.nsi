; NSIS-Installer für MiniPDF -> erzeugt eine einzelne Setup-.exe
; Wird von build-windows.sh aus dem Ordner "windows" heraus aufgerufen
; (erwartet den fertigen Programmordner in ..\dist).
Unicode true
!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!define APP "MiniPDF"

!include "MUI2.nsh"

Name "${APP} ${VERSION}"
OutFile "..\minipdf-${VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\${APP}"
InstallDirRegKey HKLM "Software\${APP}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_FINISHPAGE_RUN "$INSTDIR\minipdf.exe"
!define MUI_FINISHPAGE_RUN_TEXT "MiniPDF jetzt starten"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "German"

Section "Install"
  SetRegView 64
  SetOutPath "$INSTDIR"
  File /r "..\dist\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortcut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\minipdf.exe"
  CreateShortcut "$SMPROGRAMS\${APP}\Deinstallieren.lnk" "$INSTDIR\uninstall.exe"

  ; Eintrag in "Öffnen mit" für .pdf (Windows 11 lässt den Standard nur den Benutzer wählen)
  WriteRegStr HKLM "Software\Classes\Applications\minipdf.exe\shell\open\command" "" '"$INSTDIR\minipdf.exe" "%1"'
  WriteRegStr HKLM "Software\Classes\Applications\minipdf.exe" "FriendlyAppName" "${APP}"
  WriteRegStr HKLM "Software\Classes\Applications\minipdf.exe\SupportedTypes" ".pdf" ""

  ; Eintrag unter "Installierte Apps"
  WriteRegStr HKLM "Software\${APP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "DisplayName" "${APP}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "DisplayIcon" "$INSTDIR\minipdf.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
  Delete "$SMPROGRAMS\${APP}\Deinstallieren.lnk"
  RMDir "$SMPROGRAMS\${APP}"
  DeleteRegKey HKLM "Software\Classes\Applications\minipdf.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
  DeleteRegKey HKLM "Software\${APP}"
  RMDir /r "$INSTDIR"
SectionEnd
