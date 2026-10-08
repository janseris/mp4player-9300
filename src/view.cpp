// MP4 Player 9300 - the video view: frame bitmap, status bar, keys.

#include <eikenv.h>
#include <eikappui.h>
#include <gdi.h>
#include <e32svr.h>
#include "mp4player.h"
#include "mp4player.hrh"

const TInt KStatusPad = 3;
_LIT(KNoFileText, "No file. Press Open (Ctrl+O).");

CVideoView* CVideoView::NewL(const TRect& aRect)
    {
    CVideoView* self = new (ELeave) CVideoView;
    CleanupStack::PushL(self);
    self->ConstructL(aRect);
    CleanupStack::Pop(self);
    return self;
    }

void CVideoView::ConstructL(const TRect& aRect)
    {
    CreateWindowL();
    iMode = iEikonEnv->ScreenDevice()->DisplayMode();
    if (iMode != EColor64K && iMode != EColor4K && iMode != EColor16M && iMode != EColor16MU) iMode = EColor64K;
    iFrame = new (ELeave) CFbsBitmap;
    SetRect(aRect);
    ActivateL();
    }

CVideoView::~CVideoView()
    {
    delete iDsa;
    delete iPanelGc;
    delete iPanelDev;
    delete iPanel;
    delete iFrame;
    }

void CVideoView::SizeChanged()
    {
    iHaveFrame = EFalse;
    EnsurePanel();
    }

// The status panel is a column on the right: the screen is wide (463 px next to the CBA) but
// only 200 px high, so a 320x200 or 320x180 video keeps its full height and plays 1:1.
const TInt KPanelWidth = 136;

TRect CVideoView::StatusRect() const
    {
    TRect r = Rect();
    if (iFullScreen) return TRect(r.iBr, r.iBr);
    TInt w = KPanelWidth;
    if (w > r.Width() / 2) w = r.Width() / 2;
    r.iTl.iX = r.iBr.iX - w;
    return r;
    }

TRect CVideoView::VideoArea() const
    {
    TRect r = Rect();
    if (!iFullScreen) r.iBr.iX = StatusRect().iTl.iX;
    return r;
    }

TRect CVideoView::ScreenVideoArea() const
    {
    TRect r = VideoArea();
    r.Move(PositionRelativeToScreen());
    return r;
    }

TBool CVideoView::PrepareFrameL(TInt aVideoW, TInt aVideoH)
    {
    TRect area = VideoArea();
    TInt aw = area.Width(), ah = area.Height();
    TInt tw, th;
    if (aVideoW <= 0 || aVideoH <= 0 || aw <= 0 || ah <= 0) return EFalse;
    iSrcCrop = TRect(0, 0, aVideoW, aVideoH);
    if ((iZoom == EZoomOriginal || iZoom == EZoomFit) && aVideoW <= aw && aVideoH <= ah &&
        (iZoom == EZoomOriginal || (aVideoW * 5 > aw * 4 || aVideoH * 5 > ah * 4)))
        {
        // fits: show it 1:1 (in fit mode only when stretching would gain less than 25 %)
        tw = aVideoW; th = aVideoH;
        }
    else if (iZoom == EZoomOriginal)
        {
        // 1:1 without scaling (the fastest), cut to the area; vertically at the chosen position
        TInt cx, cy;
        tw = (aVideoW < aw ? aVideoW : aw) & ~1;
        th = aVideoH < ah ? aVideoH : ah;
        cx = ((aVideoW - tw) / 2) & ~1;
        if (iCropPos == ECropTop) cy = 0;
        else if (iCropPos == ECropBottom) cy = (aVideoH - th) & ~1;
        else cy = ((aVideoH - th) / 2) & ~1;
        iSrcCrop = TRect(TPoint(cx, cy), TSize(tw, th));
        }
    else if (iZoom == EZoomFit && aVideoW * 100 <= aw * 112 && aVideoH * 100 <= ah * 112)
        {
        // at most 12 % too big: cut a few lines instead of scaling (scaling is the slow path)
        tw = aVideoW < aw ? aVideoW : aw;
        th = aVideoH < ah ? aVideoH : ah;
        iSrcCrop = TRect(TPoint(((aVideoW - tw) / 2) & ~1, ((aVideoH - th) / 2) & ~1), TSize(tw & ~1, th));
        }
    else if (iZoom == EZoomFill)
        {
        // fill the whole area, cutting off the edges of the video (SmartMovie's "crop")
        tw = aw; th = ah;
        if (aVideoW * ah > aVideoH * aw)
            {
            TInt cw = (aw * aVideoH) / ah;      // visible source width
            if (cw < 2) cw = 2;
            iSrcCrop = TRect(TPoint(((aVideoW - cw) / 2) & ~1, 0), TSize(cw, aVideoH));
            }
        else
            {
            TInt ch = (ah * aVideoW) / aw;      // visible source height
            if (ch < 2) ch = 2;
            iSrcCrop = TRect(TPoint(0, ((aVideoH - ch) / 2) & ~1), TSize(aVideoW, ch));
            }
        }
    else if (aVideoW * ah > aVideoH * aw)
        {
        tw = aw; th = (aVideoH * aw) / aVideoW;
        }
    else
        {
        th = ah; tw = (aVideoW * ah) / aVideoH;
        }
    tw &= ~1;
    if (tw < 2) tw = 2;
    if (th < 2) th = 2;
    iFramePos = TPoint(area.iTl.iX + (aw - tw) / 2, area.iTl.iY + (ah - th) / 2);
    iFramePos.iX &= ~1;     // lines start on a word: the colour conversion's fast paths write words
    iVideoW = aVideoW; iVideoH = aVideoH;
    if (iFrame->Handle() && iFrame->SizeInPixels() == TSize(tw, th)) return EFalse;
    iFrame->Reset();
    User::LeaveIfError(iFrame->Create(TSize(tw, th), iMode));
    iHaveFrame = EFalse;
    // the area around the new frame must be repainted black
    DrawNow();
    return ETrue;
    }

