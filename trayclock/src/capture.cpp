// capture.cpp — 摄像头 DirectShow 抓帧（最大分辨率，YUY2/RGB24/MJPG → cam.bmp）
//             + 虚拟屏截屏（JPEG q80 → uhd.jpg，节省空间；摄像头不压缩）
#include "trayclock.h"

#include <initguid.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <qedit.h>
#include <wincodec.h>
#include <ocidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <functional>
#include <stdio.h>
#include <string.h>

// qedit.idl 未导出 NullRenderer 的 CLSID（部分 mingw 版本），手动定义
DEFINE_GUID(CLSID_NullRenderer, 0xC1F400A4, 0x3F08, 0x11D3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
// GDI+ 内置 JPEG 编码器与质量参数（GDI+ 自带编解码表，不查 WIC 注册表）
DEFINE_GUID(GpClsidJpeg, 0x557CF401, 0x1A04, 0x11D3, 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E);
DEFINE_GUID(GpPrmQuality, 0x1D5BE4B5, 0xFA4A, 0x452D, 0x9C, 0xDD, 0x5D, 0xB3, 0x51, 0x05, 0xE7, 0xEB);

static const DWORD FOURCC_MJPG = 0x47504A4D;   // 'MJPG'
static const float kJpegQuality = 0.80f;       // 屏幕截图 JPEG 质量（0-1）

// ---------------- 小型 COM 智能指针 ----------------
template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

static void FreeMt(AM_MEDIA_TYPE& mt)
{
    if (mt.pUnk) mt.pUnk->Release();
    if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    memset(&mt, 0, sizeof(mt));
}

// ---------------- YUY2 → RGB24（BGR 顺序） ----------------
static void Yuy2ToBgr24(const BYTE* src, BYTE* dst, int w, int h)
{
    for (int i = 0; i < h; i++) {
        const BYTE* s = src + (size_t)i * (size_t)w * 2;
        BYTE* d = dst + (size_t)i * (size_t)w * 3;
        for (int x = 0; x + 1 < w; x += 2) {
            int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            s += 4;
            int c0 = y0 - 16, c1 = y1 - 16, dd = u - 128, e = v - 128;
            auto clip = [](int t) { return (BYTE)(t < 0 ? 0 : (t > 255 ? 255 : t)); };
            // BT.601：R 用 V(e)，B 用 U(dd)；BMP 存 B,G,R
            d[0] = clip((298 * c0 + 516 * dd + 128) >> 8);   // B
            d[1] = clip((298 * c0 - 100 * dd - 208 * e + 128) >> 8); // G
            d[2] = clip((298 * c0 + 409 * e + 128) >> 8);    // R
            d[3] = clip((298 * c1 + 516 * dd + 128) >> 8);   // B
            d[4] = clip((298 * c1 - 100 * dd - 208 * e + 128) >> 8); // G
            d[5] = clip((298 * c1 + 409 * e + 128) >> 8);    // R
            d += 6;
        }
    }
}

// ---------------- 设备枚举 ----------------
std::vector<CamInfo> EnumCameras()
{
    std::vector<CamInfo> out;
    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum, (void**)devEnum.put())))
        return out;
    ComPtr<IEnumMoniker> enumM;
    if (FAILED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, enumM.put(), 0))
        || !enumM.p)
        return out;

    IMoniker* mon = nullptr;
    while (enumM->Next(1, &mon, nullptr) == S_OK) {
        CamInfo ci;
        ComPtr<IPropertyBag> pb;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)pb.put()))) {
            VARIANT var;
            VariantInit(&var);
            if (SUCCEEDED(pb->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal)
                ci.name = var.bstrVal;
            VariantClear(&var);
        }
        LPOLESTR disp = nullptr;
        if (SUCCEEDED(mon->GetDisplayName(nullptr, nullptr, &disp)) && disp) {
            ci.moniker = disp;
            CoTaskMemFree(disp);
        }
        if (!ci.name.empty()) out.push_back(ci);
        mon->Release();
    }
    return out;
}

// ---------------- media type 辅助 ----------------
static void FillFromVih(const AM_MEDIA_TYPE& mt, int* w, int* h, int* comp, int* bitcount)
{
    *w = *h = 0; *comp = 0; *bitcount = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER* v = (VIDEOINFOHEADER*)mt.pbFormat;
        *w = v->bmiHeader.biWidth;
        *h = abs(v->bmiHeader.biHeight);
        *comp = (int)v->bmiHeader.biCompression;
        *bitcount = v->bmiHeader.biBitCount;
    } else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        VIDEOINFOHEADER2* v = (VIDEOINFOHEADER2*)mt.pbFormat;
        *w = v->bmiHeader.biWidth;
        *h = abs(v->bmiHeader.biHeight);
        *comp = (int)v->bmiHeader.biCompression;
        *bitcount = v->bmiHeader.biBitCount;
    }
}

enum { K_YUY2 = 0, K_RGB24 = 1, K_MJPG = 2 };

