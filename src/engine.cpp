// MP4 Player 9300 - playback engine: demux + decode + timing, MMF audio, system video fallback.

#include <eikenv.h>
#include <e32hal.h>
#include <e32svr.h>
#include "mp4player.h"

const TInt KMaxLateMs = 1500;           // further behind than this: jump to the next key frame
const TInt KMaxRunMs = 250;             // longest decoding stretch without showing a picture
const TInt KBehindMs = 40;              // further behind than this: decode the next frame unshown
const TInt KMaxNoShowMs = 1000;         // but show a picture at least this often
const TInt KAudioSyncTicksMs = 1000;
const TInt KAudioSyncTolerance = 200;
const TInt KAudioSyncSmall = 40;
const TInt KDefaultVolume = 6;
const TInt KCopyBufSize = 0x10000;      // audio copy: bytes read per step
_LIT(KTmpDir, "\\System\\Temp\\");
_LIT(KTmpBase, "mp4player_sound");
_LIT(KNotPlayable, " (not playable)");

static void AppendAscii(TDes& aDes, const char* aText)
    {
    while (*aText && aDes.Length() < aDes.MaxLength()) aDes.Append((TChar)(TUint8)*aText++);
    }

static TInt Ms(const TTimeIntervalMicroSeconds& aTime)
    {
    TInt64 v = aTime.Int64() / TInt64(1000);
    return v.GetTInt();
    }

// ------------------------------------------------------------------ construction

