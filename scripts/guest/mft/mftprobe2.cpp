// Feasibility probe for a synchronous proxy MFT: load mtmft13.dll directly,
// create the transform through its class factory, and check that it exposes
// IMFMediaEventGenerator (async event source).
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdio>

static const CLSID kClsid = {0x60F44560, 0x5A20, 0x4857,
                             {0xBF, 0xEF, 0xD2, 0x97, 0x73, 0xCB, 0x80, 0x40}};

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFn)(REFCLSID, REFIID, void**);

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    HMODULE h = LoadLibraryW(L"C:\\mtgpu-woa\\mtmft13.dll");
    printf("LoadLibrary=%p err=%lu\n", (void*)h, GetLastError());
    if (!h) return 1;
    auto pfn = (DllGetClassObjectFn)GetProcAddress(h, "DllGetClassObject");
    printf("DllGetClassObject=%p\n", (void*)pfn);
    if (!pfn) return 1;
    IClassFactory* cf = nullptr;
    HRESULT hr = pfn(kClsid, IID_IClassFactory, (void**)&cf);
    printf("GetClassObject hr=0x%08lx cf=%p\n", (unsigned long)hr, (void*)cf);
    if (FAILED(hr)) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        hr = pfn(kClsid, IID_IClassFactory, (void**)&cf);
        printf("GetClassObject(STA) hr=0x%08lx cf=%p\n", (unsigned long)hr, (void*)cf);
    }
    if (!cf) return 1;
    IMFTransform* mft = nullptr;
    hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&mft));
    printf("CreateInstance hr=0x%08lx mft=%p\n", (unsigned long)hr, (void*)mft);
    if (!mft) return 1;
    IMFMediaEventGenerator* eg = nullptr;
    hr = mft->QueryInterface(IID_IMFMediaEventGenerator, (void**)&eg);
    printf("Query IMFMediaEventGenerator hr=0x%08lx eg=%p\n",
           (unsigned long)hr, (void*)eg);
    IMFAttributes* a = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&a)) && a) {
        UINT32 v = 0;
        if (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_ASYNC, &v)))
            printf("async=%u\n", v);
        a->Release();
    }
    if (eg) eg->Release();
    mft->Release();
    cf->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