struct FmtCand {
    AM_MEDIA_TYPE mt;      // 拥有 pbFormat
    int w = 0, h = 0;
    int kind = K_YUY2;     // 解码路径：0=YUY2 1=RGB24 2=MJPG(GDI+ 解码)
    int rank = 0;          // 同分辨率时的优先级 3=RGB24 2=YUY2 1=MJPG
    LONGLONG area = 0;
};

// 读取设备支持的格式（用临时 graph 拿到 IAMStreamConfig）
static bool CollectCands(IMoniker* mon, std::vector<FmtCand>& out, std::wstring* friendlyOut)
{
    ComPtr<IGraphBuilder> graph;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IGraphBuilder, (void**)graph.put())))
        return false;
    ComPtr<IBaseFilter> src;
    if (FAILED(mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)src.put())))
        return false;
    if (FAILED(graph->AddFilter(src, L"src"))) return false;

    if (friendlyOut) {
        ComPtr<IPropertyBag> pb;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)pb.put()))) {
            VARIANT var; VariantInit(&var);
            if (SUCCEEDED(pb->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal)
                *friendlyOut = var.bstrVal;
            VariantClear(&var);
        }
    }

    ComPtr<IAMStreamConfig> cfg;
    const GUID* cats[2] = { &PIN_CATEGORY_CAPTURE, &PIN_CATEGORY_PREVIEW };
    for (int i = 0; i < 2 && !cfg.p; i++) {
        ComPtr<ICaptureGraphBuilder2> b2;
        if (SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ICaptureGraphBuilder2, (void**)b2.put())) && b2.p) {
            b2->SetFiltergraph(graph);
            b2->FindInterface(cats[i], &MEDIATYPE_Video, src, IID_IAMStreamConfig, (void**)cfg.put());
        }
    }
    // 直接从引脚 QI 兜底
    if (!cfg.p) {
        ComPtr<IEnumPins> ep;
        if (SUCCEEDED(src->EnumPins(ep.put())) && ep.p) {
            IPin* pin = nullptr;
            while (cfg.p == nullptr && ep->Next(1, &pin, nullptr) == S_OK) {
                pin->QueryInterface(IID_IAMStreamConfig, (void**)cfg.put());
                pin->Release();
            }
        }
    }
    if (!cfg.p) return false;

    int nCaps = 0, size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&nCaps, &size)) || nCaps <= 0) return false;

    for (int i = 0; i < nCaps; i++) {
        AM_MEDIA_TYPE* pmt = nullptr;
        std::vector<BYTE> capBuf((size_t)size, 0);
        if (FAILED(cfg->GetStreamCaps(i, &pmt, capBuf.data())) || !pmt) continue;
        AM_MEDIA_TYPE& mt = *pmt;

        int w = 0, h = 0, comp = 0, bc = 0;
        FillFromVih(mt, &w, &h, &comp, &bc);

        // 逐条记录驱动能力（含被跳过的 H264 等），便于排查分辨率问题
        int kind = -1, rank = 0;
        if (w > 0 && h > 0) {
            if (comp == 0 && bc == 24) { kind = K_RGB24; rank = 3; }
            else if (mt.subtype == MEDIASUBTYPE_YUY2) { kind = K_YUY2; rank = 2; }
            else if (mt.subtype == MEDIASUBTYPE_MJPG || comp == FOURCC_MJPG) { kind = K_MJPG; rank = 1; }
        }
        LogF(L"cam: cap#%d %dx%d bc=%d comp=0x%08lX kind=%d%ls",
             i, w, h, bc, (unsigned long)comp, kind,
             kind >= 0 ? L" 用" : L" 跳过");
        if (kind < 0) { FreeMt(mt); CoTaskMemFree(pmt); continue; }

        FmtCand c;
        c.mt = mt;                              // 接管 pbFormat
        CoTaskMemFree(pmt);                     // 释放外层结构
        c.w = w; c.h = h;
        c.kind = kind;
        c.rank = rank;
        c.area = (LONGLONG)w * h;
        out.push_back(c);
    }
    return !out.empty();
}