CPlayerEngine* CPlayerEngine::NewL(CVideoView& aView, MPlayerObserver& aObserver)
    {
    CPlayerEngine* self = new (ELeave) CPlayerEngine(aView, aObserver);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

CPlayerEngine::CPlayerEngine(CVideoView& aView, MPlayerObserver& aObserver)
    : CActive(EPriorityStandard), iView(aView), iObserver(aObserver), iVolume(KDefaultVolume)
    {
    }

void CPlayerEngine::ConstructL()
    {
    User::LeaveIfError(iFs.Connect());
    User::LeaveIfError(iTimer.CreateLocal());
    CActiveScheduler::Add(this);
    // the decoder gets its own heap so a big video can't exhaust the app's default heap
    iHeap = UserHeap::ChunkHeap(NULL, 0x10000, 0xC00000);
    if (!iHeap) iHeap = UserHeap::ChunkHeap(NULL, 0x10000, 0x400000);
    iAlloc.alloc = HeapAlloc;
    iAlloc.free = HeapFree;
    iAlloc.opaque = this;
    TTimeIntervalMicroSeconds32 tick;
    if (UserHal::TickPeriod(tick) == KErrNone && tick.Int() > 0) iTickUs = tick.Int();
    else iTickUs = 15625;
    if (yuvconv_open(&iConv, &iAlloc) != 0) User::Leave(KErrNoMemory);
    iRetry = new (ELeave) CAsyncCallBack(TCallBack(RetryAudio, this), CActive::EPriorityStandard);
    iFallback = new (ELeave) CAsyncCallBack(TCallBack(DoFallback, this), CActive::EPriorityStandard);
    iStartCopy = new (ELeave) CAsyncCallBack(TCallBack(StartCopyCb, this), CActive::EPriorityStandard);
    }

CPlayerEngine::~CPlayerEngine()
    {
    Close();
    Cancel();
    delete iRetry;
    delete iFallback;
    delete iStartCopy;
    yuvconv_close(iConv);
    iTimer.Close();
    iFs.Close();
    if (iHeap) iHeap->Close();
    }

void* CPlayerEngine::HeapAlloc(void* aOpaque, int aSize)
    {
    CPlayerEngine* self = (CPlayerEngine*)aOpaque;
    return self->iHeap ? self->iHeap->Alloc(aSize) : User::Alloc(aSize);
    }

void CPlayerEngine::HeapFree(void* aOpaque, void* aPtr)
    {
    CPlayerEngine* self = (CPlayerEngine*)aOpaque;
    if (self->iHeap) self->iHeap->Free(aPtr);
    else User::Free(aPtr);
    }

int CPlayerEngine::FileRead(void* aOpaque, u32 aOffset, void* aBuf, int aLen)
    {
    CPlayerEngine* self = (CPlayerEngine*)aOpaque;
    TPtr8 des((TUint8*)aBuf, 0, aLen);
    TInt err = self->iFile.Read((TInt)aOffset, des, aLen);
    if (err != KErrNone) return -1;
    return des.Length();
    }

// ------------------------------------------------------------------ helpers

void CPlayerEngine::SetState(TState aState)
    {
    iState = aState;
    iObserver.PlayerStateChanged();
    }

void CPlayerEngine::Message(TRefByValue<const TDesC> aFmt, ...)
    {
    VA_LIST list;
    VA_START(list, aFmt);
    TBuf<120> buf;
    buf.FormatList(aFmt, list);
    iObserver.PlayerMessage(buf);
    }

void CPlayerEngine::ScheduleIn(TInt aMs)
    {
    if (IsActive()) Cancel();
    if (aMs <= 0)
        {
        iStatus = KRequestPending;
        SetActive();
        TRequestStatus* s = &iStatus;
        User::RequestComplete(s, KErrNone);
        }
    else
        {
        iTimer.After(iStatus, aMs * 1000);
        SetActive();
        }
    }

void CPlayerEngine::DoCancel()
    {
    iTimer.Cancel();
    }

TInt CPlayerEngine::RunError(TInt aError)
    {
    Message(_L("Playback error %d"), aError);
    Pause();
    return KErrNone;
    }

TInt CPlayerEngine::ClockMs()
    {
    if (iState != EPlaying || iMode == EModeSystem) return iPosMs;
    TUint t = User::TickCount();
    TUint dt = t - iClockBaseTick;
    if (dt > 2000)
        {
        iClockBaseMs += (TInt)((dt * (TUint)iTickUs) / 1000);
        iClockBaseTick = t;
        dt = 0;
        }
    return iClockBaseMs + (TInt)((dt * (TUint)iTickUs) / 1000);
    }

void CPlayerEngine::SetClock(TInt aMs)
    {
    iClockBaseMs = aMs;
    iClockBaseTick = User::TickCount();
    }

TInt CPlayerEngine::PositionMs()
    {
    TInt p;
    if (iMode == EModeSystem && iSysVideo && iSysPrepared)
        {
        TTimeIntervalMicroSeconds t(0);
        TRAPD(err, t = iSysVideo->PositionL());
        p = (err == KErrNone) ? Ms(t) : 0;
        }
    else p = ClockMs();
    if (p < 0) p = 0;
    if (iDurationMs > 0 && p > iDurationMs) p = iDurationMs;
    return p;
    }

void CPlayerEngine::ApplyVolume()
    {
    if (iAudio && iAudioReady) iAudio->SetVolume((iAudio->MaxVolume() * iVolume) / 10);
    if (iSysVideo && iSysPrepared)
        {
        TRAPD(err, iSysVideo->SetVolumeL((iSysVideo->MaxVolume() * iVolume) / 10));
        (void)err;
        }
    }

// ------------------------------------------------------------------ open / close

void CPlayerEngine::ResetCore()
    {
    if (iDec)
        {
        RHeap* old = EnterDecoderHeap();
        viddec_close(iDec);
        LeaveDecoderHeap(old);
        iDec = NULL;
        }
    if (iMp4) { mp4_close(iMp4); iMp4 = NULL; }
    if (iSample) { mp_free(&iAlloc, iSample); iSample = NULL; }
    iSampleCap = 0;
    iHavePic = EFalse;
    }

void CPlayerEngine::Close()
    {
    Cancel();
    if (iRetry) iRetry->Cancel();
    if (iFallback) iFallback->Cancel();
    if (iStartCopy) iStartCopy->Cancel();
    delete iAudio; iAudio = NULL;
    EndAudioCopy(EFalse);
    delete iCopyIdle; iCopyIdle = NULL;
    iTriedCopy = iAudioFromCopy = EFalse;
    iAudioErr = 0;
    delete iSysVideo; iSysVideo = NULL;
    iAudioReady = iAudioPlaying = iWantPlay = iSysPrepared = iAudioFailed = iAudioRetried = EFalse;
    iVideoWhy.Zero();
    ResetCore();
    if (iFileOpen) { iFile.Close(); iFileOpen = EFalse; }
    iMode = EModeNone;
    iNext = 0;
    iPosMs = 0;
    iDurationMs = 0;
    iDropped = iShown = iDecodeErrors = iBFrames = iLateJumps = 0;
    iProfDecode = iProfConv = iProfBlit = 0;
    iTickDecode = iTickConv = iTickBlit = 0;
    iProfDecoded = iProfShown = 0;
    iCalFastSum = iCalMsSum = 0;
    iPlayTicks = 0;
    iCalFast0 = User::FastCounter();
    iCalTick0 = User::TickCount();
    iVideoDone = iAudioDone = EFalse;
    if (iState != EIdle) { iState = EIdle; iView.ClearFrame(); iObserver.PlayerStateChanged(); }
    }

void CPlayerEngine::OpenL(const TDesC& aFileName)
    {
    Close();
    iFileName = aFileName;
    User::LeaveIfError(iFile.Open(iFs, aFileName, EFileRead | EFileShareReadersOnly));
    iFileOpen = ETrue;
    TInt size = 0;
    User::LeaveIfError(iFile.Size(size));
    iIo.read = FileRead;
    iIo.opaque = this;
    iIo.size = (u32)size;

    TInt r = mp4_open(&iMp4, &iIo, &iAlloc);
    if (r != MP4_OK)
        {
        TBuf<60> why;
        if (iMp4) AppendAscii(why, iMp4->err);
        Message(_L("Cannot play: %S"), &why);
        Close();
        iFileName = aFileName;
        return;
        }
    iDurationMs = (TInt)iMp4->durationMs;
    iState = EOpening;
    iObserver.PlayerStateChanged();

    TBool own = EFalse;
    if (iMp4->hasVideo && (iMp4->video.codec == MP4_VCODEC_MPEG4 || iMp4->video.codec == MP4_VCODEC_H263 ||
                           iMp4->video.codec == MP4_VCODEC_H264) &&
        iMp4->video.count > 0)
        {
        RHeap* old = EnterDecoderHeap();
        TInt c = viddec_open(&iDec, &iAlloc, &iMp4->video);
        LeaveDecoderHeap(old);
        ApplyFastMode();
        own = ETrue;
        if (c == M4V_ERR_NOMEM && !iDec) User::Leave(KErrNoMemory);
        if (c != 0)
            {
            TBuf<70> why;
            if (iDec) AppendAscii(why, viddec_error_text(iDec));
            if (c == M4V_ERR_NOMEM) why.Copy(_L("not enough memory"));
            iVideoWhy.Copy(why);
            Message(_L("Built-in decoder: %S"), &why);
            own = EFalse;
            }
        if (own)
            {
            iSampleCap = iMp4->video.maxSize + MP_INPUT_PADDING;
            iSample = (u8*)mp_alloc(&iAlloc, iSampleCap);
            if (!iSample) User::Leave(KErrNoMemory);
            }
        }

    if (own)
        {
        iMode = EModeOwn;
        // show the first picture straight away
        iNext = 0;
        TBool pic = EFalse;
        TInt d = DecodeSample(0, pic);
        iNext = 1;
        if (d == M4V_ERR_UNSUPPORTED)
            {
            TBuf<70> why;
            AppendAscii(why, viddec_error_text(iDec));
            Message(_L("Built-in decoder: %S not supported"), &why);
            own = EFalse;
            }
        else if (pic) ShowPicture();
        }
    if (!own)
        {
        // the system players open the file themselves; ours is no longer needed
        if (iDec)
            {
            RHeap* old = EnterDecoderHeap();
            viddec_close(iDec);
            LeaveDecoderHeap(old);
            iDec = NULL;
            }
        if (iSample) { mp_free(&iAlloc, iSample); iSample = NULL; iSampleCap = 0; }
        iHavePic = EFalse;
        if (iFileOpen) { iFile.Close(); iFileOpen = EFalse; }
        }
    if (!own && iMp4->hasVideo)
        {
        iMode = EModeSystem;
        TRAPD(serr, StartSystemPlayerL());
        if (serr != KErrNone)
            {
            delete iSysVideo;
            iSysVideo = NULL;
            TBuf<100> why;
            why.Format(_L("No video decoder for this file (%d). Playing the sound only."), serr);
            FallbackToAudioOnly(why);
            }
        return;
        }
    if (!own) iMode = EModeAudioOnly;

    // audio through the phone's MMF controller (AAC / AMR); playing starts when it is ready
    iWantPlay = ETrue;
    if (iSoundOff && iMode == EModeOwn)
        {
        // no sound wanted: the video runs on its own clock, the phone decodes no audio
        iWantPlay = EFalse;
        Play();
        return;
        }
    StartAudio();
    }

// opens the sound: from a copy for H.264 files (the phone's MP4 audio player refuses them),
// else from the file itself
void CPlayerEngine::StartAudio()
    {
    if (iMode == EModeOwn && iMp4 && iMp4->video.codec == MP4_VCODEC_H264 && CanCopyAudio())
        {
        if (StartAudioCopy()) return;
        }
    OpenAudio(iFileName);
    }

void CPlayerEngine::SetSoundOff(TBool aOff)
    {
    if (aOff == iSoundOff) return;
    iSoundOff = aOff;
    if (iMode != EModeOwn || !iMp4) return;
    if (aOff)
        {
        // stop the sound now; the video goes on with its own clock
        if (iStartCopy) iStartCopy->Cancel();
        if (iRetry) iRetry->Cancel();
        EndAudioCopy(EFalse);
        if (iAudio) { if (iAudioPlaying) iAudio->Stop(); delete iAudio; iAudio = NULL; }
        iAudioReady = iAudioPlaying = iWaitAudio = EFalse;
        iWantPlay = EFalse;
        if (iState == EPlaying) SetClock(ClockMs());
        else if (iState == EOpening) Play();      // was waiting for the sound
        }
    else if (iMp4->audio.present && !iAudio)
        {
        // sound back on: open it; it joins at the current position when it is ready
        iTriedCopy = iAudioFailed = iAudioRetried = EFalse;
        iAudioErr = 0;
        iWantPlay = (iState == EOpening);
        StartAudio();
        }
    iObserver.PlayerStateChanged();
    }

void CPlayerEngine::OpenAudio(const TDesC& aName)
    {
    iAudioName = aName;
    iAudioFromCopy = (aName.CompareF(iFileName) != 0);
    TRAPD(err, iAudio = CMdaAudioPlayerUtility::NewFilePlayerL(aName, *this));
    if (err != KErrNone)
        {
        iAudio = NULL;
        iAudioFailed = ETrue;
        iAudioErr = err;
        if (iMp4 && iMp4->audio.present) Message(_L("No audio (error %d)"), err);
        if (iMode == EModeAudioOnly) { iWantPlay = EFalse; SetState(EStopped); return; }
        if (iWantPlay) { iWantPlay = EFalse; Play(); }
        }
    }

// ------------------------------------------------------------------ audio-only copy

TBool CPlayerEngine::CanCopyAudio() const
    {
    return iMp4 && iMp4->audio.present && iMp4->audioTrak && iMp4->mvhd &&
           (iMode == EModeOwn || iMode == EModeAudioOnly);
    }

TInt CPlayerEngine::StartCopyCb(TAny* aSelf)
    {
    CPlayerEngine* self = (CPlayerEngine*)aSelf;
    delete self->iAudio;            // the player that refused the file (outside its callback)
    self->iAudio = NULL;
    if (!self->StartAudioCopy()) self->CopyFailed(self->iAudioErr);
    return 0;
    }

// name, size and time of the video (and the copy's size): identifies the copy made for it
void CPlayerEngine::SourceSignature(TDes8& aSig)
    {
    TEntry e;
    aSig.Copy(iFileName.Left(KMaxFileName));
    if (iFs.Entry(iFileName, e) == KErrNone)
        aSig.AppendFormat(_L8("|%d|%u|%u"), e.iSize, e.iModified.Int64().High(), e.iModified.Int64().Low());
    if (iCopy) aSig.AppendFormat(_L8("|%u"), mp4_audio_copy_total(iCopy));
    }

// Copies the sound into <drive>:\System\Temp\mp4player_sound.mp4 (.3gp for AMR), on the
// video's drive when there is room, else on C:. The last copy is kept: playing the same file
// again starts at once.
TBool CPlayerEngine::StartAudioCopy()
    {
    iTriedCopy = ETrue;
    if (!iFileOpen && !ReopenFile()) return EFalse;
    if (mp4_audio_copy_begin(&iCopy, iMp4, &iAlloc) != MP4_OK) { iCopy = NULL; return EFalse; }
    TUint total = mp4_audio_copy_total(iCopy);
    TBuf8<KMaxFileName + 48> sig;
    SourceSignature(sig);
    TParsePtrC parse(iFileName);
    TBuf<2> drives[2];
    drives[0] = parse.Drive();
    drives[1] = _L("C:");
    TInt nDrives = (drives[0].Length() == 2 && drives[0].CompareF(drives[1]) != 0) ? 2 : 1;
    if (nDrives == 1) drives[0] = drives[1];
    TInt pass, i;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < nDrives; i++)
            {
            TFileName sigName;
            iTmpName = drives[i];
            iTmpName.Append(KTmpDir);
            iTmpName.Append(KTmpBase);
            sigName = iTmpName;
            iTmpName.Append(mp4_audio_copy_is_3gp(iCopy) ? _L(".3gp") : _L(".mp4"));
            sigName.Append(_L(".txt"));
            if (pass == 0)
                {
                // a copy made earlier for this very file?
                TEntry e;
                RFile f;
                TBuf8<KMaxFileName + 48> old;
                TBool same = EFalse;
                if (iFs.Entry(iTmpName, e) != KErrNone || (TUint)e.iSize != total) continue;
                if (f.Open(iFs, sigName, EFileRead | EFileShareAny) != KErrNone) continue;
                if (f.Read(old) == KErrNone && old == sig) same = ETrue;
                f.Close();
                if (!same) continue;
                EndAudioCopy(ETrue);
                OpenAudio(iTmpName);
                return ETrue;
                }
            TVolumeInfo vol;
            TInt drive;
            if (RFs::CharToDrive(drives[i][0], drive) != KErrNone) continue;
            if (iFs.Volume(vol, drive) != KErrNone || vol.iFree < TInt64((TInt)total + 0x20000)) continue;
            iFs.MkDirAll(iTmpName);
            iFs.Delete(sigName);
            if (iTmpFile.Replace(iFs, iTmpName, EFileWrite | EFileShareExclusive) != KErrNone) continue;
            iTmpOpen = ETrue;
            pass = 2;
            break;
            }
    if (!iTmpOpen) { EndAudioCopy(EFalse); return EFalse; }
    iCopyBuf = (u8*)mp_alloc(&iAlloc, KCopyBufSize);
    if (!iCopyIdle) iCopyIdle = CIdle::New(CActive::EPriorityIdle);
    if (!iCopyBuf || !iCopyIdle) { EndAudioCopy(EFalse); return EFalse; }
    iCopyPercent = -1;
    iCopyIdle->Start(TCallBack(CopyStepCb, this));
    Message(_L("Preparing the sound... (Space: play without it)"));
    return ETrue;
    }

