// Probe the MT H.264 MFT (registered under the NVIDIA CLSID) for async mode
// and supported input/output types, to explain why FFmpeg h264_mf hangs.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <cstdio>

static const char* gname(const GUID& g) {
    static char b[64];
    snprintf(b, sizeof b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    CLSID clsid = {0x60F44560, 0x5A20, 0x4857,
                   {0xBF, 0xEF, 0xD2, 0x97, 0x73, 0xCB, 0x80, 0x40}};
    IMFTransform* mft = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&mft));
    printf("CoCreate hr=0x%08lx mft=%p\n", (unsigned long)hr, (void*)mft);
    if (!mft) return 1;

    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&attr)) && attr) {
        UINT32 v = 0;
        if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC, &v)))
            printf("MF_TRANSFORM_ASYNC=%u\n", v);
        if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, &v)))
            printf("MF_TRANSFORM_ASYNC_UNLOCK=%u\n", v);
        if (SUCCEEDED(attr->GetUINT32(MF_SA_D3D11_AWARE, &v)))
            printf("MF_SA_D3D11_AWARE=%u\n", v);
        if (SUCCEEDED(attr->GetUINT32(MF_LOW_LATENCY, &v)))
            printf("MF_LOW_LATENCY=%u\n", v);
        attr->Release();
    }

    for (DWORD i = 0;; ++i) {
        IMFMediaType* t = nullptr;
        hr = mft->GetInputAvailableType(0, i, &t);
        if (FAILED(hr)) { printf("in[%lu] end hr=0x%08lx\n", (unsigned long)i, (unsigned long)hr); break; }
        GUID maj = {}, sub = {};
        t->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        printf("in[%lu] maj=%s sub=%s\n", (unsigned long)i, gname(maj), gname(sub));
        t->Release();
    }
    for (DWORD i = 0;; ++i) {
        IMFMediaType* t = nullptr;
        hr = mft->GetOutputAvailableType(0, i, &t);
        if (FAILED(hr)) { printf("out[%lu] end hr=0x%08lx\n", (unsigned long)i, (unsigned long)hr); break; }
        GUID maj = {}, sub = {};
        t->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        printf("out[%lu] maj=%s sub=%s\n", (unsigned long)i, gname(maj), gname(sub));
        t->Release();
    }
    mft->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
