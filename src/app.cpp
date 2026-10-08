// MP4 Player 9300 - application framework, commands, file dialog.

#include <eikenv.h>
#include <eikbtgpc.h>
#include <eikmenub.h>
#include <cknenv.h>
#include <ckninfo.h>
#include <apgcli.h>
#include <mp4player.rsg>
#include "mp4player.h"
#include "mp4player.hrh"

_LIT(KAppTitle, "MP4 Player 9300 1.25");

// ------------------------------------------------------------------ framework entry points

GLDEF_C TInt E32Dll(TDllReason) { return KErrNone; }

EXPORT_C CApaApplication* NewApplication() { return new CMp4PlayerApplication; }

TUid CMp4PlayerApplication::AppDllUid() const { return KUidMp4Player; }

// Opening an .mp4 from the file manager: never treat it as a document store.
CFileStore* CMp4PlayerDocument::OpenFileL(TBool /*aDoOpen*/, const TDesC& aFilename, RFs& /*aFs*/)
    {
    if (iAppUi && aFilename.Length())
        {
        TFileName name(aFilename);
        static_cast<CMp4PlayerAppUi*>(iAppUi)->ProcessCommandParametersL(EApaCommandOpen, name, KNullDesC8);
        }
    return NULL;
    }

// ------------------------------------------------------------------ app UI

void CMp4PlayerAppUi::ConstructL()
    {
    BaseConstructL();
    CknEnv::Skin().SetAppViewType(ESkinAppViewWithCbaNoToolband);
    iWindowRect = ClientRect();
    iView = CVideoView::NewL(iWindowRect);
    AddToStackL(iView);
    iEngine = CPlayerEngine::NewL(*iView, *this);
    iView->SetEngine(iEngine);
    iStartup = new (ELeave) CAsyncCallBack(TCallBack(StartupOpen, this), CActive::EPriorityStandard);
    LoadSettings();
    UpdateCbaL();
    iView->DrawNow();
    }

CMp4PlayerAppUi::~CMp4PlayerAppUi()
    {
    delete iStartup;
    if (iView) iView->SetEngine(NULL);
    delete iEngine;
    if (iView)
        {
        RemoveFromStack(iView);
        delete iView;
        }
    }

TBool CMp4PlayerAppUi::ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& /*aTail*/)
    {
    if ((aCommand == EApaCommandOpen || aCommand == EApaCommandRun) && aDocumentName.Length())
        {
        TParsePtrC p(aDocumentName);
        if (p.Ext().Length() && iStartup)
            {
            iStartupFile = aDocumentName;
            iStartup->CallBack();
            }
        }
    aDocumentName.Zero();       // no document file of our own
    return EFalse;
    }

TInt CMp4PlayerAppUi::StartupOpen(TAny* aSelf)
    {
    CMp4PlayerAppUi* self = (CMp4PlayerAppUi*)aSelf;
    if (self->iStartupFile.Length())
        {
        TRAPD(err, self->OpenFileL(self->iStartupFile));
        (void)err;
        }
    return 0;
    }

void CMp4PlayerAppUi::HandleForegroundEventL(TBool aForeground)
    {
    CEikAppUi::HandleForegroundEventL(aForeground);
    if (!aForeground && iEngine && iEngine->State() == CPlayerEngine::EPlaying) iEngine->Pause();
    }