TUint8* CVideoView::LockFrame(TInt& aStride, TSize& aSize, TInt& aFormat)
    {
    if (!iFrame->Handle()) return NULL;
    aSize = iFrame->SizeInPixels();
    aStride = CFbsBitmap::ScanLineLength(aSize.iWidth, iMode);
    aFormat = FrameFormat();
    iFrame->LockHeap();
    return (TUint8*)iFrame->DataAddress();
    }

TInt CVideoView::FrameFormat() const
    {
    switch (iMode)
        {
        case EColor4K: return YUV_RGB444;
        case EColor16M: return YUV_BGR888;
        case EColor16MU: return YUV_XRGB8888;
        default: return YUV_RGB565;
        }
    }

// ------------------------------------------------------------------ direct drawing

void CVideoView::StartDsa()
    {
    if (!iDsa)
        {
        TRAPD(err, iDsa = CDirectScreenAccess::NewL(iCoeEnv->WsSession(), *iEikonEnv->ScreenDevice(), Window(), *this));
        if (err != KErrNone) { iDsa = NULL; iDirectWhy.Copy(_L("no direct access")); return; }
        }
    if (iDsaOn) return;
    TRAPD(err, iDsa->StartL());
    iDsaOn = (err == KErrNone);
    }

void CVideoView::AbortNow(RDirectScreenAccess::TTerminationReasons /*aReason*/)
    {
    iDsaOn = EFalse;        // something is drawn over the window: back to the window server
    }

void CVideoView::Restart(RDirectScreenAccess::TTerminationReasons /*aReason*/)
    {
    StartDsa();
    }

TRect CVideoView::FrameScreenRect() const
    {
    // Rect() of a window-owning control starts at (0, 0): window coordinates. The window itself
    // is not always at the screen's origin (on the 9300 the normal window starts at x = 92).
    TRect r(iFramePos, iFrame->SizeInPixels());
    r.Move(PositionRelativeToScreen());
    return r;
    }