// 由选定 media type 构造 SampleGrabber 期望的 FORMAT_VideoInfo 类型
static bool BuildGrabberMt(const FmtCand& c, AM_MEDIA_TYPE& out)
{
    int w = 0, h = 0, comp = 0, bc = 0;
    FillFromVih(c.mt, &w, &h, &comp, &bc);
    if (w <= 0 || h <= 0) return false;

    VIDEOINFOHEADER v;
    memset(&v, 0, sizeof(v));
    bool fromCaps = (c.kind == K_MJPG && c.mt.formattype == FORMAT_VideoInfo &&
                     c.mt.cbFormat >= sizeof(VIDEOINFOHEADER));
    if (fromCaps) {
        // MJPG：直接复制驱动 caps 的 VI 条目，逐位一致提高协商成功率
        memcpy(&v, c.mt.pbFormat, sizeof(v));
    } else {
        v.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        v.bmiHeader.biWidth = w;
        v.bmiHeader.biHeight = h;             // 正值 = bottom-up
        v.bmiHeader.biPlanes = 1;
        v.bmiHeader.biBitCount = 24;
        if (c.kind == K_MJPG) {
            v.bmiHeader.biCompression = FOURCC_MJPG;
            // 保留驱动声明的最大压缩帧大小/帧率
            if (c.mt.formattype == FORMAT_VideoInfo2 && c.mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                VIDEOINFOHEADER2* o = (VIDEOINFOHEADER2*)c.mt.pbFormat;
                v.bmiHeader.biSizeImage = o->bmiHeader.biSizeImage;
                v.AvgTimePerFrame = o->AvgTimePerFrame;
            }
        } else {
            v.bmiHeader.biCompression = BI_RGB;
            v.bmiHeader.biSizeImage = ((((DWORD)w * 3) + 3) & ~3u) * (DWORD)h;
            if (c.mt.formattype == FORMAT_VideoInfo && c.mt.cbFormat >= sizeof(VIDEOINFOHEADER))
                v.AvgTimePerFrame = ((VIDEOINFOHEADER*)c.mt.pbFormat)->AvgTimePerFrame;
            else if (c.mt.formattype == FORMAT_VideoInfo2 && c.mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
                v.AvgTimePerFrame = ((VIDEOINFOHEADER2*)c.mt.pbFormat)->AvgTimePerFrame;
        }
    }

    memset(&out, 0, sizeof(out));
    out.majortype = MEDIATYPE_Video;
    out.subtype = c.mt.subtype;           // 请求原格式（RGB24 / YUY2 / MJPG）
    out.formattype = FORMAT_VideoInfo;
    out.bFixedSizeSamples = (c.kind != K_MJPG);   // JPEG 变长帧
    out.bTemporalCompression = FALSE;
    out.lSampleSize = v.bmiHeader.biSizeImage;
    out.cbFormat = sizeof(VIDEOINFOHEADER);
    out.pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!out.pbFormat) return false;
    memcpy(out.pbFormat, &v, sizeof(v));
    return true;
}

// ---------------- WIC：MJPG(JPEG) 解码 → BMP、HBITMAP → JPEG ----------------
static void HrErr(std::wstring* err, const wchar_t* step, HRESULT hr)
{
    wchar_t b[96];
    swprintf(b, 96, L"%ls 失败 hr=0x%08lX", step, (unsigned long)hr);
    *err = b;
}

static bool SaveImage24(const std::wstring& outPath, int w, int h,
                        const unsigned char* bgr, int strideBytes);

static bool JpegToBmp24(const std::vector<BYTE>& jpeg, const std::wstring& outPath,
                        std::wstring* err)
{
    if (jpeg.empty()) { *err = L"帧为空"; return false; }
    ComPtr<IWICImagingFactory> wf;
    HRESULT hrC;
    if (FAILED(hrC = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory, (void**)wf.put())) || !wf.p) {
        HrErr(err, L"WIC 工厂", hrC);
        return false;
    }
    ComPtr<IWICStream> stm;
    if (FAILED(wf->CreateStream(stm.put())) ||
        FAILED(stm->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), (UINT)jpeg.size()))) {
        *err = L"WIC 内存流失败";
        return false;
    }
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wf->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, dec.put())) ||
        !dec.p) {
        *err = L"JPEG 解码器失败";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put())) || !fr.p) { *err = L"取帧失败"; return false; }
    UINT iw = 0, ih = 0;
    fr->GetSize(&iw, &ih);
    if (!iw || !ih) { *err = L"JPEG 尺寸无效"; return false; }

    // 统一转换为 24bppBGR（= BMP 的像素序）
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wf->CreateFormatConverter(conv.put())) || !conv.p ||
        FAILED(conv->Initialize(fr, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        *err = L"像素格式转换失败";
        return false;
    }
    const UINT stride = ((iw * 3u + 3u) & ~3u);
    std::vector<BYTE> bgr((size_t)stride * ih, 0);
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)bgr.size(), bgr.data()))) {
        *err = L"读取像素失败";
        return false;
    }
    // MJPG 解码链路（WIC/GDI+）已验证与原始帧逐像素一致，无软件色偏。
    // 色偏源于相机输出，改由驱动层白平衡锁定（LockWhiteBalance）处理；
    // 此处不做全局 gray-world，避免把局部正常区域带偏。
    if (!SaveImage24(outPath, (int)iw, (int)ih, bgr.data(), (int)stride)) {
        *err = L"写 cam 图像失败";
        return false;
    }
    return true;
}

// ---------------- 无损 PNG 写盘（GDI+ 内置编码器） -------------
// 输入为 top-down 的 24bpp BGR 缓冲；输出像素与 BMP 完全一致（无损），体积更小。
DEFINE_GUID(GpClsidPng, 0x557CF406, 0x1A04, 0x11D3, 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E);

bool SavePng24(const std::wstring& path, int w, int h,
               const unsigned char* bgr, int strideBytes)
{
    if (w <= 0 || h <= 0 || !bgr) return false;

    static bool started = false, gdipOk = false;
    if (!started) {
        started = true;
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR tok = 0;
        gdipOk = (Gdiplus::GdiplusStartup(&tok, &in, nullptr) == Gdiplus::Ok) && tok != 0;
    }
    if (!gdipOk) return false;

    // 用 BGR 缓冲构造 GDI+ 位图（PixelFormat24bppRGB 在内存中即 B,G,R 序）
    Gdiplus::Bitmap bmp(w, h, strideBytes, PixelFormat24bppRGB,
                        const_cast<BYTE*>(bgr));
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    Gdiplus::Status st = bmp.Save(path.c_str(), &GpClsidPng, nullptr);
    if (st != Gdiplus::Ok) { DeleteFileW(path.c_str()); return false; }
    return true;
}