void CMp4PlayerAppUi::HandleCommandL(TInt aCommand)
    {
    switch (aCommand)
        {
        case EEikCmdExit:
            if (iEngine) iEngine->Close();
            Exit();
            break;
        case ECmdOpen:
            OpenFileDialogL();
            break;
        case ECmdPlayPause:
            if (iEngine->State() == CPlayerEngine::EIdle) OpenFileDialogL();
            else iEngine->PlayPause();
            break;
        case ECmdStop:
            iEngine->Stop();
            break;
        case ECmdRestart:
            iEngine->Stop();
            iEngine->Play();
            break;
        case ECmdSeekBack:      iEngine->SeekRelative(-10000); break;
        case ECmdSeekFwd:       iEngine->SeekRelative(10000); break;
        case ECmdSeekBackLong:  iEngine->SeekRelative(-60000); break;
        case ECmdSeekFwdLong:   iEngine->SeekRelative(60000); break;
        case ECmdVolUp:         iEngine->ChangeVolume(1); break;
        case ECmdVolDown:       iEngine->ChangeVolume(-1); break;
        case ECmdFullScreen:
            SetFullScreenL(!iView->FullScreen());
            break;
        case ECmdZoom:
            iView->ToggleZoom();
            iEngine->RedrawCurrentFrame();
            ShowZoomMessage();
            SaveSettings();
            break;
        case ECmdCropPos:
            // which part of a too-big video 1:1 shows; switches to 1:1 if another zoom was on
            if (iView->Zoom() != CVideoView::EZoomOriginal) iView->SetZoom(CVideoView::EZoomOriginal);
            else iView->NextCropPos();
            iEngine->RedrawCurrentFrame();
            ShowZoomMessage();
            SaveSettings();
            break;
        case ECmdFastH264:
            // auto -> best -> fast -> auto
            if (iEngine->FastMode() == CPlayerEngine::EFastAuto) iEngine->SetFastMode(CPlayerEngine::EFastOff);
            else if (iEngine->FastMode() == CPlayerEngine::EFastOff) iEngine->SetFastMode(CPlayerEngine::EFastOn);
            else iEngine->SetFastMode(CPlayerEngine::EFastAuto);
            if (iEngine->FastMode() == CPlayerEngine::EFastOn)
                PlayerMessage(_L("H.264 always fast: no smoothing filter (a third faster, a little blocky)"));
            else if (iEngine->FastMode() == CPlayerEngine::EFastOff)
                PlayerMessage(_L("H.264 always best quality (big videos play with fewer frames)"));
            else
                PlayerMessage(_L("H.264 automatic: fast for videos bigger than 320x240, else best"));
            SaveSettings();
            break;
        case ECmdDirect:
            if (!iView->DirectEnabled() || iView->DirectState() == CVideoView::EDirectFailed)
                {
                // on (again): a failed test is repeated
                iView->SetDirect(ETrue, iView->DirectState() == CVideoView::EDirectOk ?
                                 CVideoView::EDirectOk : CVideoView::EDirectUntested);
                PlayerMessage(_L("Direct drawing on: the picture is written straight to the screen"));
                }
            else
                {
                iView->SetDirect(EFalse, iView->DirectState());
                PlayerMessage(_L("Direct drawing off: the picture goes through the window server"));
                }
            iEngine->RedrawCurrentFrame();
            SaveSettings();
            break;
        case ECmdBehind:
            // auto -> keep in time -> smooth -> auto
            if (iEngine->Behind() == CPlayerEngine::EBehindAuto) iEngine->SetBehind(CPlayerEngine::EBehindSync);
            else if (iEngine->Behind() == CPlayerEngine::EBehindSync) iEngine->SetBehind(CPlayerEngine::EBehindSmooth);
            else iEngine->SetBehind(CPlayerEngine::EBehindAuto);
            if (iEngine->Behind() == CPlayerEngine::EBehindSync)
                PlayerMessage(_L("Too slow video: keep in time (few pictures, jumps ahead early)"));
            else if (iEngine->Behind() == CPlayerEngine::EBehindSmooth)
                PlayerMessage(_L("Too slow video: smooth (more pictures, runs slower, jumps later)"));
            else
                PlayerMessage(_L("Too slow video: auto (keep in time with sound, smooth without)"));
            SaveSettings();
            break;
        case ECmdSound:
            iEngine->SetSoundOff(!iEngine->SoundOff());
            if (iEngine->SoundOff())
                PlayerMessage(_L("Sound off: the phone decodes no audio, a little more time for the video. M: on"));
            else
                PlayerMessage(_L("Sound on. M: off (a little faster video)"));
            SaveSettings();
            break;
        case ECmdFileInfo:
            ShowFileInfoL();
            break;
        case ECmdAnalyze:
            AnalyzeFileL();
            break;
        case ECmdBenchmark:
            {
            if (iEngine->Mode() != CPlayerEngine::EModeOwn)
                {
                PlayerMessage(_L("Speed test: open a video the built-in decoder plays first"));
                break;
                }
            PlayerMessage(_L("Speed test running (about 10-20 s)..."));
            iCoeEnv->WsSession().Flush();
            HBufC* text = HBufC::NewLC(600);
            TPtr ptr = text->Des();
            iEngine->Benchmark(ptr);
            {
            TBuf<160> set;
            SettingsText(set);
            if (ptr.Length() + set.Length() <= ptr.MaxLength()) ptr.Insert(0, set);
            }
            PlayerMessage(KNullDesC);
            CCknInfoDialog::RunDlgLD(_L("Speed test"), *text);
            CleanupStack::PopAndDestroy(text);
            }
            break;
        case ECmdAbout:
            {
            _LIT(KText, "Plays MP4, 3GP and MOV files with MPEG-4 Part 2 (Simple Profile), H.263 or H.264 Baseline video "
                        "and AAC/AMR audio (for H.264 files the sound is copied to System\\Temp first). Keys: Space play/pause, arrows seek (Shift: 1 min) and volume, "
                        "F full screen, Z zoom (fit, fill, original), Q H.264 auto/best/fast, K too slow video: auto/in time/smooth, M sound off, A analyze a file, Ctrl+O open. Best: 320x200 or smaller, 12-15 fps.");
            CCknInfoDialog::RunDlgLD(KAppTitle, KText);
            }
            break;
        default:
            break;
        }
    }

