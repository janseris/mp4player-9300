// MP4 Player 9300 - a video player for the Nokia 9300/9500 (Series 80 2.0, Symbian 7.0s).
//
// Video: own MP4/3GP demuxer + MPEG-4 Part 2 / H.263 software decoder (core/), colour
// conversion straight into a CFbsBitmap. Audio: the phone's MMF audio controller plays the
// same file (AAC / AMR); it is also used to correct the video clock. Files with a video codec
// the built-in decoder can't handle (e.g. H.264) are handed to the system CVideoPlayerUtility.

#ifndef MP4PLAYER_H
#define MP4PLAYER_H

#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <coecntrl.h>
#include <fbs.h>
#include <bitdev.h>
#include <bitstd.h>
#include <f32file.h>
#include <mdaaudiosampleplayer.h>
#include <videoplayer.h>
#include <ckndgopn.h>


#include "mpcommon.h"
#include "mp4demux.h"
#include "m4vdec.h"
#include "yuv2rgb.h"
#include "viddec.h"

const TUid KUidMp4Player = { 0x0F5A9301 };

class CVideoView;
class CPlayerEngine;

// ------------------------------------------------------------------ engine

class MPlayerObserver
    {
public:
    virtual void PlayerStateChanged() = 0;
    virtual void PlayerMessage(const TDesC& aText) = 0;
    };

