#include <initguid.h>
#include <dshow.h>
#include <stdio.h>

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ICreateDevEnum* dev = nullptr;
    CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&dev);
    IEnumMoniker* en = nullptr;
    dev->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0);
    if (!en) { wprintf(L"no camera\n"); return 1; }
    IMoniker* mon = nullptr;
    while (en->Next(1, &mon, nullptr) == S_OK) {
        IBaseFilter* src = nullptr;
        mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&src);
        IAMVideoProcAmp* amp = nullptr;
        if (src && SUCCEEDED(src->QueryInterface(IID_IAMVideoProcAmp, (void**)&amp)) && amp) {
            long mn=0,mx=0,st=0,df=0,cp=0;
            HRESULT hr = amp->GetRange(VideoProcAmp_WhiteBalance, &mn,&mx,&st,&df,&cp);
            long cur=0, fl=0;
            HRESULT hr2 = amp->Get(VideoProcAmp_WhiteBalance, &cur, &fl);
            wprintf(L"range[%ld..%ld step=%ld def=%ld] caps=0x%lX init cur=%ld flags=0x%lX\n",
                    mn, mx, st, df, cp, cur, fl);
            long tests[4] = { 2500, 4500, 6500, 8000 };
            for (int i = 0; i < 4; i++) {
                HRESULT hs = amp->Set(VideoProcAmp_WhiteBalance, tests[i], VideoProcAmp_Flags_Manual);
                long c2=0, f2=0;
                amp->Get(VideoProcAmp_WhiteBalance, &c2, &f2);
                wprintf(L"  Set(%ld,Manual) hr=0x%08lX -> Get cur=%ld flags=0x%lX\n", tests[i], hs, c2, f2);
            }
            HRESULT hsn = amp->Set(VideoProcAmp_WhiteBalance, 0, VideoProcAmp_Flags_Auto);
            long c3=0, f3=0;
            amp->Get(VideoProcAmp_WhiteBalance, &c3, &f3);
            wprintf(L"  Set(Auto) hr=0x%08lX -> Get cur=%ld flags=0x%lX\n", hsn, c3, f3);
            amp->Release();
        } else {
            wprintf(L"device has no IAMVideoProcAmp\n");
        }
        if (src) src->Release();
        mon->Release();
        break;
    }
    if (en) en->Release();
    if (dev) dev->Release();
    CoUninitialize();
    return 0;
}