// 按设置选择无损 PNG 或 BMP 写出（cam 抓帧统一入口）
static bool SaveImage24(const std::wstring& outPath, int w, int h,
                        const unsigned char* bgr, int strideBytes)
{
    if (g_set.camPng) {
        std::wstring png = outPath;
        size_t dot = png.find_last_of(L'.');
        if (dot != std::wstring::npos) png = png.substr(0, dot);
        png += L".png";
        return SavePng24(png, w, h, bgr, strideBytes);
    }
    return SaveBmp24(outPath, w, h, true, bgr, strideBytes);
}

// HBITMAP → JPEG（GDI+ 内置编码器，q80；不依赖 WIC 编码器注册表）
// hBmp 应未被选入 DC
static bool SaveJpegFromHBitmap(HBITMAP hBmp, const std::wstring& outPath, std::wstring* err)
{
    // GDI+ 支持进程内多次 Startup（各自持 token）；与 clock 的启动互不干扰
    static bool started = false, ok = false;
    if (!started) {
        started = true;
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR tok = 0;
        ok = (Gdiplus::GdiplusStartup(&tok, &in, nullptr) == Gdiplus::Ok) && tok != 0;
        // token 故意不 Shutdown：低频短生命周期，随进程退出回收
    }
    if (!ok) { *err = L"GDI+ 启动失败"; return false; }

    Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromHBITMAP(hBmp, nullptr);
    if (!img || img->GetLastStatus() != Gdiplus::Ok) {
        if (img) delete img;
        *err = L"位图转换失败";
        return false;
    }
    Gdiplus::EncoderParameters eps;
    memset(&eps, 0, sizeof(eps));
    eps.Count = 1;
    eps.Parameter[0].Guid = GpPrmQuality;         // EncoderQuality
    eps.Parameter[0].Type = 4;                    // EncoderParameterValueTypeLong
    eps.Parameter[0].NumberOfValues = 1;
    ULONG q = (ULONG)(kJpegQuality * 100.0f);     // 80
    eps.Parameter[0].Value = &q;
    Gdiplus::Status st = img->Save(outPath.c_str(), &GpClsidJpeg, &eps);
    delete img;
    if (st != Gdiplus::Ok) { *err = L"JPEG 编码失败(GDI+)"; return false; }
    return true;
}

// 锁定白平衡（色温，单位 K）。某些相机的自动白平衡会在左右半幅间产生
// 不均匀色偏；锁定到固定色温可消除。wb=0 表示不动。
static void LockWhiteBalance(IBaseFilter* src, long wb, std::wstring* diag)
{
    if (!src || wb <= 0) return;
    ComPtr<IAMVideoProcAmp> amp;
    if (FAILED(src->QueryInterface(IID_IAMVideoProcAmp, (void**)amp.put())) || !amp.p) {
        if (diag) *diag = L"无 IAMVideoProcAmp";
        return;
    }
    long minV = 0, maxV = 0, step = 0, defV = 0, caps = 0;
    HRESULT hr = amp->GetRange(VideoProcAmp_WhiteBalance, &minV, &maxV, &step, &defV, &caps);
    if (FAILED(hr)) {
        if (diag) { wchar_t b[64]; swprintf(b, 64, L"WB GetRange hr=0x%08lX", hr); *diag = b; }
        return;
    }
    if (caps & VideoProcAmp_Flags_Manual) {
        hr = amp->Set(VideoProcAmp_WhiteBalance, wb, VideoProcAmp_Flags_Manual);
    } else {
        hr = E_FAIL;
    }
    if (diag) {
        wchar_t b[128];
        swprintf(b, 128, L"WB=%ld range[%ld..%ld step=%ld] caps=0x%lX hr=0x%08lX",
                 wb, minV, maxV, step, (unsigned long)caps, hr);
        *diag = b;
    }
}

