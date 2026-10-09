#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wincodec.h>
#include <gdiplus.h>
#include <initguid.h>
#include <qedit.h>

DEFINE_GUID(CLSID_NullRenderer2, 0xC1F400A4, 0x3F08, 0x11D3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")

DEFINE_GUID(CLSID_GpJpeg2, 0x557CF401,0x1A04,0x11D3,0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E);

static void dumpPixels(const wchar_t* tag, const BYTE* d) {
    printf("[%ls] first 4 px bytes(B,G,R):\n", tag);
    for (int i = 0; i < 4; i++) { const BYTE* p = d + i * 3; printf("  px%d: %u %u %u\n", i, p[0], p[1], p[2]); }
}

static bool wicDecodeCbCr(const std::vector<BYTE>& jpeg, bool swapUV, const wchar_t* outPath, const wchar_t* tag) {
    IWICImagingFactory* wf = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wf);
    IWICStream* stm = nullptr; wf->CreateStream(&stm);
    stm->InitializeFromMemory((BYTE*)jpeg.data(), (UINT)jpeg.size());
    IWICBitmapDecoder* dec = nullptr;
    wf->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    IWICBitmapFrameDecode* fr = nullptr; dec->GetFrame(0, &fr);
    UINT w = 0, h = 0; fr->GetSize(&w, &h);
    IWICFormatConverter* conv = nullptr; wf->CreateFormatConverter(&conv);
    conv->Initialize(fr, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT stride = (w * 3 + 3) & ~3u;
    std::vector<BYTE> bgr((size_t)stride * h);
    conv->CopyPixels(nullptr, stride, (UINT)bgr.size(), bgr.data());
    if (swapUV) for (size_t p = 0; p + 2 < bgr.size(); p += 3) { BYTE t = bgr[p]; bgr[p] = bgr[p+2]; bgr[p+2] = t; }
    if (tag && wcscmp(tag, L"wb") == 0) {
        double sB = 0, sG = 0, sR = 0; size_t n = 0;
        for (UINT y = 0; y < h; y++) { const BYTE* row = bgr.data() + (size_t)y*stride;
            for (UINT x = 0; x < w; x++) { sB += row[x*3]; sG += row[x*3+1]; sR += row[x*3+2]; n++; } }
        double aB = sB/n, aG = sG/n, aR = sR/n, gray = (aB+aG+aR)/3.0;
        printf("[wb] mean B=%.1f G=%.1f R=%.1f -> gains B=%.3f G=%.3f R=%.3f\n", aB, aG, aR, gray/aB, gray/aG, gray/aR);
        for (UINT y = 0; y < h; y++) { BYTE* row = bgr.data() + (size_t)y*stride;
            for (UINT x = 0; x < w; x++) {
                int b=(int)(row[x*3]*gray/aB+0.5), g=(int)(row[x*3+1]*gray/aG+0.5), r=(int)(row[x*3+2]*gray/aR+0.5);
                row[x*3]=(BYTE)(b<0?0:(b>255?255:b)); row[x*3+1]=(BYTE)(g<0?0:(g>255?255:g)); row[x*3+2]=(BYTE)(r<0?0:(r>255?255:r)); } }
    }
    printf("[%ls] %ux%u%s\n", tag, w, h, swapUV ? " (R/B swapped)" : "");
    dumpPixels(tag, bgr.data());
    HANDLE hf = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD rowBytes = (w * 3 + 3) & ~3u, dataSize = rowBytes * h, offBits = 54;
    BYTE hdr[54]; memset(hdr, 0, 54);
    hdr[0]='B'; hdr[1]='M'; DWORD fs=offBits+dataSize; memcpy(hdr+2,&fs,4); DWORD of=offBits; memcpy(hdr+10,&of,4);
    DWORD hs=40; memcpy(hdr+14,&hs,4); LONG wl=w, hl=-(LONG)h; memcpy(hdr+18,&wl,4); memcpy(hdr+22,&hl,4);
    WORD pl=1, bp=24; memcpy(hdr+26,&pl,2); memcpy(hdr+28,&bp,2); memcpy(hdr+34,&dataSize,4);
    DWORD n; WriteFile(hf, hdr, 54, &n, nullptr);
    for (UINT y = 0; y < h; y++) WriteFile(hf, bgr.data() + (size_t)y*stride, w*3, &n, nullptr);
    CloseHandle(hf);
    conv->Release(); fr->Release(); dec->Release(); stm->Release(); wf->Release();
    return true;
}

static bool wicDecodeWB(const std::vector<BYTE>& jpeg, const wchar_t* outPath, const wchar_t* tag) {
    IWICImagingFactory* wf = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wf);
    IWICStream* stm = nullptr; wf->CreateStream(&stm);
    stm->InitializeFromMemory((BYTE*)jpeg.data(), (UINT)jpeg.size());
    IWICBitmapDecoder* dec = nullptr;
    wf->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    IWICBitmapFrameDecode* fr = nullptr; dec->GetFrame(0, &fr);
    UINT w = 0, h = 0; fr->GetSize(&w, &h);
    IWICFormatConverter* conv = nullptr; wf->CreateFormatConverter(&conv);
    conv->Initialize(fr, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT stride = (w * 3 + 3) & ~3u;
    std::vector<BYTE> bgr((size_t)stride * h);
    conv->CopyPixels(nullptr, stride, (UINT)bgr.size(), bgr.data());
    double sB = 0, sG = 0, sR = 0; size_t n = 0;
    for (UINT y = 0; y < h; y++) { const BYTE* r = bgr.data() + (size_t)y*stride; for (UINT x = 0; x < w; x++) { sB += r[x*3]; sG += r[x*3+1]; sR += r[x*3+2]; n++; } }
    double aB = sB/n, aG = sG/n, aR = sR/n, gray = (aB+aG+aR)/3.0;
    printf("[%ls] pre-avg B=%.1f G=%.1f R=%.1f\n", tag, aB, aG, aR);
    double gB = gray/aB, gG = gray/aG, gR = gray/aR;
    for (UINT y = 0; y < h; y++) { BYTE* r = bgr.data() + (size_t)y*stride; for (UINT x = 0; x < w; x++) {
        int b=(int)(r[x*3]*gB+0.5), g=(int)(r[x*3+1]*gG+0.5), rr=(int)(r[x*3+2]*gR+0.5);
        r[x*3]=(BYTE)(b<0?0:(b>255?255:b)); r[x*3+1]=(BYTE)(g<0?0:(g>255?255:g)); r[x*3+2]=(BYTE)(rr<0?0:(rr>255?255:rr)); } }
    printf("[%ls] %ux%u (gray-world WB)\n", tag, w, h);
    dumpPixels(tag, bgr.data());
    HANDLE hf = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD rowBytes = (w * 3 + 3) & ~3u, dataSize = rowBytes * h, offBits = 54;
    BYTE hdr[54]; memset(hdr, 0, 54);
    hdr[0]='B'; hdr[1]='M'; DWORD fs=offBits+dataSize; memcpy(hdr+2,&fs,4); DWORD of=offBits; memcpy(hdr+10,&of,4);
    DWORD hs=40; memcpy(hdr+14,&hs,4); LONG wl=w, hl=-(LONG)h; memcpy(hdr+18,&wl,4); memcpy(hdr+22,&hl,4);
    WORD pl=1, bp=24; memcpy(hdr+26,&pl,2); memcpy(hdr+28,&bp,2); memcpy(hdr+34,&dataSize,4);
    DWORD nw; WriteFile(hf, hdr, 54, &nw, nullptr);
    for (UINT y = 0; y < h; y++) WriteFile(hf, bgr.data() + (size_t)y*stride, w*3, &nw, nullptr);
    CloseHandle(hf);
    conv->Release(); fr->Release(); dec->Release(); stm->Release(); wf->Release();
    return true;
}

static bool wicDecode(const std::vector<BYTE>& jpeg, bool swap, const wchar_t* outPath, const wchar_t* tag) {
    IWICImagingFactory* wf = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wf);
    IWICStream* stm = nullptr; wf->CreateStream(&stm);
    stm->InitializeFromMemory((BYTE*)jpeg.data(), (UINT)jpeg.size());
    IWICBitmapDecoder* dec = nullptr;
    wf->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    IWICBitmapFrameDecode* fr = nullptr; dec->GetFrame(0, &fr);
    UINT w = 0, h = 0; fr->GetSize(&w, &h);
    IWICFormatConverter* conv = nullptr; wf->CreateFormatConverter(&conv);
    conv->Initialize(fr, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT stride = (w * 3 + 3) & ~3u;
    std::vector<BYTE> bgr((size_t)stride * h);
    conv->CopyPixels(nullptr, stride, (UINT)bgr.size(), bgr.data());
    if (swap) for (size_t p = 0; p + 2 < bgr.size(); p += 3) { BYTE t = bgr[p]; bgr[p] = bgr[p+2]; bgr[p+2] = t; }
    printf("[%ls] WIC 24bppBGR %ux%u%s\n", tag, w, h, swap ? " (R/B swapped)" : "");
    dumpPixels(tag, bgr.data());
    HANDLE hf = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD rowBytes = (w * 3 + 3) & ~3u, dataSize = rowBytes * h, offBits = 54;
    BYTE hdr[54]; memset(hdr, 0, 54);
    hdr[0]='B'; hdr[1]='M'; DWORD fs=offBits+dataSize; memcpy(hdr+2,&fs,4); DWORD of=offBits; memcpy(hdr+10,&of,4);
    DWORD hs=40; memcpy(hdr+14,&hs,4); LONG wl=w, hl=-(LONG)h; memcpy(hdr+18,&wl,4); memcpy(hdr+22,&hl,4);
    WORD pl=1, bp=24; memcpy(hdr+26,&pl,2); memcpy(hdr+28,&bp,2); memcpy(hdr+34,&dataSize,4);
    DWORD n; WriteFile(hf, hdr, 54, &n, nullptr);
    for (UINT y = 0; y < h; y++) WriteFile(hf, bgr.data() + (size_t)y*stride, w*3, &n, nullptr);
    CloseHandle(hf);
    conv->Release(); fr->Release(); dec->Release(); stm->Release(); wf->Release();
    return true;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ICreateDevEnum* de = nullptr;
    CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&de);
    IEnumMoniker* em = nullptr; de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0);
    if (!em) { printf("no cam\n"); return 1; }
    IMoniker* mon = nullptr; em->Next(1, &mon, nullptr);
    if (!mon) { printf("no moniker\n"); return 1; }
    IGraphBuilder* g = nullptr; ICaptureGraphBuilder2* cb = nullptr; IBaseFilter* cap = nullptr;
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
    CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&cb);
    cb->SetFiltergraph(g);
    mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&cap);
    g->AddFilter(cap, L"cap");
    IAMStreamConfig* sc = nullptr;
    cb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, cap, IID_IAMStreamConfig, (void**)&sc);
    int n = 0, sz = 0; sc->GetNumberOfCapabilities(&n, &sz);
    AM_MEDIA_TYPE* mt = nullptr;
    for (int i = 0; i < n; i++) {
        VIDEO_STREAM_CONFIG_CAPS vsc; AM_MEDIA_TYPE* m = nullptr;
        if (FAILED(sc->GetStreamCaps(i, &m, (BYTE*)&vsc))) continue;
        if (m->subtype == MEDIASUBTYPE_MJPG && vsc.InputSize.cx == 8192) { mt = m; break; }
        if (m->pbFormat) CoTaskMemFree(m->pbFormat); CoTaskMemFree(m);
    }
    if (!mt) { printf("no 8192 MJPG\n"); return 1; }
    sc->SetFormat(mt);
    ISampleGrabber* grab = nullptr;
    CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_ISampleGrabber, (void**)&grab);
    IBaseFilter* gf = nullptr; grab->QueryInterface(IID_IBaseFilter, (void**)&gf);
    g->AddFilter(gf, L"grab");
    grab->SetBufferSamples(TRUE);
    cb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, cap, gf, nullptr);
    IMediaControl* ctrl = nullptr; g->QueryInterface(IID_IMediaControl, (void**)&ctrl);
    ctrl->Run();
    long bufSize = 0; std::vector<BYTE> frame; bool have = false;
    for (int t = 0; t < 200; t++) {
        if (grab->GetCurrentBuffer(&bufSize, nullptr) == S_OK && bufSize > 0) {
            frame.resize(bufSize); long s = bufSize;
            if (SUCCEEDED(grab->GetCurrentBuffer(&s, (LONG*)frame.data())) && s > 0) { frame.resize(s); have = true; break; }
        }
        Sleep(50);
    }
    ctrl->Stop();
    if (!have) { printf("no frame\n"); return 1; }
    printf("got MJPG frame %zu bytes, magic=%02X%02X\n", frame.size(), frame[0], frame[1]);
    HANDLE hf = CreateFileW(L"C:\\Users\\seewo\\Documents\\raw_frame.jpg", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD nw; WriteFile(hf, frame.data(), (DWORD)frame.size(), &nw, nullptr); CloseHandle(hf);
    wicDecode(frame, false, L"C:\\Users\\seewo\\Documents\\dec_asis.bmp", L"asis");
    wicDecode(frame, true,  L"C:\\Users\\seewo\\Documents\\dec_swap.bmp", L"swap");
    wicDecode(frame, false, L"C:\\Users\\seewo\\Documents\\dec_wb.bmp", L"wb");
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR tok; Gdiplus::GdiplusStartup(&tok, &gi, nullptr);
    {
        IStream* is = nullptr; CreateStreamOnHGlobal(nullptr, TRUE, &is);
        is->Write(frame.data(), (ULONG)frame.size(), nullptr);
        LARGE_INTEGER li = {}; is->Seek(li, STREAM_SEEK_SET, nullptr);
        Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromStream(is);
        if (bm && bm->GetLastStatus() == Gdiplus::Ok) {
            printf("[gdi+] %ux%u\n", bm->GetWidth(), bm->GetHeight());
            Gdiplus::Color c;
            for (int i = 0; i < 4; i++) { bm->GetPixel(100 + i, 100, &c); printf("  gdi+px%d R=%u G=%u B=%u\n", i, c.GetR(), c.GetG(), c.GetB()); }
            CLSID clsid; CLSIDFromString(L"{557CF401-1A04-11D3-9A73-0000F81EF32E}", &clsid);
            bm->Save(L"C:\\Users\\seewo\\Documents\\dec_gdiplus.bmp", &clsid, nullptr);
        } else printf("[gdi+] decode failed\n");
        if (bm) delete bm;
        is->Release();
    }
    Gdiplus::GdiplusShutdown(tok);
    printf("done\n");
    return 0;
}