int CPlayerEngine::TmpWrite(void* aOpaque, const u8* aData, int aLen)
    {
    CPlayerEngine* self = (CPlayerEngine*)aOpaque;
    return self->iTmpFile.Write(TPtrC8(aData, aLen)) == KErrNone ? 0 : -1;
    }

TInt CPlayerEngine::CopyStepCb(TAny* aSelf)
    {
    return ((CPlayerEngine*)aSelf)->CopyStep() ? 1 : 0;
    }

// one step of the copy (runs while nothing else is going on); EFalse when finished
TBool CPlayerEngine::CopyStep()
    {
    if (!iCopy || !iTmpOpen) return EFalse;
    if (!iFileOpen && !ReopenFile()) { EndAudioCopy(EFalse); CopyFailed(KErrNotReady); return EFalse; }
    TInt r = mp4_audio_copy_step(iCopy, &iIo, iCopyBuf, KCopyBufSize, TmpWrite, this);
    if (r == 0)
        {
        TUint total = mp4_audio_copy_total(iCopy);
        TInt pc = (TInt)(mp4_audio_copy_done(iCopy) / (total / 100 + 1));
        if (pc != iCopyPercent)
            {
            iCopyPercent = pc;
            Message(_L("Preparing the sound %d%% (Space: play without it)"), pc);
            }
        return ETrue;
        }
    if (r < 0)
        {
        EndAudioCopy(EFalse);
        CopyFailed(r == -2 ? KErrCorrupt : KErrWrite);
        return EFalse;
        }
    // done: close it, note which video it belongs to, play it
    TInt err = iTmpFile.Flush();
    iTmpFile.Close();
    iTmpOpen = EFalse;
    if (err == KErrNone)
        {
        TFileName sigName(iTmpName);
        TParsePtrC p(iTmpName);
        sigName.SetLength(sigName.Length() - p.Ext().Length());
        sigName.Append(_L(".txt"));
        TBuf8<KMaxFileName + 48> sig;
        SourceSignature(sig);
        RFile f;
        if (f.Replace(iFs, sigName, EFileWrite) == KErrNone) { f.Write(sig); f.Close(); }
        }
    EndAudioCopy(ETrue);
    if (err != KErrNone) { iFs.Delete(iTmpName); CopyFailed(err); return EFalse; }
    iObserver.PlayerMessage(KNullDesC);
    OpenAudio(iTmpName);
    return EFalse;
    }