// ---------------- 单次尝试：全新 graph ----------------
// wildcard=true 时 SampleGrabber 接受任意格式（应对驱动只支持 VideoInfo2 的情况）
static bool AttemptCapture(IMoniker* mon, const FmtCand& c, bool wildcard,
                           const std::wstring& outPath, std::wstring* err,
                           std::wstring* diag, bool* ranGraph)
{
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> build;
    ComPtr<IBaseFilter> src;
    ComPtr<ISampleGrabber> grab;
    ComPtr<IBaseFilter> grabF;
    ComPtr<IBaseFilter> nullF;
    ComPtr<IMediaControl> ctrl;
    HRESULT hr;

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IGraphBuilder, (void**)graph.put())) ||
        FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICaptureGraphBuilder2, (void**)build.put())) ||
        FAILED(build->SetFiltergraph(graph)) ||
        FAILED(mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)src.put())) ||
        FAILED(graph->AddFilter(src, L"src"))) {
        *err = L"graph 构建失败";
        return false;
    }

    // 锁定白平衡（消除左右不均匀色偏）
    LockWhiteBalance(src.p, g_set.camWhiteBalance, diag);

    // 设备当前格式
    ComPtr<IAMStreamConfig> cfg;
    const GUID* cats[2] = { &PIN_CATEGORY_CAPTURE, &PIN_CATEGORY_PREVIEW };
    build->FindInterface(cats[0], &MEDIATYPE_Video, src, IID_IAMStreamConfig, (void**)cfg.put());
    if (!cfg.p) build->FindInterface(cats[1], &MEDIATYPE_Video, src, IID_IAMStreamConfig, (void**)cfg.put());
    if (cfg.p) {
        hr = cfg->SetFormat(const_cast<AM_MEDIA_TYPE*>(&c.mt));
        if (FAILED(hr)) {
            *err = L"SetFormat 被拒绝";
            if (diag) { wchar_t b[64]; swprintf(b, 64, L"SetFormat hr=0x%08lX", hr); *diag = b; }
            return false;
        }
    }

    if (FAILED(CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ISampleGrabber, (void**)grab.put())) ||
        FAILED(grab->QueryInterface(IID_IBaseFilter, (void**)grabF.put()))) {
        *err = L"SampleGrabber 创建失败";
        return false;
    }
    grab->SetOneShot(FALSE);
    grab->SetBufferSamples(TRUE);      // GetCurrentBuffer 依赖缓冲当前帧

    if (wildcard) {
        AM_MEDIA_TYPE mtAny;
        memset(&mtAny, 0, sizeof(mtAny));   // 全 GUID_NULL = 接受任意
        grab->SetMediaType(&mtAny);
    } else {
        AM_MEDIA_TYPE gmt;
        if (!BuildGrabberMt(c, gmt)) { *err = L"内存不足"; return false; }
        hr = grab->SetMediaType(&gmt);
        FreeMt(gmt);
        if (FAILED(hr)) { *err = L"SetMediaType 失败"; return false; }
    }
    graph->AddFilter(grabF, L"Grab");

    if (FAILED(CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IBaseFilter, (void**)nullF.put()))) {
        *err = L"NullRenderer 创建失败";
        return false;
    }
    graph->AddFilter(nullF, L"Null");

    // 连接：capture 引脚 → grabber → null
    hr = build->RenderStream(cats[0], &MEDIATYPE_Video, src, grabF, nullF);
    HRESULT hr2 = hr;
    if (FAILED(hr))
        hr2 = build->RenderStream(cats[1], &MEDIATYPE_Video, src, grabF, nullF);
    if (FAILED(hr) && FAILED(hr2)) {
        *err = L"引脚连接失败";
        if (diag) { wchar_t b[96]; swprintf(b, 96, L"Render hr=0x%08lX/0x%08lX", hr, hr2); *diag = b; }
        return false;
    }

    // 实际连接格式
    AM_MEDIA_TYPE conn;
    memset(&conn, 0, sizeof(conn));
    if (FAILED(grab->GetConnectedMediaType(&conn))) {
        *err = L"GetConnectedMediaType 失败";
        return false;
    }
    int w = 0, h = 0, comp = 0, bc = 0;
    FillFromVih(conn, &w, &h, &comp, &bc);
    bool isRgb24 = (comp == 0 && bc == 24);
    bool isYuy2 = (conn.subtype == MEDIASUBTYPE_YUY2);
    bool isMjpg = (conn.subtype == MEDIASUBTYPE_MJPG) || comp == FOURCC_MJPG;
    if (diag) {
        wchar_t b[160];
        swprintf(b, 160, L"conn %dx%d comp=0x%lX bc=%d sub=%ls fmt=%ls",
                 w, h, (unsigned long)comp, bc,
                 isMjpg ? L"MJPG" : isYuy2 ? L"YUY2" : isRgb24 ? L"RGB24" : L"?",
                 conn.formattype == FORMAT_VideoInfo ? L"VI" :
                 (conn.formattype == FORMAT_VideoInfo2 ? L"VI2" : L"?"));
        *diag = b;
    }
    if (w <= 0 || h <= 0 || (!isRgb24 && !isYuy2 && !isMjpg)) {
        FreeMt(conn);
        *err = L"连接格式不支持（非 RGB24/YUY2/MJPG）";
        return false;
    }
    int srcStride = isRgb24 ? ((w * 3 + 3) & ~3) : (w * 2);
    bool srcTopDown = false;
    if (isRgb24) {
        VIDEOINFOHEADER* v = (VIDEOINFOHEADER*)conn.pbFormat;
        srcTopDown = (v->bmiHeader.biHeight < 0);
    }
    FreeMt(conn);

    // 运行取帧
    graph->QueryInterface(IID_IMediaControl, (void**)ctrl.put());
    if (!ctrl.p) { *err = L"IMediaControl 不可用"; return false; }
    hr = ctrl->Run();
    if (FAILED(hr)) {
        Sleep(400);                       // 驱动释放上一轮占用后重试一次
        hr = ctrl->Run();
    }
    if (FAILED(hr)) {
        *err = L"Run 失败";
        if (diag) { wchar_t b[48]; swprintf(b, 48, L"Run hr=0x%08lX", hr); *diag = b; }
        return false;
    }
    if (ranGraph) *ranGraph = true;        // graph 已跑起来（但可能没帧）

    // Run 后驱动会把白平衡复位为 Auto，此时 pin 已连接、格式已生效，
    // 再锁一次才能让 Manual 保持到取帧结束。
    LockWhiteBalance(src.p, g_set.camWhiteBalance, diag);

    long bufSize = 0;
    bool have = false;
    HRESULT lastG = S_OK;
    std::vector<BYTE> frame;
    for (int t = 0; t < 160; t++) {          // 最长 ~8s
        HRESULT hrG = grab->GetCurrentBuffer(&bufSize, nullptr);
        lastG = hrG;
        if (hrG == S_OK && bufSize > 0) {
            frame.resize((size_t)bufSize);
            long sz = bufSize;
            if (SUCCEEDED(grab->GetCurrentBuffer(&sz, (LONG*)frame.data())) && sz > 0) {
                frame.resize((size_t)sz);
                have = true;
                break;
            }
        }
        Sleep(50);                            // 驱动状态抖动也继续轮询，不提前退出
    }
    ctrl->Stop();
    if (!have) {
        *err = L"取帧超时";
        if (diag) { wchar_t b[48]; swprintf(b, 48, L"last hr=0x%08lX", lastG); *diag = b; }
        return false;
    }

    // 转换 + 写盘：RGB24/YUY2 → BMP；MJPG → GDI+ 解码 JPEG 后写 BMP
    if (isMjpg) {
        if (!JpegToBmp24(frame, outPath, err)) {
            if (err->empty()) *err = L"写 cam.bmp 失败";
            return false;
        }
    } else if (isRgb24) {
        // RGB24 是 bottom-up 的 DIB（srcTopDown 标注真实方向）；先规整为 top-down BGR
        int outStride = ((w * 3 + 3) & ~3);
        std::vector<BYTE> bgr((size_t)outStride * h, 0);
        for (int y = 0; y < h; y++) {
            int sy = srcTopDown ? y : (h - 1 - y);
            memcpy(bgr.data() + (size_t)y * outStride,
                   frame.data() + (size_t)sy * srcStride, (size_t)w * 3);
        }
        if (!SaveImage24(outPath, w, h, bgr.data(), outStride)) {
            *err = L"写 cam 图像失败";
            return false;
        }
    } else {
        int outStride = ((w * 3 + 3) & ~3);
        std::vector<BYTE> bgr((size_t)outStride * h, 0);
        Yuy2ToBgr24(frame.data(), bgr.data(), w, h);
        if (!SaveImage24(outPath, w, h, bgr.data(), outStride)) {
            *err = L"写 cam 图像失败";
            return false;
        }
    }
    LogF(L"cam: 抓帧成功 %dx%d %ls -> %ls", w, h,
         isMjpg ? L"MJPG" : isYuy2 ? L"YUY2" : L"RGB24", outPath.c_str());
    return true;
}

