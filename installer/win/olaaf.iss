; olAAF installer for Windows (Inno Setup 6)
[Setup]
AppName=olAAF
AppVersion=0.4.0
AppPublisher=dmajorsound
AppPublisherURL=https://dmajorsound.com
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=olAAF-0.4.0-win64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Uninstallable=yes
UninstallDisplayName=olAAF (REAPER extension)
LicenseFile=..\..\LICENSE
WizardStyle=modern
WizardImageFile=wizard-100.bmp,wizard-200.bmp
WizardSmallImageFile=small-100.bmp,small-200.bmp
WizardImageStretch=no
DisableWelcomePage=no

[Files]
Source: "..\..\build\src\ext\Release\reaper_olaaf.dll"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
SelectDirLabel3=olAAF goes into the REAPER UserPlugins folder. Change it only if you use a portable REAPER install.