void CPlayerEngine::EndAudioCopy(TBool aKeepFile)
    {
    if (iCopyIdle) iCopyIdle->Cancel();
    if (iTmpOpen)
        {
        iTmpFile.Close();
        iTmpOpen = EFalse;
        if (!aKeepFile) iFs.Delete(iTmpName);
        }
    if (iCopy) { mp4_audio_copy_end(iCopy); iCopy = NULL; }
    if (iCopyBuf) { mp_free(&iAlloc, iCopyBuf); iCopyBuf = NULL; }
    }

// no sound after all: play the video alone
void CPlayerEngine::CopyFailed(TInt aError)
    {
    iAudioFailed = ETrue;
    if (aError) iAudioErr = aError;
    if (iMode == EModeAudioOnly)
        {
        Message(_L("The phone cannot play this audio (error %d)"), iAudioErr);
        iWantPlay = EFalse;
        SetState(EStopped);
        return;
        }
    Message(_L("Video only: the phone cannot play this audio (error %d)"), iAudioErr);
    if (iWantPlay) { iWantPlay = EFalse; Play(); }
    else iObserver.PlayerStateChanged();
    }

// ------------------------------------------------------------------ MMF audio callbacks

// The audio controller couldn't open the file while we hold it open: retry with our handle
// closed, then reopen ours (outside the MMF callback).
TInt CPlayerEngine::RetryAudio(TAny* aSelf)
    {
    CPlayerEngine* self = (CPlayerEngine*)aSelf;
    delete self->iAudio;
    self->iAudio = NULL;
    if (self->iFileOpen) { self->iFile.Close(); self->iFileOpen = EFalse; }
    TRAPD(err, self->iAudio = CMdaAudioPlayerUtility::NewFilePlayerL(self->iFileName, *self));
    if (err != KErrNone)
        {
        self->iAudio = NULL;
        self->ReopenFile();
        TTimeIntervalMicroSeconds zero(0);
        self->MapcInitComplete(err, zero);
        }
    return 0;
    }

TBool CPlayerEngine::ReopenFile()
    {
    if (iFileOpen) return ETrue;
    TInt err = iFile.Open(iFs, iFileName, EFileRead | EFileShareReadersOnly);
    if (err != KErrNone) err = iFile.Open(iFs, iFileName, EFileRead | EFileShareAny);
    iFileOpen = (err == KErrNone);
    return iFileOpen;
    }

void CPlayerEngine::MapcInitComplete(TInt aError, const TTimeIntervalMicroSeconds& aDuration)
    {
    if (aError == KErrInUse && !iAudioRetried && iFileOpen && iMode == EModeOwn && !iAudioFromCopy)
        {
        iAudioRetried = ETrue;
        iRetry->CallBack();
        return;
        }
    if (aError != KErrNone) iAudioErr = aError;
    if (aError != KErrNone && !iTriedCopy && !iAudioFromCopy && CanCopyAudio())
        {
        // the controller refused the file: try an audio-only copy of it
        iStartCopy->CallBack();
        return;
        }
    if (iMode == EModeOwn && !iFileOpen && !ReopenFile())
        {
        // the audio player keeps the file to itself: play the sound only
        if (iMode == EModeOwn)
            {
            iMode = EModeAudioOnly;
            iHavePic = EFalse;
            iView.ClearFrame();
            Message(_L("The file is locked by the audio player: sound only"));
            }
        }
    if (aError == KErrNone)
        {
        iAudioReady = ETrue;
        TInt d = Ms(aDuration);
        if (d > iDurationMs) iDurationMs = d;
        ApplyVolume();
        }
    else
        {
        iAudioReady = EFalse;
        iAudioFailed = ETrue;
        if (iMp4 && iMp4->audio.present)
            {
            if (iMode == EModeAudioOnly) Message(_L("The phone cannot play this audio (error %d)"), aError);
            else Message(_L("Video only: the phone cannot play this audio (error %d)"), aError);
            }
        if (iMode == EModeAudioOnly)
            {
            iWantPlay = EFalse;
            SetState(EStopped);
            return;
            }
        }
    if (iWantPlay)
        {
        iWantPlay = EFalse;
        Play();
        }
    else if (iState == EPlaying && iAudioReady && !iAudioPlaying && iAudio)
        {
        // the sound got ready while the video was already playing (sound switched back on, or
        // its copy finished): start it at the video's position
        iAudio->SetPosition(TTimeIntervalMicroSeconds(TInt64(ClockMs()) * TInt64(1000)));
        iAudio->Play();
        iAudioPlaying = ETrue;
        iLastAudioSync = User::TickCount();
        iObserver.PlayerStateChanged();
        }
    else iObserver.PlayerStateChanged();
    }

void CPlayerEngine::MapcPlayComplete(TInt /*aError*/)
    {
    iAudioPlaying = EFalse;
    iAudioDone = ETrue;
    if (iMode == EModeAudioOnly || (iMode == EModeOwn && iVideoDone))
        {
        Cancel();
        iPosMs = iDurationMs;
        SetState(EEnded);
        }
    }

// ------------------------------------------------------------------ system video player (fallback)

void CPlayerEngine::StartSystemPlayerL()
    {
    CCoeEnv* env = CCoeEnv::Static();
    TRect r = iView.ScreenVideoArea();
    iSysVideo = CVideoPlayerUtility::NewL(*this, EMdaPriorityNormal, EMdaPriorityPreferenceTimeAndQuality,
                                          env->WsSession(), *env->ScreenDevice(),
                                          *iView.DrawableWindow(), r, r);
    iSysVideo->OpenFileL(iFileName);
    }

void CPlayerEngine::MvpuoOpenComplete(TInt aError)
    {
    if (aError != KErrNone)
        {
        TBuf<160> why;
        if (iVideoWhy.Length())
            why.Format(_L("%S: no decoder (%d). Playing the sound only."), &iVideoWhy, aError);
        else if (iMp4 && iMp4->hasVideo && iMp4->video.codec == MP4_VCODEC_H264)
            why.Format(_L("H.264 video: no decoder on this phone (%d). Playing the sound only."), aError);
        else why.Format(_L("This video format has no decoder (%d). Playing the sound only."), aError);
        FallbackLater(why);
        return;
        }
    iSysVideo->Prepare();
    }

void CPlayerEngine::MvpuoPrepareComplete(TInt aError)
    {
    if (aError != KErrNone)
        {
        TBuf<100> why;
        why.Format(_L("The system video player failed (%d). Playing the sound only."), aError);
        FallbackLater(why);
        return;
        }
    iSysPrepared = ETrue;
    TTimeIntervalMicroSeconds d(0);
    TRAPD(err, d = iSysVideo->DurationL());
    if (err == KErrNone && Ms(d) > iDurationMs) iDurationMs = Ms(d);
    ApplyVolume();
    Message(_L("Using the phone's own video decoder"));
    iSysVideo->Play();
    SetState(EPlaying);
    ScheduleIn(500);
    }

void CPlayerEngine::MvpuoFrameReady(CFbsBitmap& /*aFrame*/, TInt /*aError*/) {}

void CPlayerEngine::MvpuoPlayComplete(TInt /*aError*/)
    {
    Cancel();
    iPosMs = iDurationMs;
    SetState(EEnded);
    }

void CPlayerEngine::MvpuoEvent(const TMMFEvent& /*aEvent*/) {}

void CPlayerEngine::FallbackLater(const TDesC& aWhy)
    {
    iFallbackWhy.Copy(aWhy.Left(iFallbackWhy.MaxLength()));
    iFallback->CallBack();
    }

