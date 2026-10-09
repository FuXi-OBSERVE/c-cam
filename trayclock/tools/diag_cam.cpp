// diag_cam.cpp — 诊断：列出所有视频输入设备、每个引脚的 IAMStreamConfig caps
// 与引脚原生 media types（含被 capture.cpp 过滤掉的 MJPG 等格式）。
// 输出：UTF-8 stdout + 同目录 diag_cam.txt
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

static FILE* g_out = nullptr;

static void P(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_out) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
    }
}

static std::string GuidStr(const GUID& g)
{
    char b[64];
    snprintf(b, sizeof(b), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

// fourcc 可打印则按 4 字符，否则十六进制
static std::string Fourcc(DWORD f)
{
    char c[5] = { (char)(f & 0xFF), (char)((f >> 8) & 0xFF),
                  (char)((f >> 16) & 0xFF), (char)((f >> 24) & 0xFF), 0 };
    bool printable = true;
    for (int i = 0; i < 4; i++)
        if (c[i] < 0x20 || c[i] > 0x7E) printable = false;
    if (printable) return std::string("\"") + c + "\"";
    char b[16];
    snprintf(b, sizeof(b), "0x%08lX", f);
    return b;
}

static std::string SubtypeName(const GUID& s)
{
    if (s == MEDIASUBTYPE_YUY2) return "YUY2";
    if (s == MEDIASUBTYPE_RGB24) return "RGB24";
    if (s == MEDIASUBTYPE_RGB32) return "RGB32";
    if (s == MEDIASUBTYPE_MJPG) return "MJPG";
    if (s == MEDIASUBTYPE_NV12) return "NV12";
    if (s == MEDIASUBTYPE_IYUV) return "IYUV";
    return GuidStr(s);
}

static void FillDims(const AM_MEDIA_TYPE& mt, int* w, int* h, int* bc, DWORD* comp)
{
    *w = *h = 0; *bc = 0; *comp = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER* v = (VIDEOINFOHEADER*)mt.pbFormat;
        *w = v->bmiHeader.biWidth; *h = abs(v->bmiHeader.biHeight);
        *bc = v->bmiHeader.biBitCount; *comp = v->bmiHeader.biCompression;
    } else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        VIDEOINFOHEADER2* v = (VIDEOINFOHEADER2*)mt.pbFormat;
        *w = v->bmiHeader.biWidth; *h = abs(v->bmiHeader.biHeight);
        *bc = v->bmiHeader.biBitCount; *comp = v->bmiHeader.biCompression;
    }
}

static double Fps(const AM_MEDIA_TYPE& mt)
{
    LONGLONG t = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER))
        t = ((VIDEOINFOHEADER*)mt.pbFormat)->AvgTimePerFrame;
    else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
        t = ((VIDEOINFOHEADER2*)mt.pbFormat)->AvgTimePerFrame;
    return t ? 1e7 / (double)t : 0.0;
}

