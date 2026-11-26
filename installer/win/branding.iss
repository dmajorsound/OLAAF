// Shared by installer and uninstaller: white pages, and a welcome page laid out like the Mac
// installer - the app icon in the middle, the dmajorsound logo small in the top right corner.

function HiDpi(): Boolean;
begin
  Result := ScaleX(100) > 150;
end;

function LoadBmp(const Base: String): String;
begin
  if HiDpi() then Result := Base + '-200.bmp' else Result := Base + '-100.bmp';
  ExtractTemporaryFile(Result);
  Result := ExpandConstant('{tmp}\') + Result;
end;

procedure BrandWelcome(const Title, Body: String);
var
  Page: TNewNotebookPage;
  Icon, Brand: TBitmapImage;
  Head, Text: TLabel;
  Margin: Integer;
begin
  WizardForm.Color := clWhite;
  WizardForm.MainPanel.Color := clWhite;
  WizardForm.InnerPage.Color := clWhite;
  WizardForm.WelcomePage.Color := clWhite;
  WizardForm.FinishedPage.Color := clWhite;

  Page := WizardForm.WelcomePage;
  WizardForm.WizardBitmapImage.Visible := False;
  WizardForm.WelcomeLabel1.Visible := False;
  WizardForm.WelcomeLabel2.Visible := False;
  Margin := ScaleX(24);

  Brand := TBitmapImage.Create(WizardForm);
  Brand.Parent := Page;
  Brand.Bitmap.LoadFromFile(LoadBmp('welcome-brand'));
  Brand.Stretch := True;
  Brand.Width := ScaleX(44);
  Brand.Height := ScaleY(37);
  Brand.Left := Page.Width - Brand.Width - ScaleX(12);
  Brand.Top := ScaleY(10);

  Icon := TBitmapImage.Create(WizardForm);
  Icon.Parent := Page;
  Icon.Bitmap.LoadFromFile(LoadBmp('welcome-icon'));
  Icon.Stretch := True;
  Icon.Width := ScaleX(72);
  Icon.Height := ScaleY(72);
  Icon.Left := (Page.Width - Icon.Width) div 2;
  Icon.Top := ScaleY(56);

  Head := TLabel.Create(WizardForm);
  Head.Parent := Page;
  Head.AutoSize := False;
  Head.Alignment := taCenter;
  Head.Font.Assign(WizardForm.WelcomeLabel1.Font);
  Head.Caption := Title;
  Head.Left := Margin;
  Head.Width := Page.Width - 2 * Margin;
  Head.Top := Icon.Top + Icon.Height + ScaleY(14);
  Head.Height := ScaleY(30);

  Text := TLabel.Create(WizardForm);
  Text.Parent := Page;
  Text.AutoSize := False;
  Text.WordWrap := True;
  Text.Alignment := taCenter;
  Text.Font.Assign(WizardForm.WelcomeLabel2.Font);
  Text.Caption := Body;
  Text.Left := Margin;
  Text.Width := Page.Width - 2 * Margin;
  Text.Top := Head.Top + Head.Height + ScaleY(6);
  Text.Height := Page.Height - Text.Top - ScaleY(8);
end;