TInt CPlayerEngine::DoFallback(TAny* aSelf)
    {
    CPlayerEngine* self = (CPlayerEngine*)aSelf;
    TBuf<100> why(self->iFallbackWhy);
    self->FallbackToAudioOnly(why);
    return 0;
    }

void CPlayerEngine::FallbackToAudioOnly(const TDesC& aWhy)
    {
    delete iSysVideo;
    iSysVideo = NULL;
    iSysPrepared = EFalse;
    iAudioFailed = EFalse;
    iObserver.PlayerMessage(aWhy);
    iMode = EModeAudioOnly;
    iView.ClearFrame();
    iWantPlay = ETrue;
    TRAPD(err, iAudio = CMdaAudioPlayerUtility::NewFilePlayerL(iFileName, *this));
    if (err != KErrNone)
        {
        iAudio = NULL;
        iWantPlay = EFalse;
        SetState(EStopped);
        }
    }

// ------------------------------------------------------------------ transport

void CPlayerEngine::PlayPause()
    {
    if (iState == EPlaying) Pause();
    else Play();
    }

void CPlayerEngine::Play()
    {
    if (iState == EIdle || iState == EPlaying) return;
    if (iMode == EModeSystem)
        {
        if (!iSysVideo || !iSysPrepared) return;
        if (iState == EEnded || iState == EStopped)
            {
            TRAPD(err, iSysVideo->SetPositionL(TTimeIntervalMicroSeconds(0)));
            (void)err;
            }
        iSysVideo->Play();
        SetState(EPlaying);
        ScheduleIn(500);
        return;
        }
    if (iCopyIdle && iCopyIdle->IsActive())
        {
        // the user doesn't want to wait for the sound
        EndAudioCopy(EFalse);
        iAudioFailed = ETrue;
        iWantPlay = EFalse;
        }
    if (iState == EOpening && iAudio && !iAudioReady && !iAudioFailed)
        {
        iWantPlay = ETrue;          // starts when the audio is ready
        return;
        }
    if (iState == EEnded || (iDurationMs > 0 && iPosMs >= iDurationMs)) SeekToMs(0);
    iVideoDone = (iMode != EModeOwn) || (iMp4 && iNext >= iMp4->video.count);
    iAudioDone = EFalse;
    if (iAudio && iAudioReady)
        {
        iAudio->SetPosition(TTimeIntervalMicroSeconds(TInt64(iPosMs) * TInt64(1000)));
        iAudio->Play();
        iAudioPlaying = ETrue;
        }
    SetClock(iPosMs);
    iLastAudioSync = User::TickCount();
    // the phone's audio needs a moment to start: hold the picture until the sound really runs
    iWaitAudio = iAudioPlaying;
    iWaitTick0 = User::TickCount();
    iWaitFromMs = iPosMs;
    iState = EPlaying;
    iObserver.PlayerMessage(KNullDesC);
    iObserver.PlayerStateChanged();
    ScheduleIn(0);
    }

void CPlayerEngine::Pause()
    {
    if (iState != EPlaying) return;
    if (iMode == EModeSystem)
        {
        TRAPD(err, iSysVideo->PauseL());
        (void)err;
        iPosMs = PositionMs();
        Cancel();
        SetState(EPaused);
        return;
        }
    iPosMs = ClockMs();
    if (iAudioPlaying) { iAudio->Pause(); iAudioPlaying = EFalse; }
    Cancel();
    SetState(EPaused);
    if (iLastDirect && iMode == EModeOwn) ShowPicture();   // the bitmap is behind: bring it up to date
    }

void CPlayerEngine::Stop()
    {
    if (iState == EIdle) return;
    if (iCopyIdle && iCopyIdle->IsActive()) iWantPlay = EFalse;    // the copy goes on, playing waits
    Cancel();
    if (iMode == EModeSystem)
        {
        if (iSysVideo) { iSysVideo->Stop(); TRAPD(err, iSysVideo->SetPositionL(TTimeIntervalMicroSeconds(0))); (void)err; }
        iPosMs = 0;
        SetState(EStopped);
        return;
        }
    if (iAudio && iAudioReady) iAudio->Stop();
    iAudioPlaying = EFalse;
    iState = EStopped;
    SeekToMs(0);
    iObserver.PlayerStateChanged();
    }

void CPlayerEngine::SeekRelative(TInt aMs)
    {
    if (iState == EIdle || iState == EOpening) return;
    TInt target = PositionMs() + aMs;
    if (target < 0) target = 0;
    if (iDurationMs > 0 && target > iDurationMs - 500) target = iDurationMs > 500 ? iDurationMs - 500 : 0;
    if (iMode == EModeSystem)
        {
        if (iSysVideo && iSysPrepared)
            {
            TRAPD(err, iSysVideo->SetPositionL(TTimeIntervalMicroSeconds(TInt64(target) * TInt64(1000))));
            (void)err;
            }
        return;
        }
    TBool forward = aMs > 0;
    TInt from = PositionMs();
    TBool playing = iState == EPlaying;
    if (playing && iAudioPlaying) { iAudio->Pause(); iAudioPlaying = EFalse; }
    Cancel();
    // with long GOPs the key frame before the target may be behind us: take the next one
    if (iMode == EModeOwn && iMp4 && forward)
        {
        const Mp4VideoTrack* t = &iMp4->video;
        TInt k = mp4_key_before(t, mp4_sample_at(t, (u32)target));
        if ((TInt)t->timeMs[k] <= from)
            {
            TInt k2 = mp4_key_after(t, mp4_sample_at(t, (u32)from));
            if (k2 >= 0) target = (TInt)t->timeMs[k2];
            }
        }
    SeekToMs(target);
    if (playing)
        {
        iState = EPaused;
        Play();
        }
    else iObserver.PlayerStateChanged();
    }

// decodes the key frame at or before aMs and makes it the current position
void CPlayerEngine::SeekToMs(TInt aMs)
    {
    if (iMode == EModeOwn && iMp4 && iDec)
        {
        const Mp4VideoTrack* t = &iMp4->video;
        TInt k = mp4_key_before(t, mp4_sample_at(t, (u32)aMs));
        {
        RHeap* old = EnterDecoderHeap();
        viddec_flush(iDec);
        LeaveDecoderHeap(old);
        }
        TBool pic = EFalse;
        DecodeSample(k, pic);
        iNext = k + 1;
        aMs = (TInt)t->timeMs[k];
        if (pic) ShowPicture();
        iVideoDone = iNext >= t->count;
        }
    iPosMs = aMs;
    if (iAudio && iAudioReady) iAudio->SetPosition(TTimeIntervalMicroSeconds(TInt64(aMs) * TInt64(1000)));
    SetClock(aMs);
    }

void CPlayerEngine::ChangeVolume(TInt aDelta)
    {
    iVolume += aDelta;
    if (iVolume < 0) iVolume = 0;
    if (iVolume > 10) iVolume = 10;
    ApplyVolume();
    iObserver.PlayerStateChanged();
    }

// ------------------------------------------------------------------ decoding and display