class CPlayerEngine : public CActive, public MMdaAudioPlayerCallback, public MVideoPlayerUtilityObserver
    {
public:
    enum TState { EIdle, EOpening, EPlaying, EPaused, EStopped, EEnded };
    enum TMode { EModeNone, EModeOwn, EModeSystem, EModeAudioOnly };

    static CPlayerEngine* NewL(CVideoView& aView, MPlayerObserver& aObserver);
    ~CPlayerEngine();

    void OpenL(const TDesC& aFileName);
    void Close();
    void PlayPause();
    void Play();
    void Pause();
    void Stop();
    void SeekRelative(TInt aMs);
    void ChangeVolume(TInt aDelta);
    void RedrawCurrentFrame();      // after a layout change

    TState State() const { return iState; }
    TMode Mode() const { return iMode; }
    TInt PositionMs();
    TInt DurationMs() const { return iDurationMs; }
    TInt Volume() const { return iVolume; }
    const TDesC& FileName() const { return iFileName; }
    TBool HasAudio() const { return iAudioReady; }
    void InfoText(TDes& aText);
    void Benchmark(TDes& aText);    // times decode / colour / draw in long loops
    void Analyze(const TDesC& aFileName, TDes& aText);  // file parameters + decode test, no playing
    TInt DroppedFrames() const { return iDropped; }
    // H.264 without the deblocking filter: faster, a little blocky. Auto: only for videos
    // bigger than 320x240, which the phone can't decode at full speed otherwise.
    enum TFastMode { EFastAuto, EFastOff, EFastOn };
    void SetFastMode(TFastMode aMode) { iFastMode = aMode; ApplyFastMode(); }
    TFastMode FastMode() const { return iFastMode; }
    // no sound: the phone doesn't decode the audio, which leaves more time for the video
    void SetSoundOff(TBool aOff);
    TBool SoundOff() const { return iSoundOff; }
    TBool LastDirect() const { return iLastDirect; }    // the last picture went straight to the screen
    // a video too slow to decode in time: keep in time with the sound, or draw more (smooth)
    enum TBehind { EBehindAuto, EBehindSync, EBehindSmooth };
    void SetBehind(TBehind aMode) { iBehind = aMode; }
    TBehind Behind() const { return iBehind; }
    TBool SyncNow() const;
    TBool TooSlow() const { return iTooSlow; }
    TBool FastNow() const;
    TInt ShownFrames() const { return iShown; }

private:
    CPlayerEngine(CVideoView& aView, MPlayerObserver& aObserver);
    void ConstructL();
    // CActive
    void RunL();
    void DoCancel();
    TInt RunError(TInt aError);
    // MMdaAudioPlayerCallback
    void MapcInitComplete(TInt aError, const TTimeIntervalMicroSeconds& aDuration);
    void MapcPlayComplete(TInt aError);
    // MVideoPlayerUtilityObserver
    void MvpuoOpenComplete(TInt aError);
    void MvpuoPrepareComplete(TInt aError);
    void MvpuoFrameReady(CFbsBitmap& aFrame, TInt aError);
    void MvpuoPlayComplete(TInt aError);
    void MvpuoEvent(const TMMFEvent& aEvent);

    void ScheduleIn(TInt aMs);
    TInt ClockMs();
    void SetClock(TInt aMs);
    TInt DecodeSample(TInt aIndex, TBool& aPicture);
    void ShowPicture();
    void SeekToMs(TInt aMs);
    void StartSystemPlayerL();
    void FallbackToAudioOnly(const TDesC& aWhy);
    void SetState(TState aState);
    void Message(TRefByValue<const TDesC> aFmt, ...);
    void ApplyVolume();
    void ResetCore();
    // H.264 allocates from the current thread heap: run decoder calls on the decoder heap
    RHeap* EnterDecoderHeap() { return iHeap ? User::SwitchHeap(iHeap) : NULL; }
    void LeaveDecoderHeap(RHeap* aOld) { if (aOld) User::SwitchHeap(aOld); }
    TInt DecodeBuffer(u8* aBuf, TInt aLen, M4vPicture* aPic);
    static TInt RetryAudio(TAny* aSelf);
    static TInt DoFallback(TAny* aSelf);
    void FallbackLater(const TDesC& aWhy);
    TBool ReopenFile();
    static void* HeapAlloc(void* aOpaque, int aSize);
    static void HeapFree(void* aOpaque, void* aPtr);
    static int FileRead(void* aOpaque, u32 aOffset, void* aBuf, int aLen);
    // sound for files the phone's MP4 audio player refuses (H.264 video): an audio-only copy
    void OpenAudio(const TDesC& aName);
    TBool CanCopyAudio() const;
    static TInt StartCopyCb(TAny* aSelf);
    TBool StartAudioCopy();
    static TInt CopyStepCb(TAny* aSelf);
    TBool CopyStep();
    void EndAudioCopy(TBool aKeepFile);
    void CopyFailed(TInt aError);
    void SourceSignature(TDes8& aSig);
    static int TmpWrite(void* aOpaque, const u8* aData, int aLen);

private:
    CVideoView& iView;
    MPlayerObserver& iObserver;
    RFs iFs;
    RFile iFile;
    TBool iFileOpen;
    TFileName iFileName;
    RTimer iTimer;
    RHeap* iHeap;               // decoder heap (own chunk)
    MpAlloc iAlloc;
    MpIo iIo;
    Mp4File* iMp4;
    VidDec* iDec;               // video decoder (MPEG-4 Part 2 / H.263 / H.264)
    YuvConv* iConv;
    u8* iSample;
    TInt iSampleCap;
    M4vPicture iPic;
    TBool iHavePic;

    TState iState;
    TMode iMode;
    TInt iNext;                 // next video sample to decode
    TInt iDurationMs;
    TInt iPosMs;                // position when not playing
    TInt iClockBaseMs;          // clock: position at iClockBaseTick
    TUint iClockBaseTick;
    TInt iTickUs;
    TUint iLastAudioSync;
    TUint iLastInactivityReset;
    TUint iLastStatusTick;
    TUint iLastShowTick;
    TBool iWaitAudio;           // holding the video until the sound has started
    TUint iWaitTick0;
    TInt iWaitFromMs;
    TBool iVideoDone;
    TBool iAudioDone;
    TInt iDropped, iShown, iDecodeErrors, iBFrames;
    TInt iLateJumps;            // frames jumped over to catch up (key frame jumps)
    // profiling with User::FastCounter, calibrated against User::TickCount
    TUint32 iProfDecode, iProfConv, iProfBlit;
    TInt iProfDecoded, iProfShown;
    TUint32 iCalFast0;
    TUint iCalTick0;
    TUint32 iCalFastSum;        // FastCounter counts over iCalMsSum milliseconds
    TUint32 iCalMsSum;
    TUint32 iTickDecode, iTickConv, iTickBlit;  // the same sums in system ticks (fallback)
    TUint iPlayTicks;           // ticks spent playing (for the CPU load figure)
    TUint iPlayTick0;
    void Calibrate();
    TInt ProfMs10(TUint32 aFast, TUint32 aTicks, TInt aCount);
    void PerfText(TDes& aText);
    TInt iVolume;               // 0..10

    CMdaAudioPlayerUtility* iAudio;
    TBool iAudioReady;
    TBool iAudioFailed;
    TBool iAudioRetried;        // KErrInUse: retried with our file handle closed
    CAsyncCallBack* iRetry;
    CAsyncCallBack* iFallback;  // leaves the system video player outside its own callbacks
    TBuf<100> iFallbackWhy;
    TBuf<70> iVideoWhy;         // why the built-in decoder refused the video (for the fallback message)
    TBool iAudioPlaying;
    TBool iWantPlay;            // start playing when the audio is initialised
    CVideoPlayerUtility* iSysVideo;
    TBool iSysPrepared;
    TInt iAudioErr;             // why the phone refused the sound (for the info text)
    TFileName iAudioName;       // file the audio player plays (the video or the copy)
    CAsyncCallBack* iStartCopy;
    CIdle* iCopyIdle;           // runs the copy in steps
    Mp4AudioCopy* iCopy;
    u8* iCopyBuf;
    RFile iTmpFile;
    TBool iTmpOpen;
    TFileName iTmpName;
    TBool iTriedCopy;
    TBool iAudioFromCopy;
    TInt iCopyPercent;
    TFastMode iFastMode;
    void ApplyFastMode();
    TBool iLastDirect;
    TBool iSoundOff;
    void StartAudio();
    TInt MaxNoShowMs();
    TBool iTooSlow;             // decoding alone is slower than the frame rate
    TBool iSlowChecked;
    TBool iCopySkipped;         // Space pressed while the sound was being prepared
    u8* iRdBuf;                 // read-ahead buffer (KReadAhead bytes at iRdPos, iRdLen valid)
    u32 iRdPos;
    TInt iRdLen;
    TBool iAutoFast;            // auto H.264 mode's current choice
    TInt iAutoN0;               // decode statistics at the last check
    TUint32 iAutoDec0, iAutoTick0;
    TBehind iBehind;
    void AutoFastCheck();          // the last picture went straight to the screen (the bitmap is older)
    };