// Finds the frame buffer layout: draws 2 x 8 pixels with the screen graphics context, looks for
// them in the screen memory (gives the offset and the line length), then writes 8 pixels into
// the memory and reads them back through the screen device. Only screens with 2 bytes per pixel.
TBool CVideoView::DirectSelfTest()
    {
    iScrBase = NULL;
    TPckgBuf<TScreenInfoV01> info;
    UserSvr::ScreenInfo(info);
    if (!info().iScreenAddressValid || !info().iScreenAddress) { iDirectWhy.Copy(_L("no screen address")); return EFalse; }
    CFbsScreenDevice* sd = iDsa->ScreenDevice();
    TDisplayMode m = sd->DisplayMode();
    if ((m != EColor64K && m != EColor4K) || m != iMode) { iDirectWhy.Copy(_L("screen colour mode")); return EFalse; }
    TInt W = info().iScreenSize.iWidth, H = info().iScreenSize.iHeight;
    TRect fr = FrameScreenRect();
    if (fr.Width() < 24 || fr.Height() < 8 || W <= 0 || H <= 0 || fr.iBr.iX > W || fr.iBr.iY > H)
        { iDirectWhy.Copy(_L("picture too small")); return EFalse; }

    // pure colours: the same in every 16-bit format, whatever the rounding
    const TRgb cols[8] = { TRgb(255, 0, 0), TRgb(0, 255, 0), TRgb(0, 0, 255), TRgb(0, 255, 255),
                           TRgb(255, 0, 255), TRgb(255, 255, 0), TRgb(255, 255, 255), TRgb(0, 0, 0) };
    TUint16 pat[16];
    TInt k;
    for (k = 0; k < 16; k++)
        {
        const TRgb& c = cols[k < 8 ? k : 15 - k];
        pat[k] = (TUint16)(m == EColor64K ? c.Color64K() : c.Color4K());
        }
    // the graphics context works in window coordinates
    TPoint pw(iFramePos.iX + 4, iFramePos.iY + 2);
    TPoint ps(fr.iTl.iX + 4, fr.iTl.iY + 2);
    CFbsBitGc* gc = iDsa->Gc();
    gc->SetPenStyle(CGraphicsContext::ESolidPen);
    gc->SetBrushStyle(CGraphicsContext::ENullBrush);
    gc->SetDrawMode(CGraphicsContext::EDrawModePEN);
    for (k = 0; k < 16; k++)
        {
        gc->SetPenColor(cols[k < 8 ? k : 15 - k]);
        gc->Plot(TPoint(pw.iX + (k & 7), pw.iY + (k >> 3)));
        }
    sd->Update();

    const TUint16* s = (const TUint16*)info().iScreenAddress;
    TInt n = W * H + 512 - 8;           // halfwords searched (a palette or header may come first)
    TInt i, j = -1;
    for (i = 0; i < n; i++)
        if (s[i] == pat[0] && s[i + 1] == pat[1] && s[i + 2] == pat[2] && s[i + 3] == pat[3] &&
            s[i + 4] == pat[4] && s[i + 5] == pat[5] && s[i + 6] == pat[6] && s[i + 7] == pat[7])
            break;
    if (i >= n) { iDirectWhy.Copy(_L("test pixels not found")); return EFalse; }
    for (k = i + W; k <= i + 2 * W && k < n; k++)
        if (s[k] == pat[8] && s[k + 1] == pat[9] && s[k + 2] == pat[10] && s[k + 3] == pat[11] &&
            s[k + 4] == pat[12] && s[k + 5] == pat[13] && s[k + 6] == pat[14] && s[k + 7] == pat[15])
            { j = k; break; }
    if (j < 0) { iDirectWhy.Copy(_L("line length not found")); return EFalse; }
    TInt stride = j - i;                // halfwords per line
    TInt b = i - (ps.iY * stride + ps.iX);
    if (b < 0 || b + (H - 1) * stride + W > n + 8) { iDirectWhy.Copy(_L("odd screen layout")); return EFalse; }
    iScrBase = (TUint8*)(s + b);
    iScrStride = stride * 2;

    // write through memory, read back through the screen device
    TPoint q(fr.iBr.iX - 12, fr.iBr.iY - 3);
    TUint16* d = (TUint16*)(iScrBase + q.iY * iScrStride) + q.iX;
    for (k = 0; k < 8; k++) d[k] = pat[(k + 3) & 7];
    TBuf8<32> line;
    sd->GetScanLine(line, q, 8, m);
    if (line.Length() < 16) { iDirectWhy.Copy(_L("read back failed")); return EFalse; }
    const TUint16* r = (const TUint16*)line.Ptr();
    for (k = 0; k < 8; k++)
        if (r[k] != pat[(k + 3) & 7]) { iDirectWhy.Copy(_L("read back differs")); iScrBase = NULL; return EFalse; }
    return ETrue;
    }