TInt CPlayerEngine::DecodeSample(TInt aIndex, TBool& aPicture)
    {
    aPicture = EFalse;
    const Mp4VideoTrack* t = &iMp4->video;
    if (aIndex < 0 || aIndex >= t->count || !iFileOpen) return M4V_NOFRAME;
    TInt size = (TInt)t->size[aIndex];
    if (size <= 0) return M4V_NOFRAME;
    if (size + MP_INPUT_PADDING > iSampleCap)
        {
        mp_free(&iAlloc, iSample);
        iSampleCap = size + MP_INPUT_PADDING;
        iSample = (u8*)mp_alloc(&iAlloc, iSampleCap);
        if (!iSample) { iSampleCap = 0; return M4V_ERR_NOMEM; }
        }
    TInt got = FileRead(this, t->offset[aIndex], iSample, size);
    if (got <= 0) return M4V_ERR_GENERIC;
    mp_memset(iSample + got, 0, MP_INPUT_PADDING);
    M4vPicture pic;
    TUint32 f0 = User::FastCounter();
    TUint t0 = User::TickCount();
    TInt r = DecodeBuffer(iSample, got, &pic);
    iProfDecode += User::FastCounter() - f0;
    iTickDecode += User::TickCount() - t0;
    iProfDecoded++;
    if (r == M4V_FRAME_I || r == M4V_FRAME_P || r == M4V_FRAME_REPEAT)
        {
        iPic = pic;
        iHavePic = ETrue;
        aPicture = ETrue;
        }
    else if (r == M4V_FRAME_SKIPPED)
        {
        if (iBFrames++ == 0) Message(_L("B-frames are skipped: re-encode without them (ffmpeg -bf 0)"));
        }
    else if (r < 0 && r != M4V_ERR_NOREF) iDecodeErrors++;
    return r;
    }

void CPlayerEngine::ShowPicture()
    {
    if (!iHavePic) return;
    TBool resized = EFalse;
    TRAPD(err, resized = iView.PrepareFrameL(iPic.width, iPic.height));
    if (err != KErrNone) return;
    TInt stride, format;
    TSize size;
    // while playing, straight into the screen memory when possible (no copy through the window
    // server); otherwise, and when paused, into the frame bitmap (which also serves redraws)
    TUint8* dst = iState == EPlaying ? iView.LockDirect(stride, size, format) : NULL;
    TBool direct = dst != NULL;
    if (!dst) dst = iView.LockFrame(stride, size, format);
    if (!dst) return;
    (void)resized;
    const TRect& crop = iView.SourceCrop();
    yuvconv_setup_crop(iConv, crop.iTl.iX, crop.iTl.iY, crop.Width(), crop.Height(), size.iWidth, size.iHeight, format);
    TUint32 f0 = User::FastCounter();
    TUint t0 = User::TickCount();
    yuvconv_run(iConv, &iPic, dst, stride);
    if (!direct) iView.UnlockFrame();
    TUint32 f1 = User::FastCounter();
    TUint t1 = User::TickCount();
    if (direct) iView.UnlockDirect();
    else iView.ShowFrame();
    iLastDirect = direct;
    TUint32 f2 = User::FastCounter();
    TUint t2 = User::TickCount();
    iProfConv += f1 - f0;
    iProfBlit += f2 - f1;
    iTickConv += t1 - t0;
    iTickBlit += t2 - t1;
    iProfShown++;
    iShown++;
    Calibrate();
    }

// FastCounter's rate isn't known on Symbian 7.0s: measure it against the system tick
// (in windows of >= 5 s so that the 1/64 s tick resolution doesn't matter).
void CPlayerEngine::Calibrate()
    {
    TUint t = User::TickCount();
    TUint dt = t - iCalTick0;
    TUint ms = (dt * (TUint)iTickUs) / 1000;
    if (dt > 100000) { iCalTick0 = t; iCalFast0 = User::FastCounter(); return; }
    if (ms < 2000) return;
    // FastCounter's rate isn't known on Symbian 7.0s: measure it against the system tick
    TUint32 df = User::FastCounter() - iCalFast0;
    if (iCalMsSum < 0x10000000 && (iCalFastSum >> 1) + (df >> 1) < 0x7FFFFFFF)
        {
        iCalFastSum += df;
        iCalMsSum += ms;
        }
    iCalTick0 = t;
    iCalFast0 = User::FastCounter();
    }

// average milliseconds * 10 per item, from FastCounter if it's fine enough, else from ticks
TInt CPlayerEngine::ProfMs10(TUint32 aFast, TUint32 aTicks, TInt aCount)
    {
    if (aCount <= 0) return 0;
    if (iCalMsSum > 0 && iCalFastSum >= iCalMsSum * 4)     // better than 1/4 ms resolution
        {
        TInt64 v = TInt64((TInt)(aFast >> 1)) * TInt64((TInt)iCalMsSum) * TInt64(20);
        v = v / TInt64((TInt)(iCalFastSum >> 1)) / TInt64(aCount);
        return v.GetTInt();
        }
    return (TInt)(((aTicks * (TUint32)iTickUs) / 100) / (TUint32)aCount);
    }

void CPlayerEngine::PerfText(TDes& aText)
    {
    TInt dec = ProfMs10(iProfDecode, iTickDecode, iProfDecoded);
    TInt conv = ProfMs10(iProfConv, iTickConv, iProfShown);
    TInt blit = ProfMs10(iProfBlit, iTickBlit, iProfShown);
    aText.AppendFormat(_L("Shown %d, skipped %d, errors %d. Decode %d.%d, colour %d.%d, draw %d.%d ms. "),
                       iShown, iDropped, iDecodeErrors,
                       dec / 10, dec % 10, conv / 10, conv % 10, blit / 10, blit % 10);
    if (iLateJumps) aText.AppendFormat(_L("Jumped %d. "), iLateJumps);
    if (iBFrames) aText.AppendFormat(_L("B-frames skipped %d. "), iBFrames);
    }

void CPlayerEngine::RedrawCurrentFrame()
    {
    if (iMode == EModeOwn && iHavePic) ShowPicture();
    else iView.DrawNow();
    }