void CMp4PlayerAppUi::SetFullScreenL(TBool aFull)
    {
    CEikButtonGroupContainer* cba = CEikButtonGroupContainer::Current();
    iView->SetFullScreen(aFull);
    if (aFull)
        {
        CknEnv::Skin().SetAppViewType(ESkinAppViewNoCbaNoToolband);
        if (cba) cba->MakeVisible(EFalse);
        iView->SetRect(ApplicationRect());
        }
    else
        {
        CknEnv::Skin().SetAppViewType(ESkinAppViewWithCbaNoToolband);
        if (cba) cba->MakeVisible(ETrue);
        iView->SetRect(iWindowRect);
        }
    iEngine->RedrawCurrentFrame();
    iView->DrawNow();
    if (!aFull && cba) cba->DrawNow();
    }

void CMp4PlayerAppUi::UpdateCbaL()
    {
    CEikButtonGroupContainer* cba = CEikButtonGroupContainer::Current();
    TBool playing = iEngine && iEngine->State() == CPlayerEngine::EPlaying;
    if (!cba) return;
    if (playing == iCbaPlaying) return;
    iCbaPlaying = playing;
    cba->SetCommandL(1, ECmdPlayPause, playing ? _L("Pause") : _L("Play"));
    if (!iView->FullScreen()) cba->DrawNow();
    }

void CMp4PlayerAppUi::PlayerStateChanged()
    {
    TRAPD(err, UpdateCbaL());
    (void)err;
    iView->DrawStatusNow();
    }

void CMp4PlayerAppUi::PlayerMessage(const TDesC& aText)
    {
    iView->SetMessage(aText);
    iView->DrawStatusNow();
    }

TBool CMp4PlayerAppUi::ShowFileInFileListL(const TParsePtrC& aParse, const TUidType& /*aType*/, const TDataType& /*aDataType*/)
    {
    TBuf<8> ext;
    if (aParse.Ext().Length() > 8) return EFalse;
    ext.CopyLC(aParse.Ext());
    return ext == _L(".mp4") || ext == _L(".3gp") || ext == _L(".3g2") || ext == _L(".m4v") ||
           ext == _L(".mov") || ext == _L(".m4a") || ext == _L(".3gpp") || ext == _L(".mp4v");
    }

