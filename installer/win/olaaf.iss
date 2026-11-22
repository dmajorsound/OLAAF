; olAAF installer for Windows (Inno Setup 6)
[Setup]
AppName=olAAF
AppVersion=0.1.0
AppPublisher=dmajorsound
AppPublisherURL=https://dmajorsound.com
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=olAAF-0.1.0-win64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Uninstallable=yes
UninstallDisplayName=olAAF (REAPER extension)
LicenseFile=..\..\LICENSE

[Files]
Source: "..\..\build\src\ext\Release\reaper_olaaf.dll"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
SelectDirLabel3=olAAF goes into the REAPER UserPlugins folder. Change it only if you use a portable REAPER install.