TUint8* CVideoView::LockDirect(TInt& aStride, TSize& aSize, TInt& aFormat)
    {
    if (!iDirectEnabled || iDirectState == EDirectFailed || !iFrame->Handle() || !IsReadyToDraw())
        return NULL;
    if (!iDsaOn && iDsaRetryTick && (User::TickCount() - iDsaRetryTick) < 128) return NULL;   // retry after ~2 s
    StartDsa();
    if (!iDsaOn) { if (iDirectState != EDirectFailed) iDirectWhy.Copy(_L("not available now")); return NULL; }
    TRect fr = FrameScreenRect();
    // only when the window server lets us have the whole picture area
    RRegion reg;
    reg.AddRect(fr);
    reg.SubRegion(*iDsa->DrawingRegion());
    TBool covered = !reg.CheckError() && reg.IsEmpty();
    reg.Close();
    if (!covered)
        {
        // tell what the window server allows (for the speed test), and stop asking for a while:
        // an active direct access may cost the window server's drawing time
        if (iDirectState != EDirectFailed)
            {
            const RRegion* dr = iDsa->DrawingRegion();
            TRect b = dr->BoundingRect();
            iDirectWhy.Format(_L("picture partly covered (picture %d,%d-%d,%d, free %d,%d-%d,%d in %d)"),
                fr.iTl.iX, fr.iTl.iY, fr.iBr.iX, fr.iBr.iY, b.iTl.iX, b.iTl.iY, b.iBr.iX, b.iBr.iY, dr->Count());
            }
        iDsa->Cancel();
        iDsaOn = EFalse;
        iDsaRetryTick = User::TickCount();
        if (!iDsaRetryTick) iDsaRetryTick = 1;
        return NULL;
        }
    if (iDirectState != EDirectOk || !iScrBase)
        {
        // (a saved "ok" only says the test passed before: the layout is found again every run)
        // remember that the test is running: if it crashes the phone app, it isn't tried again
        CMp4PlayerAppUi* ui = (CMp4PlayerAppUi*)iEikonEnv->EikAppUi();
        iDirectState = EDirectTesting;
        ui->SaveSettings();
        iDirectState = DirectSelfTest() ? EDirectOk : EDirectFailed;
        ui->SaveSettings();
        if (iDirectState != EDirectOk) return NULL;
        }
    TUint8* p = iScrBase + fr.iTl.iY * iScrStride + fr.iTl.iX * 2;
    if ((((TUint)p) | (TUint)iScrStride) & 3) { iDirectWhy.Copy(_L("picture not word aligned")); return NULL; }
    iDirectRect = fr;
    iDirectWhy.Copy(_L("on"));
    aSize = fr.Size();
    aStride = iScrStride;
    aFormat = FrameFormat();
    return p;
    }

void CVideoView::UnlockDirect()
    {
    if (iDsa && iDsaOn)
        {
        TRegionFix<1> reg(iDirectRect);
        iDsa->ScreenDevice()->Update(reg);
        }
    }

void CVideoView::UnlockFrame()
    {
    iFrame->UnlockHeap();
    iHaveFrame = ETrue;
    }

void CVideoView::ShowFrame()
    {
    if (!iHaveFrame || !IsReadyToDraw()) return;
    CWindowGc& gc = SystemGc();
    ActivateGc();
    gc.BitBlt(iFramePos, iFrame);
    DeactivateGc();
    iCoeEnv->WsSession().Flush();
    }

void CVideoView::ClearFrame()
    {
    iHaveFrame = EFalse;
    DrawNow();
    }

void CVideoView::SetMessage(const TDesC& aText)
    {
    iMessage.Copy(aText.Left(iMessage.MaxLength()));
    }

// (re)creates the off-screen panel bitmap when the panel size changes
void CVideoView::EnsurePanel()
    {
    TSize size = StatusRect().Size();
    if (iFullScreen || size.iWidth <= 0 || size.iHeight <= 0) return;
    if (iPanel && iPanel->SizeInPixels() == size) return;
    delete iPanelGc; iPanelGc = NULL;
    delete iPanelDev; iPanelDev = NULL;
    delete iPanel; iPanel = NULL;
    CFbsBitmap* bmp = new CFbsBitmap;
    if (!bmp) return;
    if (bmp->Create(size, iMode) != KErrNone) { delete bmp; return; }
    CFbsBitmapDevice* dev = NULL;
    TRAPD(err, dev = CFbsBitmapDevice::NewL(bmp));
    if (err != KErrNone) { delete bmp; return; }
    CFbsBitGc* gc = NULL;
    if (dev->CreateContext(gc) != KErrNone) { delete dev; delete bmp; return; }
    iPanel = bmp; iPanelDev = dev; iPanelGc = gc;
    iStatusSig = 0;
    }