void CMp4PlayerAppUi::OpenFileDialogL()
    {
    TFileName path(iLastPath);
    TBool wasPlaying = iEngine->State() == CPlayerEngine::EPlaying;
    if (wasPlaying) iEngine->Pause();
    if (CCknOpenFileDialog::RunDlgLD(path, this, _L("Videos"), CCknFileListDialogBase::EShowAllDrives))
        {
        OpenFileL(path);
        }
    else if (wasPlaying) iEngine->Play();
    }

void CMp4PlayerAppUi::OpenFileL(const TDesC& aName)
    {
    TParsePtrC p(aName);
    iLastPath = p.DriveAndPath();
    iView->SetMessage(KNullDesC);
    TRAPD(err, iEngine->OpenL(aName));
    if (err != KErrNone)
        {
        TBuf<80> t;
        t.Format(_L("Cannot open the file (error %d)"), err);
        PlayerMessage(t);
        }
    PlayerStateChanged();
    }

void CMp4PlayerAppUi::ShowFileInfoL()
    {
    HBufC* text = HBufC::NewLC(700);
    TPtr ptr = text->Des();
    iEngine->InfoText(ptr);
    {
    TBuf<160> set;
    SettingsText(set);
    if (ptr.Length() + set.Length() <= ptr.MaxLength()) ptr.Insert(0, set);
    }
    TParsePtrC p(iEngine->FileName());
    TBuf<80> title;
    title.Copy(p.NameAndExt().Left(80));
    if (title.Length() == 0) title.Copy(_L("No file"));
    CCknInfoDialog::RunDlgLD(title, *text);
    CleanupStack::PopAndDestroy(text);
    }

// "Analyze a file": parameters of a video + a short decode test, without playing it. The report
// is shown in pages and saved next to the video as <name>_analysis.txt (or on C: if that fails).
void CMp4PlayerAppUi::AnalyzeFileL()
    {
    TFileName path(iLastPath);
    if (iEngine->State() == CPlayerEngine::EPlaying) iEngine->Pause();
    if (!CCknOpenFileDialog::RunDlgLD(path, this, _L("Analyze a video"), CCknFileListDialogBase::EShowAllDrives)) return;
    TParsePtrC parse(path);
    iLastPath = parse.DriveAndPath();
    PlayerMessage(_L("Analyzing (about 10-30 s)..."));
    iCoeEnv->WsSession().Flush();
    HBufC* text = HBufC::NewLC(2400);
    TPtr ptr = text->Des();
    iEngine->Analyze(path, ptr);
    {
    TBuf<40> head;
    head.Copy(KAppTitle);
    head.Append(_L(" analysis\n"));
    if (ptr.Length() + head.Length() <= ptr.MaxLength()) ptr.Insert(0, head);
    }

    // save: next to the video, else on C:
    TFileName out;
    out.Copy(parse.DriveAndPath());
    TInt room = KMaxFileName - out.Length() - 16;
    out.Append(parse.Name().Left(room > 0 ? room : 0));
    out.Append(_L("_analysis.txt"));
    TBool saved = WriteTextFile(out, ptr);
    if (!saved)
        {
        out.Copy(_L("C:\\"));
        out.Append(parse.Name().Left(KMaxFileName - 20));
        out.Append(_L("_analysis.txt"));
        saved = WriteTextFile(out, ptr);
        }
    if (saved)
        {
        if (ptr.Length() + out.Length() + 12 <= ptr.MaxLength()) { ptr.Append(_L("Saved as ")); ptr.Append(out); }
        }
    else if (ptr.Length() + 20 <= ptr.MaxLength()) ptr.Append(_L("(could not save)"));
    PlayerMessage(_L("Analysis done. Open a video to play it."));

    // show in pages of whole lines
    const TInt KPage = 380;
    TInt pages = 0, pos = 0;
    while (pos < ptr.Length())
        {
        TInt len = ptr.Length() - pos;
        if (len > KPage)
            {
            len = KPage;
            TInt cut = ptr.Mid(pos, KPage).LocateReverse('\n');
            if (cut > 40) len = cut + 1;
            }
        pages++;
        pos += len;
        }
    pos = 0;
    for (TInt page = 1; page <= pages && pos < ptr.Length(); page++)
        {
        TInt len = ptr.Length() - pos;
        if (len > KPage)
            {
            len = KPage;
            TInt cut = ptr.Mid(pos, KPage).LocateReverse('\n');
            if (cut > 40) len = cut + 1;
            }
        HBufC* part = ptr.Mid(pos, len).AllocLC();
        TPtr pp = part->Des();
        for (TInt i = 0; i < pp.Length(); i++) if (pp[i] == '\n') pp[i] = ' ';
        TBuf<40> title;
        title.Format(_L("Analysis %d/%d"), page, pages);
        CCknInfoDialog::RunDlgLD(title, *part);
        CleanupStack::PopAndDestroy(part);
        pos += len;
        }
    CleanupStack::PopAndDestroy(text);
    }