void CPlayerEngine::RunL()
    {
    if (iState != EPlaying) return;
    TUint tick = User::TickCount();

    // keep the screen saver / backlight timeout away while playing
    if ((tick - iLastInactivityReset) * (TUint)iTickUs > 4000000)
        {
        User::ResetInactivityTime();
        iLastInactivityReset = tick;
        }

    if (iMode == EModeSystem)
        {
        iObserver.PlayerStateChanged();
        ScheduleIn(500);
        return;
        }

    // start: wait (up to 1.5 s) until the audio position moves, then start the video clock there
    if (iWaitAudio && iMode == EModeOwn)
        {
        TTimeIntervalMicroSeconds p(0);
        TInt a = -1;
        if (iAudioPlaying && iAudio->GetPosition(p) == KErrNone) a = Ms(p);
        TUint waited = ((tick - iWaitTick0) * (TUint)iTickUs) / 1000;
        if (a > iWaitFromMs + 10 || waited > 1500 || !iAudioPlaying)
            {
            iWaitAudio = EFalse;
            SetClock(a > iWaitFromMs ? a : iWaitFromMs);
            iLastAudioSync = tick;
            }
        else
            {
            ScheduleIn(20);
            return;
            }
        }

    // follow the audio clock: big differences at once, small ones halved (no visible jumps)
    if (iAudioPlaying && (tick - iLastAudioSync) * (TUint)iTickUs > (TUint)KAudioSyncTicksMs * 1000)
        {
        iLastAudioSync = tick;
        TTimeIntervalMicroSeconds p(0);
        if (iAudio->GetPosition(p) == KErrNone)
            {
            TInt a = Ms(p);
            TInt now = ClockMs();
            TInt diff = a - now;
            if (a > 0)
                {
                if (diff > KAudioSyncTolerance * 3 || diff < -KAudioSyncTolerance * 3) SetClock(a);
                else if (diff > KAudioSyncSmall || diff < -KAudioSyncSmall) SetClock(now + diff / 2);
                }
            }
        }

    TInt now = ClockMs();
    if (iMode == EModeAudioOnly)
        {
        if (iDurationMs > 0 && now >= iDurationMs && !iAudioPlaying) { iPosMs = iDurationMs; SetState(EEnded); return; }
        iObserver.PlayerStateChanged();
        ScheduleIn(500);
        return;
        }

    const Mp4VideoTrack* t = &iMp4->video;
    TInt count = t->count;
    TInt decoded = 0;
    TBool pic = EFalse;

    // far behind (slow phone, big video): skip to the latest key frame that is due
    if (iNext < count && now - (TInt)t->timeMs[iNext] > KMaxLateMs)
        {
        TInt k = mp4_key_before(t, mp4_sample_at(t, (u32)now));
        if (k > iNext)
            {
            iDropped += k - iNext;
            iLateJumps += k - iNext;
            iNext = k;
            }
        }
    // Decode what is due. Every frame has to be decoded (the next ones are predicted from it), but
    // not every one has to be shown: while the video is behind the sound, keep decoding and only
    // show a picture when it has caught up, or when nothing was shown for KMaxNoShowMs (so the
    // picture still moves). After KMaxRunMs the loop yields so keys still work. This keeps a
    // slow video in sync at a lower frame rate.
    while (iNext < count && (TInt)t->timeMs[iNext] <= now + 5)
        {
        TBool got = EFalse;
        TInt r = DecodeSample(iNext, got);
        iNext++;
        decoded++;
        if (got) pic = ETrue;
        if (r == M4V_ERR_NOREF)
            {
            // P-frame without its reference: wait for the next key frame
            TInt k = mp4_key_after(t, iNext - 1);
            if (k < 0) iNext = count;
            else if ((TInt)t->timeMs[k] <= now + 5) iNext = k;
            }
        now = ClockMs();
        if (iNext >= count || now - (TInt)t->timeMs[iNext] < KBehindMs) break;   // caught up
        if ((TInt)(((User::TickCount() - tick) * (TUint)iTickUs) / 1000) >= KMaxRunMs) break;
        }
    TBool show = pic;
    if (pic && iNext < count && now - (TInt)t->timeMs[iNext] >= KBehindMs &&
        (TInt)(((User::TickCount() - iLastShowTick) * (TUint)iTickUs) / 1000) < KMaxNoShowMs)
        show = EFalse;      // still behind: the next RunL goes on decoding first
    iDropped += show ? decoded - 1 : decoded;
    if (show)
        {
        ShowPicture();
        iLastShowTick = User::TickCount();
        }

    if (iNext >= count) iVideoDone = ETrue;
    if (iVideoDone && !iAudioPlaying)
        {
        TInt end = count > 0 ? (TInt)t->timeMs[count - 1] : 0;
        if (iAudioDone || !iAudio || !iAudioReady || now >= end + 200)
            {
            iPosMs = iDurationMs > 0 ? iDurationMs : now;
            SetState(EEnded);
            return;
            }
        }

    // status bar about twice a second
    if ((tick - iLastStatusTick) * (TUint)iTickUs >= 500000)
        {
        iLastStatusTick = tick;
        iObserver.PlayerStateChanged();     // the view redraws only if the panel would change
        }

    TInt delay = 100;
    if (iNext < count)
        {
        delay = (TInt)t->timeMs[iNext] - ClockMs();
        if (delay > 100) delay = 100;
        }
    ScheduleIn(delay);
    }

// ------------------------------------------------------------------ fast H.264

TBool CPlayerEngine::FastNow() const
    {
    if (!iDec || !viddec_is_h264(iDec)) return EFalse;
    if (iFastMode == EFastOn) return ETrue;
    if (iFastMode == EFastOff || !iMp4) return EFalse;
    return iMp4->video.width * iMp4->video.height > 320 * 240;
    }

void CPlayerEngine::ApplyFastMode()
    {
    if (iDec) viddec_set_fast(iDec, FastNow() ? 1 : 0);
    }

// ------------------------------------------------------------------ info

void CPlayerEngine::InfoText(TDes& aText)
    {
    aText.Zero();
    if (!iMp4 && iMode != EModeSystem)
        {
        aText.Append(_L("No file open."));
        return;
        }
    if (iMode == EModeOwn) PerfText(aText);
    if (iMp4 && iMp4->hasVideo)
        {
        const Mp4VideoTrack& v = iMp4->video;
        TBuf<48> codec;
        if (iDec && viddec_is_h264(iDec))
            {
            AppendAscii(codec, viddec_stream_info(iDec));
            if (FastNow()) codec.Append(_L(" (fast)"));
            }
        else AppendAscii(codec, mp4_codec_name(v.codec));
        TInt fps10 = v.durationMs > 0 ? (TInt)((TInt64(v.count) * TInt64(10000) / TInt64((TInt)v.durationMs)).GetTInt()) : 0;
        aText.AppendFormat(_L("%S %dx%d %d.%d fps. "), &codec, v.width, v.height, fps10 / 10, fps10 % 10);
        }
    if (iMp4 && iMp4->audio.present)
        {
        const Mp4AudioInfo& a = iMp4->audio;
        TBuf<12> codec;
        if (a.fourcc == MP4_FOURCC('m','p','4','a')) codec.Copy(a.objectType == 0x40 || a.objectType == 0x67 ? _L("AAC") : _L("MPEG audio"));
        else if (a.fourcc == MP4_FOURCC('s','a','m','r')) codec.Copy(_L("AMR-NB"));
        else if (a.fourcc == MP4_FOURCC('s','a','w','b')) codec.Copy(_L("AMR-WB"));
        else codec.Copy(_L("other"));
        TBuf<40> how;
        if (iAudioReady) { if (iAudioFromCopy) how.Copy(_L(" (played from a copy)")); }
        else if (iSoundOff && iMode == EModeOwn) how.Copy(_L(" (sound off: M)"));
        else if (iCopyIdle && iCopyIdle->IsActive()) how.Copy(_L(" (preparing)"));
        else if (iAudioErr) how.Format(_L(" (not playable, error %d)"), iAudioErr);
        else how.Copy(KNotPlayable);
        aText.AppendFormat(_L("%S %d Hz %dch%S. "), &codec, a.sampleRate, a.channels, &how);
        }
    else aText.Append(_L("No audio. "));
    switch (iMode)
        {
        case EModeOwn:
            {
            TSize fs = iView.FrameSize(), as = iView.AreaSize();
            aText.AppendFormat(_L("Shown at %dx%d in %dx%d."), fs.iWidth, fs.iHeight, as.iWidth, as.iHeight);
            }
            break;
        case EModeSystem: aText.Append(_L("Phone's own video decoder.")); break;
        case EModeAudioOnly: aText.Append(_L("Sound only.")); break;
        default: break;
        }
    }

TInt CPlayerEngine::DecodeBuffer(u8* aBuf, TInt aLen, M4vPicture* aPic)
    {
    RHeap* old = EnterDecoderHeap();
    TInt r = viddec_decode(iDec, aBuf, aLen, aPic);
    LeaveDecoderHeap(old);
    return r;
    }

// ------------------------------------------------------------------ speed test