// a number that changes whenever something shown in the panel changes
TUint CVideoView::StatusSignature() const
    {
    TUint h = 2166136261u;
    TInt i;
    h = (h ^ (TUint)iMessage.Length()) * 16777619u;
    for (i = 0; i < iMessage.Length(); i++) h = (h ^ iMessage[i]) * 16777619u;
    if (iEngine)
        {
        TInt dur = iEngine->DurationMs();
        TInt pos = iEngine->PositionMs();
        h = (h ^ (TUint)iEngine->State()) * 16777619u;
        h = (h ^ (TUint)(pos / 1000)) * 16777619u;
        h = (h ^ (TUint)(dur / 1000)) * 16777619u;
        h = (h ^ (TUint)(dur > 0 ? (TInt)(((TInt64)pos * 128 / dur).GetTInt()) : 0)) * 16777619u;
        h = (h ^ (TUint)(iEngine->Volume() + 16 * iEngine->HasAudio() + 64 * iEngine->Mode())) * 16777619u;
        h = (h ^ (TUint)iEngine->FileName().Length()) * 16777619u;
        }
    return h | 1;
    }

void CVideoView::BlitStatus(CWindowGc& aGc) const
    {
    TRect r = StatusRect();
    if (iPanelGc && iPanel->SizeInPixels() == r.Size())
        {
        DrawStatus(*iPanelGc, TRect(r.Size()));
        aGc.BitBlt(r.iTl, iPanel);
        }
    else DrawStatus(aGc, r);
    iStatusSig = StatusSignature();
    }

void CVideoView::DrawStatusNow()
    {
    if (iFullScreen || !IsReadyToDraw()) return;
    if (StatusSignature() == iStatusSig) return;        // nothing visible changed
    CWindowGc& gc = SystemGc();
    ActivateGc();
    BlitStatus(gc);
    DeactivateGc();
    }

void CVideoView::FormatTime(TDes& aBuf, TInt aMs) const
    {
    TInt s = aMs / 1000;
    if (s < 0) s = 0;
    if (s >= 3600) aBuf.AppendFormat(_L("%d:%02d:%02d"), s / 3600, (s / 60) % 60, s % 60);
    else aBuf.AppendFormat(_L("%d:%02d"), s / 60, s % 60);
    }

// draws aText word-wrapped into lines of width aW from aY downwards (while above aMaxY)
static void DrawWrapped(CGraphicsContext& aGc, const CFont* aFont, const TDesC& aText, TInt aX, TInt& aY,
                        TInt aW, TInt aMaxY, TBool aCentre)
    {
    TPtrC rest(aText);
    TInt lh = aFont->HeightInPixels() + 3;
    while (rest.Length() > 0 && aY + aFont->DescentInPixels() <= aMaxY)
        {
        TInt n = aFont->TextCount(rest, aW);
        if (n <= 0) n = 1;
        if (n < rest.Length())
            {
            TInt sp = rest.Left(n + 1).LocateReverse(' ');
            if (sp > 0) n = sp + 1;
            }
        TPtrC line(rest.Left(n));
        while (line.Length() > 0 && line[line.Length() - 1] == ' ') line.Set(line.Left(line.Length() - 1));
        TInt x = aX;
        if (aCentre) x += (aW - aFont->TextWidthInPixels(line)) / 2;
        aGc.DrawText(line, TPoint(x, aY));
        aY += lh;
        rest.Set(rest.Mid(n));
        }
    }