// ASCII text file with CRLF line ends
TBool CMp4PlayerAppUi::WriteTextFile(const TDesC& aName, const TDesC& aText)
    {
    RFs& fs = iCoeEnv->FsSession();
    HBufC8* buf = HBufC8::New(aText.Length() * 2 + 2);
    if (!buf) return EFalse;
    TPtr8 b = buf->Des();
    for (TInt i = 0; i < aText.Length(); i++)
        {
        TUint c = aText[i];
        if (c == '\n') b.Append(_L8("\r\n"));
        else b.Append((TUint8)(c >= 32 && c < 127 ? c : '?'));
        }
    RFile f;
    TBool ok = EFalse;
    if (f.Replace(fs, aName, EFileWrite) == KErrNone)
        {
        ok = f.Write(b) == KErrNone;
        f.Close();
        }
    delete buf;
    return ok;
    }

// ------------------------------------------------------------------ settings (zoom, 1:1 position)

_LIT(KSettingsFile, "C:\\System\\Data\\mp4player9300.ini");

void CMp4PlayerAppUi::LoadSettings()
    {
    RFile f;
    TBuf8<8> d;
    iView->SetDirect(ETrue, CVideoView::EDirectUntested);
    if (f.Open(iCoeEnv->FsSession(), KSettingsFile, EFileRead) != KErrNone) return;
    if (f.Read(d) == KErrNone && d.Length() >= 2)
        {
        if (d[0] <= CVideoView::EZoomOriginal) iView->SetZoom((CVideoView::TZoom)d[0]);
        if (d[1] <= CVideoView::ECropBottom) iView->SetCropPos((CVideoView::TCropPos)d[1]);
        // byte 3: H.264 mode + 1 (byte 2 was 2.1's on/off switch)
        if (d.Length() >= 4 && d[3] >= 1 && d[3] <= 3) iEngine->SetFastMode((CPlayerEngine::TFastMode)(d[3] - 1));
        // bytes 4, 5: direct drawing test result, switched off by the user
        if (d.Length() >= 6)
            {
            CVideoView::TDirectState st = d[4] <= CVideoView::EDirectFailed ? (CVideoView::TDirectState)d[4] : CVideoView::EDirectUntested;
            if (st == CVideoView::EDirectTesting) st = CVideoView::EDirectFailed;    // the test crashed last time
            iView->SetDirect(d[5] == 0, st);
            }
        if (d.Length() >= 7) iEngine->SetSoundOff(d[6] != 0);      // byte 6: sound off
        if (d.Length() >= 8 && d[7] <= CPlayerEngine::EBehindSmooth)   // byte 7: too slow video
            iEngine->SetBehind((CPlayerEngine::TBehind)d[7]);
        }
    f.Close();
    }