// ------------------------------------------------------------------ view

class CVideoView : public CCoeControl, public MDirectScreenAccess
    {
public:
    // Direct drawing: the colour conversion writes straight into the screen memory (found and
    // checked by a self-test), which saves copying every frame through the window server.
    enum TDirectState { EDirectUntested, EDirectTesting, EDirectOk, EDirectFailed };
    void SetDirect(TBool aEnabled, TDirectState aState) { iDirectEnabled = aEnabled; iDirectState = aState; }
    TBool DirectEnabled() const { return iDirectEnabled; }
    TDirectState DirectState() const { return iDirectState; }
    // the screen memory under the frame, or NULL when direct drawing isn't possible right now
    TUint8* LockDirect(TInt& aStride, TSize& aSize, TInt& aFormat);
    void UnlockDirect();
    const TDesC& DirectWhy() const { return iDirectWhy; }
    // MDirectScreenAccess
    void AbortNow(RDirectScreenAccess::TTerminationReasons aReason);
    void Restart(RDirectScreenAccess::TTerminationReasons aReason);
    enum TZoom { EZoomFit, EZoomFill, EZoomOriginal };    // original = 1:1, cropped when too big
    enum TCropPos { ECropTop, ECropMiddle, ECropBottom };
    static CVideoView* NewL(const TRect& aRect);
    ~CVideoView();
    void SetEngine(CPlayerEngine* aEngine) { iEngine = aEngine; }
    void SetFullScreen(TBool aFull) { iFullScreen = aFull; }
    TBool FullScreen() const { return iFullScreen; }
    void ToggleZoom() { iZoom = (iZoom == EZoomFit) ? EZoomFill : (iZoom == EZoomFill ? EZoomOriginal : EZoomFit); }
    void SetZoom(TZoom aZoom) { iZoom = aZoom; }
    void NextCropPos() { iCropPos = (iCropPos == ECropTop) ? ECropMiddle : (iCropPos == ECropMiddle ? ECropBottom : ECropTop); }
    void SetCropPos(TCropPos aPos) { iCropPos = aPos; }
    TCropPos CropPos() const { return iCropPos; }
    const TRect& SourceCrop() const { return iSrcCrop; }   // part of the video shown
    TDisplayMode BitmapMode() const { return iMode; }
    TSize FrameSize() const { return iFrame->Handle() ? iFrame->SizeInPixels() : TSize(0, 0); }
    TSize AreaSize() const { return VideoArea().Size(); }
    TZoom Zoom() const { return iZoom; }

    // video frame output
    TBool PrepareFrameL(TInt aVideoW, TInt aVideoH);   // (re)creates the bitmap; ETrue if new size
    TUint8* LockFrame(TInt& aStride, TSize& aSize, TInt& aFormat);
    void UnlockFrame();
    void ShowFrame();
    void ClearFrame();
    TRect VideoArea() const;
    TRect ScreenVideoArea() const;      // in screen coordinates (for the system player)

    void SetMessage(const TDesC& aText);
    void DrawStatusNow();

    TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
    void Draw(const TRect& aRect) const;
    void SizeChanged();

private:
    CVideoView() : iZoom(EZoomFit), iCropPos(ECropMiddle) {}
    void ConstructL(const TRect& aRect);
    void DrawStatus(CGraphicsContext& aGc, const TRect& aRect) const;
    void DrawHelp(CGraphicsContext& aGc, const TRect& aRect) const;
    void BlitStatus(CWindowGc& aGc) const;  // panel drawn off-screen, then copied: no flicker
    TUint StatusSignature() const;
    void EnsurePanel();
    TRect StatusRect() const;
    void FormatTime(TDes& aBuf, TInt aMs) const;

    CPlayerEngine* iEngine;
    CFbsBitmap* iFrame;
    CFbsBitmap* iPanel;             // off-screen status panel
    CFbsBitmapDevice* iPanelDev;
    CFbsBitGc* iPanelGc;
    mutable TUint iStatusSig;       // what the panel on screen shows (to skip identical redraws)
    TPoint iFramePos;
    TRect iSrcCrop;
    TInt iVideoW, iVideoH;
    TBool iHaveFrame;
    TBool iFullScreen;
    TZoom iZoom;
    TCropPos iCropPos;
    TDisplayMode iMode;
    TBuf<120> iMessage;
    // direct drawing
    void StartDsa();
    TRect FrameScreenRect() const;
    TBool DirectSelfTest();
    TInt FrameFormat() const;
    CDirectScreenAccess* iDsa;
    TBool iDsaOn;
    TBool iDirectEnabled;
    TDirectState iDirectState;
    TUint8* iScrBase;           // screen pixel (0, 0)
    TInt iScrStride;
    TRect iDirectRect;          // screen rectangle being written
    TBuf<100> iDirectWhy;
    TUint iDsaRetryTick;        // direct access given up (picture covered) at this tick
    };