void CVideoView::DrawStatus(CGraphicsContext& aGc, const TRect& r) const
    {
    if (r.Width() <= 0) return;
    const CFont* font = iEikonEnv->DenseFont();
    TInt lh = font->HeightInPixels() + 3;
    TInt x = r.iTl.iX + 6;
    TInt w = r.Width() - 10;
    TInt y = r.iTl.iY + KStatusPad + font->AscentInPixels() + 2;
    TBuf<160> t;
    aGc.SetPenStyle(CGraphicsContext::ENullPen);
    aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    aGc.SetBrushColor(TRgb(0x20, 0x20, 0x28));
    aGc.DrawRect(r);
    aGc.SetPenStyle(CGraphicsContext::ESolidPen);
    aGc.SetPenColor(TRgb(0x50, 0x50, 0x60));
    aGc.DrawLine(r.iTl, TPoint(r.iTl.iX, r.iBr.iY));
    aGc.UseFont(font);
    aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
    aGc.SetPenColor(KRgbWhite);

    if (!iEngine || iEngine->State() == CPlayerEngine::EIdle)
        {
        if (iMessage.Length()) aGc.SetPenColor(TRgb(0xFF, 0xD0, 0x60));
        DrawWrapped(aGc, font, iMessage.Length() ? (const TDesC&)iMessage : KNoFileText(), x, y, w, r.iBr.iY - 2, EFalse);
        aGc.DiscardFont();
        return;
        }
    switch (iEngine->State())
        {
        case CPlayerEngine::EOpening: t.Copy(_L("Opening")); break;
        case CPlayerEngine::EPlaying: t.Copy(_L("Playing")); break;
        case CPlayerEngine::EPaused: t.Copy(_L("Paused")); break;
        case CPlayerEngine::EStopped: t.Copy(_L("Stopped")); break;
        case CPlayerEngine::EEnded: t.Copy(_L("End")); break;
        default: break;
        }
    aGc.DrawText(t, TPoint(x, y));
    // volume on the same line, right-aligned
    t.Zero();
    if (iEngine->HasAudio() || iEngine->Mode() == CPlayerEngine::EModeSystem) t.Format(_L("Vol %d"), iEngine->Volume());
    else t.Copy(_L("No audio"));
    aGc.SetPenColor(TRgb(0xB0, 0xB0, 0xC0));
    aGc.DrawText(t, TPoint(r.iBr.iX - 5 - font->TextWidthInPixels(t), y));
    y += lh;

    aGc.SetPenColor(KRgbWhite);
    t.Zero();
    FormatTime(t, iEngine->PositionMs());
    t.Append(_L(" / "));
    FormatTime(t, iEngine->DurationMs());
    aGc.DrawText(t, TPoint(x, y));
    y += 5;

    // progress bar
    {
    TRect bar(x, y, x + w, y + 8);
    TInt dur = iEngine->DurationMs();
    TInt pos = iEngine->PositionMs();
    TInt fill = 0;
    if (dur > 0)
        {
        if (pos > dur) pos = dur;
        fill = (TInt)(((TInt64)pos * (bar.Width() - 2) / dur).GetTInt());
        }
    aGc.SetPenColor(TRgb(0x90, 0x90, 0xA0));
    aGc.DrawRect(bar);
    if (fill > 0)
        {
        aGc.SetPenStyle(CGraphicsContext::ENullPen);
        aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        aGc.SetBrushColor(TRgb(0x40, 0xA0, 0xFF));
        aGc.DrawRect(TRect(bar.iTl.iX + 1, bar.iTl.iY + 1, bar.iTl.iX + 1 + fill, bar.iBr.iY - 1));
        aGc.SetPenStyle(CGraphicsContext::ESolidPen);
        aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
        }
    y = bar.iBr.iY + 6 + font->AscentInPixels();
    }

    // message (yellow) or the file name, wrapped over the rest of the panel
    if (iMessage.Length())
        {
        aGc.SetPenColor(TRgb(0xFF, 0xD0, 0x60));
        DrawWrapped(aGc, font, iMessage, x, y, w, r.iBr.iY - 2, EFalse);
        }
    else
        {
        aGc.SetPenColor(TRgb(0xB0, 0xB0, 0xC0));
        TParsePtrC p(iEngine->FileName());
        DrawWrapped(aGc, font, p.NameAndExt(), x, y, w, r.iBr.iY - 2, EFalse);
        }
    aGc.DiscardFont();
    }

void CVideoView::DrawHelp(CGraphicsContext& aGc, const TRect& aRect) const
    {
    const CFont* font = iEikonEnv->NormalFont();
    const CFont* dense = iEikonEnv->DenseFont();
    TInt y = aRect.iTl.iY + aRect.Height() / 2 - 2 * (dense->HeightInPixels() + 3) - 6;
    TInt w = aRect.Width() - 16;
    aGc.SetPenColor(KRgbWhite);
    aGc.UseFont(font);
    DrawWrapped(aGc, font, _L("MP4 Player 9300"), aRect.iTl.iX + 8, y, w, aRect.iBr.iY, ETrue);
    aGc.DiscardFont();
    y += 4;
    aGc.UseFont(dense);
    aGc.SetPenColor(TRgb(0xC0, 0xC0, 0xD0));
    DrawWrapped(aGc, dense, _L("Press Open (Ctrl+O) to choose a video: MP4, 3GP or MOV with MPEG-4 Part 2, H.263 or H.264 Baseline video and AAC or AMR sound."),
                aRect.iTl.iX + 8, y, w, aRect.iBr.iY, ETrue);
    y += 4;
    DrawWrapped(aGc, dense, _L("Space play/pause, Left/Right seek, Up/Down volume, F full screen, Z zoom, I info"),
                aRect.iTl.iX + 8, y, w, aRect.iBr.iY, ETrue);
    aGc.DiscardFont();
    }