// ---------------- 单设备抓帧入口 ----------------
static bool CaptureDevice(IMoniker* mon, const std::wstring& outPath, std::wstring* err)
{
    std::wstring friendly, diag;
    std::vector<FmtCand> cands;
    if (!CollectCands(mon, cands, &friendly)) {
        *err = L"格式枚举失败";
        LogF(L"cam: %ls 格式枚举失败", friendly.empty() ? L"?" : friendly.c_str());
        return false;
    }
    std::sort(cands.begin(), cands.end(), [](const FmtCand& a, const FmtCand& b) {
        if (a.area != b.area) return a.area > b.area;   // 先看分辨率（8192x2048 优先）
        if (a.rank != b.rank) return a.rank > b.rank;    // 同尺寸：RGB24 > YUY2 > MJPG
        // 同尺寸 MJPG：VI2 条目先试（该驱动拒绝 VI 精确连接，实测 0x80004005）
        if (a.kind == K_MJPG && a.mt.formattype != b.mt.formattype)
            return b.mt.formattype != FORMAT_VideoInfo2;
        return false;
    });

    LogF(L"cam: [%ls] 候选格式数=%d", friendly.c_str(), (int)cands.size());
    for (size_t i = 0; i < cands.size(); i++)
        LogF(L"cam:   #%d rank=%d %dx%d", (int)i, cands[i].rank, cands[i].w, cands[i].h);

    bool ok = false;
    bool ranNoFrame = false;               // graph 跑通但无帧 → 设备不出数据
    for (size_t i = 0; i < cands.size() && !ok; i++) {
        if (i) Sleep(250);                  // 留时间让驱动释放上一轮
        diag.clear();
        std::wstring e;
        // 尝试 1：精确类型
        bool ran = false;
        ok = AttemptCapture(mon, cands[i], false, outPath, &e, &diag, &ran);
        if (ok) break;
        if (ran) ranNoFrame = true;
        *err = e;
        LogF(L"cam: 候选#%d 精确失败 %ls %ls", (int)i, e.c_str(), diag.c_str());

        // 尝试 2：通配类型（应对 VideoInfo2 驱动）
        diag.clear();
        e.clear();
        ran = false;
        ok = AttemptCapture(mon, cands[i], true, outPath, &e, &diag, &ran);
        if (ok) break;
        if (ran) ranNoFrame = true;
        *err = e;
        LogF(L"cam: 候选#%d 通配失败 %ls %ls", (int)i, e.c_str(), diag.c_str());

        if (ranNoFrame) {
            LogF(L"cam: graph 跑通但无帧，终止轮试");
            *err = L"取帧超时（设备不出帧）";
            break;
        }
    }

    for (auto& c : cands) FreeMt(c.mt);
    if (!ok && err->empty()) *err = L"摄像头抓帧失败";
    return ok;
}