// ------------------------------------------------------------------ app framework

class CMp4PlayerAppUi : public CEikAppUi, public MPlayerObserver, public MCknOpenFileDialogObserver
    {
public:
    void ConstructL();
    ~CMp4PlayerAppUi();
    void HandleCommandL(TInt aCommand);
    TBool ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail);
    void HandleForegroundEventL(TBool aForeground);
    // MPlayerObserver
    void PlayerStateChanged();
    void PlayerMessage(const TDesC& aText);
    // MCknOpenFileDialogObserver
    TBool ShowFileInFileListL(const TParsePtrC& aParse, const TUidType& aType, const TDataType& aDataType);

    void OpenFileDialogL();
    void OpenFileL(const TDesC& aName);
    void SetFullScreenL(TBool aFull);
    void UpdateCbaL();
    void ShowFileInfoL();
    void AnalyzeFileL();
    TBool WriteTextFile(const TDesC& aName, const TDesC& aText);
    static TInt StartupOpen(TAny* aSelf);
    CVideoView* iView;
    CPlayerEngine* iEngine;
    TFileName iLastPath;
    TFileName iStartupFile;
    CAsyncCallBack* iStartup;
    TBool iCbaPlaying;
    TRect iWindowRect;          // view rect with the buttons shown (ClientRect is wrong after full screen)
    void LoadSettings();
    void SaveSettings();
    void ShowZoomMessage();
    void SettingsText(TDes& aText);     // all settings in short form, for the info and speed test
    };

class CMp4PlayerDocument : public CEikDocument
    {
public:
    CMp4PlayerDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
    CEikAppUi* CreateAppUiL() { return new (ELeave) CMp4PlayerAppUi; }
    CFileStore* OpenFileL(TBool aDoOpen, const TDesC& aFilename, RFs& aFs);
    };

class CMp4PlayerApplication : public CEikApplication
    {
public:
    TUid AppDllUid() const;
protected:
    CApaDocument* CreateDocumentL() { return new (ELeave) CMp4PlayerDocument(*this); }
    };

#endif