static void DumpPin(IPin* pin, int devIdx, int pinIdx)
{
    PIN_INFO pi{};
    std::string pname = "?", dir = "?";
    if (SUCCEEDED(pin->QueryPinInfo(&pi))) {
        if (pi.pFilter) pi.pFilter->Release();
        if (pi.achName[0]) {
            int n = 0;
            while (n < 127 && pi.achName[n]) n++;
            char u8[512];
            int m = WideCharToMultiByte(CP_UTF8, 0, pi.achName, n, u8, sizeof(u8) - 1, nullptr, nullptr);
            if (m > 0) { u8[m] = 0; pname = u8; }
        }
        dir = (pi.dir == PINDIR_INPUT) ? "in" : "out";
    }
    P("  pin#%d [%s] dir=%s\n", pinIdx, pname.c_str(), dir.c_str());

    // --- IAMStreamConfig caps ---
    ComPtr<IAMStreamConfig> cfg;
    if (SUCCEEDED(pin->QueryInterface(IID_IAMStreamConfig, (void**)cfg.put())) && cfg.p) {
        int n = 0, size = 0;
        if (SUCCEEDED(cfg->GetNumberOfCapabilities(&n, &size)) && n > 0) {
            P("    IAMStreamConfig: nCaps=%d size=%d\n", n, size);
            for (int i = 0; i < n; i++) {
                AM_MEDIA_TYPE* pmt = nullptr;
                std::vector<BYTE> capBuf((size_t)size, 0);
                if (FAILED(cfg->GetStreamCaps(i, &pmt, capBuf.data())) || !pmt) {
                    P("      cap#%d: GetStreamCaps 失败\n", i);
                    continue;
                }
                int w, h, bc; DWORD comp;
                FillDims(*pmt, &w, &h, &bc, &comp);
                const char* fmt = "?";
                if (pmt->formattype == FORMAT_VideoInfo) fmt = "VI";
                else if (pmt->formattype == FORMAT_VideoInfo2) fmt = "VI2";
                P("      cap#%d: sub=%s fmt=%s %dx%d bc=%d comp=%s fps=%.1f\n",
                  i, SubtypeName(pmt->subtype).c_str(), fmt, w, h, bc,
                  Fourcc(comp).c_str(), Fps(*pmt));
                // VIDEO_STREAM_CONFIG_CAPS 的 min/max 分辨率（caps 是范围型时）
                if (size >= sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
                    VIDEO_STREAM_CONFIG_CAPS* sc = (VIDEO_STREAM_CONFIG_CAPS*)capBuf.data();
                    if (sc->MinOutputSize.cx != sc->MaxOutputSize.cx ||
                        sc->MinOutputSize.cy != sc->MaxOutputSize.cy)
                        P("        range: min=%dx%d max=%dx%d\n",
                          sc->MinOutputSize.cx, sc->MinOutputSize.cy,
                          sc->MaxOutputSize.cx, sc->MaxOutputSize.cy);
                }
                if (pmt->pbFormat) CoTaskMemFree(pmt->pbFormat);
                CoTaskMemFree(pmt);
            }
        } else {
            P("    IAMStreamConfig: GetNumberOfCapabilities 失败/0\n");
        }
    } else {
        P("    (无 IAMStreamConfig)\n");
    }

    // --- 引脚原生 IEnumMediaTypes（驱动实际可协商的固定类型） ---
    ComPtr<IEnumMediaTypes> emt;
    if (SUCCEEDED(pin->EnumMediaTypes(emt.put())) && emt.p) {
        AM_MEDIA_TYPE* mt = nullptr;
        int k = 0;
        while (emt->Next(1, &mt, nullptr) == S_OK && mt) {
            int w, h, bc; DWORD comp;
            FillDims(*mt, &w, &h, &bc, &comp);
            const char* fmt = "?";
            if (mt->formattype == FORMAT_VideoInfo) fmt = "VI";
            else if (mt->formattype == FORMAT_VideoInfo2) fmt = "VI2";
            else if (mt->formattype == FORMAT_MPEGVideo) fmt = "MPEGV";
            P("    mt#%d: sub=%s fmt=%s %dx%d bc=%d comp=%s\n",
              k, SubtypeName(mt->subtype).c_str(), fmt, w, h, bc, Fourcc(comp).c_str());
            if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
            if (mt->pUnk) mt->pUnk->Release();
            CoTaskMemFree(mt);
            k++;
            if (k >= 64) break;
        }
    }
}

int main(int argc, char** argv)
{
    const char* outPath = (argc > 1) ? argv[1] : "diag_cam.txt";
    g_out = fopen(outPath, "wb"); // UTF-8 文本
    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;
    P("=== diag_cam ===\n");

    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum, (void**)devEnum.put())) || !devEnum.p) {
        P("CreateClassEnumerator CoCreate 失败\n");
        return 1;
    }
    ComPtr<IEnumMoniker> enumM;
    if (FAILED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, enumM.put(), 0))
        || !enumM.p) {
        P("未发现任何视频输入设备\n");
        return 1;
    }

    IMoniker* mon = nullptr;
    int devIdx = 0;
    while (enumM->Next(1, &mon, nullptr) == S_OK) {
        std::string name = "?";
        ComPtr<IPropertyBag> pb;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)pb.put()))) {
            VARIANT var;
            VariantInit(&var);
            if (SUCCEEDED(pb->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal) {
                int n = (int)wcslen(var.bstrVal);
                char u8[512];
                int m = WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, n, u8, sizeof(u8) - 1, nullptr, nullptr);
                if (m > 0) { u8[m] = 0; name = u8; }
            }
            VariantClear(&var);
        }
        P("device#%d: %s\n", devIdx, name.c_str());

        ComPtr<IBaseFilter> flt;
        if (SUCCEEDED(mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)flt.put())) && flt.p) {
            ComPtr<IEnumPins> ep;
            if (SUCCEEDED(flt->EnumPins(ep.put())) && ep.p) {
                IPin* pin = nullptr;
                int pinIdx = 0;
                while (ep->Next(1, &pin, nullptr) == S_OK) {
                    DumpPin(pin, devIdx, pinIdx++);
                    pin->Release();
                }
            }
        } else {
            P("  BindToObject 失败\n");
        }
        mon->Release();
        devIdx++;
    }
    if (devIdx == 0) P("（无设备）\n");

    if (g_out) fclose(g_out);
    CoUninitialize();
    return 0;
}
