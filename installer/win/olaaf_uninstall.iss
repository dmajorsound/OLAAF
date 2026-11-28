; OLAAF uninstaller for Windows (Inno Setup 6): removes the extension, keeps projects and audio
[Setup]
AppId=OLAAF_Uninstaller
AppName=OLAAF Uninstaller
AppVersion=0.8.7
AppPublisher=dmajorsound
AppPublisherURL=https://dmajorsound.com
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=lowest
OutputBaseFilename=OLAAF-0.8.7-win64-uninstall
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Uninstallable=no
CloseApplications=no
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

[Messages]
SetupWindowTitle=Uninstall OLAAF
WelcomeLabel1=Uninstall OLAAF
WelcomeLabel2=This removes the OLAAF extension from REAPER.%n%nYour REAPER projects and imported audio stay untouched.%n%nQuit REAPER first.
SelectDirLabel3=OLAAF is removed from this REAPER UserPlugins folder. Change it only if you use a portable REAPER install.
SelectDirBrowseLabel=To continue, click Next.
WizardReady=Ready to uninstall
ReadyLabel1=OLAAF will now be removed.
ReadyLabel2a=Click Uninstall to continue.
ButtonInstall=&Uninstall
WizardInstalling=Uninstalling
InstallingLabel=Please wait while OLAAF is removed.
FinishedHeadingLabel=OLAAF is removed
FinishedLabelNoIcons=OLAAF has been removed from REAPER. Restart REAPER if it was open.
FinishedLabel=OLAAF has been removed from REAPER. Restart REAPER if it was open.

[Code]
#include "branding.iss"

procedure InitializeWizard();
begin
  BrandWelcome('Uninstall OLAAF',
    'This removes the OLAAF extension from REAPER. Your REAPER projects and imported audio stay untouched.' + #13#10 + #13#10 +
    'Quit REAPER first.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Unins, Dll: String;
  Code: Integer;
begin
  if CurStep <> ssInstall then exit;
  { the uninstaller the OLAAF installer left behind also clears the Installed apps entry }
  Unins := ExpandConstant('{userappdata}\dmajorsound\OLAAF\unins000.exe');
  if FileExists(Unins) then
    Exec(Unins, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Dll := AddBackslash(WizardDirValue) + 'reaper_olaaf.dll';
  if FileExists(Dll) and not DeleteFile(Dll) then
    MsgBox('OLAAF could not be removed because REAPER is still running.' + #13#10 +
           'Quit REAPER and run this uninstaller again.', mbError, MB_OK);
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OLAAF_is1');
  DelTree(ExpandConstant('{userappdata}\dmajorsound\OLAAF'), True, True, True);
end;