void CVideoView::Draw(const TRect& /*aRect*/) const
    {
    CWindowGc& gc = SystemGc();
    TRect area = VideoArea();
    gc.SetPenStyle(CGraphicsContext::ENullPen);
    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    gc.SetBrushColor(KRgbBlack);
    if (iHaveFrame)
        {
        TRect fr(iFramePos, iFrame->SizeInPixels());
        // black borders around the frame
        gc.DrawRect(TRect(area.iTl.iX, area.iTl.iY, area.iBr.iX, fr.iTl.iY));
        gc.DrawRect(TRect(area.iTl.iX, fr.iBr.iY, area.iBr.iX, area.iBr.iY));
        gc.DrawRect(TRect(area.iTl.iX, fr.iTl.iY, fr.iTl.iX, fr.iBr.iY));
        gc.DrawRect(TRect(fr.iBr.iX, fr.iTl.iY, area.iBr.iX, fr.iBr.iY));
        gc.BitBlt(iFramePos, iFrame);
        }
    else
        {
        gc.DrawRect(area);
        if (!iEngine || iEngine->State() == CPlayerEngine::EIdle) DrawHelp(gc, area);
        else if (iEngine->Mode() == CPlayerEngine::EModeAudioOnly)
            {
            const CFont* font = iEikonEnv->NormalFont();
            gc.UseFont(font);
            gc.SetBrushStyle(CGraphicsContext::ENullBrush);
            gc.SetPenStyle(CGraphicsContext::ESolidPen);
            gc.SetPenColor(TRgb(0xC0, 0xC0, 0xD0));
            TParsePtrC p(iEngine->FileName());
            gc.DrawText(p.NameAndExt(), area, area.Height() / 2, CGraphicsContext::ECenter);
            gc.DiscardFont();
            }
        }
    gc.SetPenStyle(CGraphicsContext::ESolidPen);
    if (!iFullScreen) BlitStatus(gc);
    }

TKeyResponse CVideoView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
    {
    if (aType != EEventKey) return EKeyWasNotConsumed;
    CEikAppUi* ui = iEikonEnv->EikAppUi();
    TBool shift = (aKeyEvent.iModifiers & EModifierShift) != 0;
    TInt cmd = 0;
    if (aKeyEvent.iModifiers & EModifierCtrl) return EKeyWasNotConsumed;   // hotkeys
    switch (aKeyEvent.iCode)
        {
        case ' ':
        case EKeyEnter:
        case EKeyDevice3:
            cmd = ECmdPlayPause; break;
        case EKeyLeftArrow: cmd = shift ? ECmdSeekBackLong : ECmdSeekBack; break;
        case EKeyRightArrow: cmd = shift ? ECmdSeekFwdLong : ECmdSeekFwd; break;
        case EKeyUpArrow: cmd = ECmdVolUp; break;
        case EKeyDownArrow: cmd = ECmdVolDown; break;
        case 'f': case 'F': cmd = ECmdFullScreen; break;
        case 'z': case 'Z': cmd = ECmdZoom; break;
        case 'i': case 'I': cmd = ECmdFileInfo; break;
        case 'b': case 'B': cmd = ECmdBenchmark; break;
        case 'c': case 'C': cmd = ECmdCropPos; break;
        case 'q': case 'Q': cmd = ECmdFastH264; break;
        case 'd': case 'D': cmd = ECmdDirect; break;
        case 'm': case 'M': cmd = ECmdSound; break;
        case 'k': case 'K': cmd = ECmdBehind; break;
        case 's': case 'S': cmd = ECmdStop; break;
        case 'o': case 'O': cmd = ECmdOpen; break;
        case EKeyEscape:
            cmd = iFullScreen ? ECmdFullScreen : ECmdStop; break;
        default:
            return EKeyWasNotConsumed;
        }
    ui->HandleCommandL(cmd);
    return EKeyWasConsumed;
    }