// ---------------- 屏幕截取 ----------------
static bool CaptureScreen(const std::wstring& outPath, std::wstring* err)
{
    int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (sw <= 0 || sh <= 0) { *err = L"屏幕尺寸无效"; return false; }

    int outW = g_set.uhdOutput ? 3840 : sw;
    int outH = g_set.uhdOutput ? 2160 : sh;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) { *err = L"GetDC 失败"; return false; }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, outW, outH);
    bool ok = false;
    if (hdcMem && hBmp) {
        HGDIOBJ old = SelectObject(hdcMem, hBmp);
        if (sw != outW || sh != outH) {
            SetStretchBltMode(hdcMem, HALFTONE);
            SetBrushOrgEx(hdcMem, 0, 0, nullptr);
            ok = StretchBlt(hdcMem, 0, 0, outW, outH, hdcScreen, sx, sy, sw, sh, SRCCOPY) != 0;
        } else {
            ok = BitBlt(hdcMem, 0, 0, outW, outH, hdcScreen, sx, sy, SRCCOPY) != 0;
        }
        // 先取消选入：FromHBITMAP 需要未选入 DC 的位图
        SelectObject(hdcMem, old);
        if (ok) {
            // 编码为 JPEG（q80）落盘 —— 屏幕截图用 jpg 省空间
            ok = SaveJpegFromHBitmap(hBmp, outPath, err);
        }
    } else *err = L"屏幕位图创建失败";

    if (hBmp) DeleteObject(hBmp);
    if (hdcMem) DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok && err->empty()) *err = L"截屏失败";
    return ok;
}

// ---------------- 归档：时间戳命名 + 保留 N 个自动清理 ----------------
// 时间戳定宽（_YYYYMMDD_HHMMSS）→ 文件名字典序即时间序，倒序删除即可
static std::wstring ArchivePath(const std::wstring& dir, const wchar_t* prefix, const wchar_t* ext)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[48];
    swprintf(ts, 48, L"_%04d%02d%02d_%02d%02d%02d%ls",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
    return dir + L"\\" + prefix + ts;
}

// 单个归档文件信息（用于按时间/大小清理）
struct ArcFile {
    std::wstring name;
    ULONGLONG    size;   // 字节
    FILETIME     ft;     // 最后写入时间（UTC）
};

static ULONGLONG FileTimeToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// 采集某前缀归档文件列表（新→旧排序）
static void CollectArchive(const std::wstring& dir, const wchar_t* prefix, const wchar_t* ext,
                           std::vector<ArcFile>& out)
{
    std::wstring pat = dir + L"\\" + prefix + L"_*" + ext;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ArcFile a;
        a.name = fd.cFileName;
        a.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        a.ft   = fd.ftLastWriteTime;
        out.push_back(a);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // 文件名时间戳定宽，字典序倒排即新→旧
    std::sort(out.begin(), out.end(), [](const ArcFile& x, const ArcFile& y) {
        return x.name > y.name;
    });
}

// 增强版归档清理：按 pruneMode 组合「数量 / 天数 / 总大小」三种策略
// 时间线模式（timelineMode）下永不自动清理，保证时间轴上的条目都能取到原图。
static void PruneArchive(const std::wstring& dir, const wchar_t* prefix, const wchar_t* ext)
{
    if (g_set.timelineMode) return;   // 时间线存档：全部永久保留
    if (g_set.pruneMode == 0) return; // 0 = 不清理

    std::vector<ArcFile> files;
    CollectArchive(dir, prefix, ext, files);
    if (files.empty()) return;

    const bool byCount = (g_set.pruneMode & 1) != 0;
    const bool byDays  = (g_set.pruneMode & 2) != 0;
    const bool bySize  = (g_set.pruneMode & 4) != 0;

    // 需要保留的最旧边界（UTC 100ns 单位）
    ULONGLONG cutoffTicks = 0;
    if (byDays) {
        FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
        ULONGLONG dayTicks = 864000000000ULL * (ULONGLONG)g_set.keepDays;  // 100ns * 秒 * 天
        ULONGLONG now = FileTimeToU64(nowFt);
        cutoffTicks = (now > dayTicks) ? (now - dayTicks) : 0;
    }

    ULONGLONG sizeCap = bySize ? ((ULONGLONG)g_set.keepMB * 1024ULL * 1024ULL) : 0;

    int removed = 0;
    ULONGLONG accSize = 0;
    for (int i = 0; i < (int)files.size(); i++) {
        bool del = false;
        // 数量：超出 keepCount 的（已新→旧排序，i >= keepCount 即旧于保留线）
        if (byCount && i >= g_set.keepCount) del = true;
        // 天数：早于 cutoff
        if (!del && byDays && FileTimeToU64(files[i].ft) < cutoffTicks) del = true;
        // 大小：累计（含本文件）超过上限则删本文件及其后所有更旧文件
        if (!del && bySize) {
            accSize += files[i].size;
            if (accSize > sizeCap) del = true;
        }
        if (del) {
            DeleteFileW((dir + L"\\" + files[i].name).c_str());
            removed++;
        }
    }
    if (removed > 0)
        LogF(L"archive: 清理 %ls 前缀 %d 个（模式=%d 保留数=%d 保留天=%d 上限MB=%d）",
             prefix, removed, g_set.pruneMode, g_set.keepCount, g_set.keepDays, g_set.keepMB);
}

