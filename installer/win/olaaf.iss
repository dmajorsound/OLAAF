; OLAAF installer for Windows (Inno Setup 6)
[Setup]
AppName=OLAAF
AppVersion=0.8.8
AppPublisher=dmajorsound
AppPublisherURL=https://dmajorsound.com
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=OLAAF-0.8.8-win64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Uninstallable=yes
UninstallFilesDir={userappdata}\dmajorsound\OLAAF
UninstallDisplayIcon={uninstallexe}
CloseApplications=no
UninstallDisplayName=OLAAF (REAPER extension)
LicenseFile=..\..\LICENSE
WizardStyle=modern
WizardImageFile=wizard-100.bmp,wizard-200.bmp
WizardSmallImageFile=small-100.bmp,small-200.bmp
WizardImageStretch=no
DisableWelcomePage=no

[Files]
Source: "welcome-icon-100.bmp"; DestDir: "{tmp}"; Flags: dontcopy
Source: "welcome-icon-200.bmp"; DestDir: "{tmp}"; Flags: dontcopy
Source: "welcome-brand-100.bmp"; DestDir: "{tmp}"; Flags: dontcopy
Source: "welcome-brand-200.bmp"; DestDir: "{tmp}"; Flags: dontcopy
Source: "..\..\build\src\ext\Release\reaper_olaaf.dll"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
WelcomeLabel1=OLAAF - AAF Import for Reaper
WelcomeLabel2=This installs the OLAAF extension into your REAPER UserPlugins folder.%n%nQuit REAPER first. Afterwards, start REAPER and open Extensions > OLAAF.
SelectDirLabel3=OLAAF goes into the REAPER UserPlugins folder. Change it only if you use a portable REAPER install.

[Code]
#include "branding.iss"

procedure InitializeWizard();
begin
  BrandWelcome('OLAAF - AAF Import for Reaper',
    'This installs the OLAAF extension into your REAPER UserPlugins folder.' + #13#10 + #13#10 +
    'Quit REAPER first. Afterwards, start REAPER and open Extensions > OLAAF.');
end;
