; NSIS installer for otd (On The Dial). Built by package-windows.sh inside
; the cross-build container; makensis runs on Linux.
;
; Defines given on the command line:
;   VERSION   e.g. 1.5.3
;   PKGDIR    the prepared package folder (otd.exe, Qt runtime, hamlib\)
;   ICON      the .ico for the installer window
;   OUTFILE   the installer to write
;   FILELIST  generated include with one File line per packaged file
;   DELLIST   generated include with one Delete/RMDir line per packaged file
;
; Installs for the current user only (no administrator prompt), under the
; user's own programs folder, with a Start menu entry, an optional desktop
; icon and an uninstaller listed in "Apps and features".

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "otd ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\otd"
InstallDirRegKey HKCU "Software\otd" "InstallDir"
BrandingText "otd ${VERSION} - free software, GPL-3.0-or-later"

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\otd"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\otd.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start otd now"
!define MUI_FINISHPAGE_LINK "otd.oh2gba.eu - how to connect your radio"
!define MUI_FINISHPAGE_LINK_LOCATION "https://otd.oh2gba.eu/rig.html"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PKGDIR}/LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "otd (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  !include "${FILELIST}"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Start menu
  CreateShortcut "$SMPROGRAMS\otd.lnk" "$INSTDIR\otd.exe" "" "$INSTDIR\otd.exe" 0

  ; Apps and features
  WriteRegStr HKCU "Software\otd" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "otd (On The Dial)"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "John, OH2GBA"
  WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "https://otd.oh2gba.eu/"
  WriteRegStr HKCU "${UNINST_KEY}" "HelpLink" "https://otd.oh2gba.eu/rig.html"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\otd.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop icon" SecDesktop
  CreateShortcut "$DESKTOP\otd.lnk" "$INSTDIR\otd.exe" "" "$INSTDIR\otd.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The program, the Qt runtime and Hamlib's rigctld."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; only what we put there; the user's database and settings stay in
  ; %LOCALAPPDATA%\onthedial\otd
  !include "${DELLIST}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\otd.lnk"
  Delete "$DESKTOP\otd.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\otd"
SectionEnd