void CMp4PlayerAppUi::SaveSettings()
    {
    RFs& fs = iCoeEnv->FsSession();
    RFile f;
    TBuf8<8> d;
    d.Append((TUint8)iView->Zoom());
    d.Append((TUint8)iView->CropPos());
    d.Append((TUint8)(iEngine->FastMode() == CPlayerEngine::EFastOn ? 1 : 0));
    d.Append((TUint8)(iEngine->FastMode() + 1));
    d.Append((TUint8)iView->DirectState());
    d.Append((TUint8)(iView->DirectEnabled() ? 0 : 1));
    d.Append((TUint8)(iEngine->SoundOff() ? 1 : 0));
    d.Append((TUint8)iEngine->Behind());
    fs.MkDirAll(KSettingsFile);     // the folder usually exists already
    if (f.Replace(fs, KSettingsFile, EFileWrite) != KErrNone) return;
    f.Write(d);
    f.Close();
    }

// e.g. "[1.20 | Z 1:1 mid | F win | Q auto=fast | D on, used | M sound] "
void CMp4PlayerAppUi::SettingsText(TDes& aText)
    {
    aText.Zero();
    aText.Append(_L("["));
    aText.Append(KAppTitle().Mid(16));          // the version number
    switch (iView->Zoom())
        {
        case CVideoView::EZoomFit: aText.Append(_L(" | Z fit")); break;
        case CVideoView::EZoomFill: aText.Append(_L(" | Z fill")); break;
        default:
            aText.Append(_L(" | Z 1:1 "));
            aText.Append(iView->CropPos() == CVideoView::ECropTop ? _L("top") :
                         iView->CropPos() == CVideoView::ECropBottom ? _L("bot") : _L("mid"));
            break;
        }
    aText.Append(iView->FullScreen() ? _L(" | F full") : _L(" | F win"));
    switch (iEngine->FastMode())
        {
        case CPlayerEngine::EFastOn: aText.Append(_L(" | Q fast")); break;
        case CPlayerEngine::EFastOff: aText.Append(_L(" | Q best")); break;
        default: aText.Append(iEngine->FastNow() ? _L(" | Q auto=fast") : _L(" | Q auto=best")); break;
        }
    if (!iView->DirectEnabled()) aText.Append(_L(" | D off"));
    else if (iView->DirectState() == CVideoView::EDirectFailed) aText.Append(_L(" | D on, test failed"));
    else aText.Append(iEngine->LastDirect() ? _L(" | D on, used") : _L(" | D on, not used"));
    aText.Append(iEngine->SoundOff() ? _L(" | M no sound") : _L(" | M sound"));
    switch (iEngine->Behind())
        {
        case CPlayerEngine::EBehindSync: aText.Append(_L(" | K sync")); break;
        case CPlayerEngine::EBehindSmooth: aText.Append(_L(" | K smooth")); break;
        default: aText.Append(iEngine->SyncNow() ? _L(" | K auto=sync") : _L(" | K auto=smooth")); break;
        }
    aText.Append(iEngine->TooSlow() ? _L(", too slow] ") : _L("] "));
    }

void CMp4PlayerAppUi::ShowZoomMessage()
    {
    switch (iView->Zoom())
        {
        case CVideoView::EZoomFit:
            PlayerMessage(_L("Zoom: fit (whole picture, scaled if needed)"));
            break;
        case CVideoView::EZoomFill:
            PlayerMessage(_L("Zoom: fill (scaled, edges cut off)"));
            break;
        default:
            if (iView->CropPos() == CVideoView::ECropTop) PlayerMessage(_L("Zoom: 1:1, no scaling (fastest), top part. C: position"));
            else if (iView->CropPos() == CVideoView::ECropBottom) PlayerMessage(_L("Zoom: 1:1, no scaling (fastest), bottom part. C: position"));
            else PlayerMessage(_L("Zoom: 1:1, no scaling (fastest), middle part. C: position"));
            break;
        }
    }