// ---------------- 一轮抓拍 ----------------
void RunCaptureCycle(std::wstring* statusOut)
{
    std::wstring dir = g_set.captureDir;
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    CreateDirectoryW(dir.c_str(), nullptr);

    const wchar_t* camExt = g_set.camPng ? L".png" : L".bmp";
    std::wstring scrPath, camPath;
    if (g_set.archiveMode) {
        scrPath = ArchivePath(dir, L"uhd", L".jpg");
        camPath = ArchivePath(dir, L"cam", camExt);
    } else {
        scrPath = dir + L"\\uhd.jpg";
        camPath = dir + L"\\cam"; camPath += camExt;
    }

    // 1) 屏幕（JPEG）
    std::wstring screenErr;
    bool scrOk = CaptureScreen(scrPath, &screenErr);
    if (scrOk && g_set.archiveMode) PruneArchive(dir, L"uhd", L".jpg");

    // 2) 摄像头
    std::wstring camErr;
    bool camOk = false;
    std::vector<CamInfo> cams = EnumCameras();
    if (cams.empty()) {
        camErr = L"未发现摄像头";
    } else {
        bool explicitCam = (g_set.camIndex >= 0 && g_set.camIndex < (int)cams.size());
        int idx = 0, tries = 0;
        int maxTries = explicitCam ? 1 : (int)cams.size();
        while (tries < maxTries && !camOk) {
            if (explicitCam) idx = g_set.camIndex;
            tries++;

            // 重新枚举取 moniker（保证有效）
            std::vector<CamInfo> cur = EnumCameras();
            if (idx >= (int)cur.size()) { camErr = L"设备序号无效"; break; }

            ComPtr<ICreateDevEnum> devEnum;
            ComPtr<IEnumMoniker> enumM;
            IMoniker* mon = nullptr;
            bool gotMon = false;
            if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_ICreateDevEnum, (void**)devEnum.put())) &&
                SUCCEEDED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, enumM.put(), 0)) &&
                enumM.p) {
                for (int k = 0; k <= idx; k++) {
                    if (enumM->Next(1, &mon, nullptr) != S_OK) { mon = nullptr; break; }
                    if (k < idx) { mon->Release(); mon = nullptr; }
                }
                gotMon = (mon != nullptr);
            }
            if (!gotMon) { camErr = L"设备获取失败"; break; }

            std::wstring err;
            camOk = CaptureDevice(mon, camPath, &err);
            if (!camOk) camErr = err.empty() ? L"抓帧失败" : err;
            mon->Release();

            if (!explicitCam) idx++;   // 自动模式下一台
        }
    }
    if (camOk && g_set.archiveMode) {
        // BMP 与 PNG 都是合法的 cam 归档扩展名，分别清理，避免旧格式成为孤儿
        PruneArchive(dir, L"cam", L".bmp");
        PruneArchive(dir, L"cam", L".png");
    }

    std::wstring msg = scrOk ? std::wstring(L"屏幕OK")
                             : (std::wstring(L"屏幕失败") + (screenErr.empty() ? L"" : L"(" + screenErr + L")"));
    msg += camOk ? std::wstring(L"  摄像头OK")
                 : (std::wstring(L"  摄像头失败") + (camErr.empty() ? L"" : L"(" + camErr + L")"));
    LogF(L"capture: %ls dir=%ls", msg.c_str(), dir.c_str());
    if (statusOut) *statusOut = msg;
}

// ---------------- 异步启动 ----------------
static DWORD WINAPI CaptureThreadProc(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring status;
    RunCaptureCycle(&status);
    std::wstring* p = new std::wstring(std::move(status));
    g_captureBusy = false;
    PostMessageW(g_hMsgWnd, WM_APP_CAPDONE, 0, (LPARAM)p);
    CoUninitialize();
    return 0;
}

void StartCaptureAsync()
{
    bool expected = false;
    if (!g_captureBusy.compare_exchange_strong(expected, true)) return;
    HANDLE h = CreateThread(nullptr, 0, CaptureThreadProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        g_captureBusy = false;
        LogF(L"capture: 启动线程失败");
    }
}