// Playback timing per step is unreliable on the 9300: the only timer ticks at 64 Hz and playback
// runs in step with it. Here every step runs in a long loop and only the loop is timed.
void CPlayerEngine::Benchmark(TDes& aText)
    {
    aText.Zero();
    if (iMode != EModeOwn || !iMp4 || !iDec || !iFileOpen) { aText.Append(_L("No video open.")); return; }
    if (iState == EPlaying) Pause();
    TInt resumeMs = iPosMs;
    const Mp4VideoTrack* t = &iMp4->video;
    TInt n = t->count < 150 ? t->count : 150;
    TInt i, total = 0;
    for (i = 0; i < n; i++) total += (TInt)t->size[i] + MP_INPUT_PADDING;
    u8* all = (u8*)mp_alloc(&iAlloc, total);
    TInt* offs = (TInt*)mp_alloc(&iAlloc, n * (TInt)sizeof(TInt));
    u8* work = (u8*)mp_alloc(&iAlloc, t->maxSize + MP_INPUT_PADDING);
    if (!all || !offs || !work)
        {
        mp_free(&iAlloc, all); mp_free(&iAlloc, offs); mp_free(&iAlloc, work);
        aText.Append(_L("Not enough memory."));
        return;
        }

    // 1. read the samples from the file
    TUint t0 = User::TickCount();
    TInt pos = 0;
    for (i = 0; i < n; i++)
        {
        offs[i] = pos;
        FileRead(this, t->offset[i], all + pos, (TInt)t->size[i]);
        pos += (TInt)t->size[i] + MP_INPUT_PADDING;     // padding stays zero (mp_alloc)
        }
    TUint tRead = User::TickCount() - t0;

    // 2. decode them back to back (each from a fresh copy: the H.264 decoder modifies its input)
    M4vPicture pic;
    TBool have = EFalse;
    RHeap* oldHeap = EnterDecoderHeap();
    viddec_flush(iDec);
    LeaveDecoderHeap(oldHeap);
    t0 = User::TickCount();
    for (i = 0; i < n; i++)
        {
        Mem::Copy(work, all + offs[i], (TInt)t->size[i] + MP_INPUT_PADDING);
        TInt r = DecodeBuffer(work, (TInt)t->size[i], &pic);
        if (r == M4V_FRAME_I || r == M4V_FRAME_P || r == M4V_FRAME_REPEAT) { iPic = pic; have = ETrue; }
        }
    TUint tDec = User::TickCount() - t0;
    // second pass: checksum of the decoded pictures, to compare with the PC
    u32 hash = 2166136261u;
    oldHeap = EnterDecoderHeap();
    viddec_flush(iDec);
    LeaveDecoderHeap(oldHeap);
    for (i = 0; i < n; i++)
        {
        Mem::Copy(work, all + offs[i], (TInt)t->size[i] + MP_INPUT_PADDING);
        TInt r = DecodeBuffer(work, (TInt)t->size[i], &pic);
        if (r == M4V_FRAME_I || r == M4V_FRAME_P || r == M4V_FRAME_REPEAT) { hash = m4v_picture_hash(&pic, hash); iPic = pic; }
        }
    iHavePic = have;
    mp_free(&iAlloc, all);
    mp_free(&iAlloc, offs);
    mp_free(&iAlloc, work);

    // 3. colour conversion into the screen bitmap, and into ordinary memory
    const TInt KLoops = 60;
    TUint tConvBmp = 0, tConvRam = 0, tBlit = 0, tCopy = 0, tDirect = 0;
    TSize size;
    if (have)
        {
        ShowPicture();                      // sets up the bitmap and the converter
        TInt stride, format;
        TUint8* dst = iView.LockFrame(stride, size, format);
        iView.UnlockFrame();
        if (dst)
            {
            t0 = User::TickCount();
            for (i = 0; i < KLoops; i++)
                {
                dst = iView.LockFrame(stride, size, format);
                yuvconv_run(iConv, &iPic, dst, stride);
                iView.UnlockFrame();
                }
            tConvBmp = User::TickCount() - t0;
            TInt bytes = stride * size.iHeight;
            u8* ram = (u8*)mp_alloc(&iAlloc, bytes);
            u8* ram2 = (u8*)mp_alloc(&iAlloc, bytes);
            if (ram && ram2)
                {
                t0 = User::TickCount();
                for (i = 0; i < KLoops; i++) yuvconv_run(iConv, &iPic, ram, stride);
                tConvRam = User::TickCount() - t0;
                t0 = User::TickCount();
                for (i = 0; i < KLoops; i++) Mem::Copy(ram2, ram, bytes);
                tCopy = User::TickCount() - t0;
                }
            mp_free(&iAlloc, ram);
            mp_free(&iAlloc, ram2);
            // 4. drawing the bitmap to the screen
            t0 = User::TickCount();
            for (i = 0; i < KLoops; i++) iView.ShowFrame();
            tBlit = User::TickCount() - t0;
            // 5. colour conversion straight into the screen memory
            TInt dstride, dformat;
            TSize dsize;
            if (iView.LockDirect(dstride, dsize, dformat))
                {
                t0 = User::TickCount();
                for (i = 0; i < KLoops; i++)
                    {
                    dst = iView.LockDirect(dstride, dsize, dformat);
                    if (!dst) break;
                    yuvconv_run(iConv, &iPic, dst, dstride);
                    iView.UnlockDirect();
                    }
                if (i == KLoops) tDirect = User::TickCount() - t0;
                }
            iView.ShowFrame();
            }
        }

    // results in tenths of a millisecond per item
    TUint us = (TUint)iTickUs;
    TInt read10 = n ? (TInt)((tRead * us / 100) / (TUint)n) : 0;
    TInt dec10 = n ? (TInt)((tDec * us / 100) / (TUint)n) : 0;
    TInt cb10 = (TInt)((tConvBmp * us / 100) / KLoops);
    TInt cr10 = (TInt)((tConvRam * us / 100) / KLoops);
    TInt bl10 = (TInt)((tBlit * us / 100) / KLoops);
    TInt cp10 = (TInt)((tCopy * us / 100) / KLoops);
    aText.AppendFormat(_L("%d frames %dx%d. Per frame: read %d.%d, decode %d.%d ms. "), n, t->width, t->height,
                       read10 / 10, read10 % 10, dec10 / 10, dec10 % 10);
    aText.AppendFormat(_L("Colour %dx%d: into bitmap %d.%d, into RAM %d.%d ms. Draw %d.%d ms. Copy %d KB %d.%d ms."),
                       size.iWidth, size.iHeight, cb10 / 10, cb10 % 10, cr10 / 10, cr10 % 10, bl10 / 10, bl10 % 10,
                       (size.iWidth * size.iHeight * 2) / 1024, cp10 / 10, cp10 % 10);
    if (tDirect)
        {
        TInt di10 = (TInt)((tDirect * us / 100) / KLoops);
        aText.AppendFormat(_L(" Direct to screen (colour + show) %d.%d ms."), di10 / 10, di10 % 10);
        }
    else
        {
        _LIT(KOff, "off (D)");
        aText.AppendFormat(_L(" Direct drawing: %S."), iView.DirectEnabled() ? &iView.DirectWhy() : &KOff());
        }
    _LIT(KFast, " (fast H.264)");
    aText.AppendFormat(_L(" Check %08X%S."), (TUint)hash, FastNow() ? &KFast() : &KNullDesC());
    SeekToMs(resumeMs);
    iObserver.PlayerStateChanged();
    }
