// Minimal async MFT harness: drive the MT H.264 MFT with proper async
// event handling and report where (if anywhere) it blocks.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <cstdio>
#include <vector>

static const CLSID kClsid = {0x60F44560, 0x5A20, 0x4857,
                             {0xBF, 0xEF, 0xD2, 0x97, 0x73, 0xCB, 0x80, 0x40}};
typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFn)(REFCLSID, REFIID, void**);

static bool wait_event(IMFMediaEventGenerator* ev, MediaEventType want, DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        IMFMediaEvent* e = nullptr;
        HRESULT r = ev->GetEvent(MF_EVENT_FLAG_NO_WAIT, &e);
        if (r == MF_E_NO_EVENTS_AVAILABLE) { Sleep(5); continue; }
        if (FAILED(r)) { printf("  GetEvent hr=0x%08lx\n", (unsigned long)r); return false; }
        MediaEventType type = MEUnknown;
        e->GetType(&type);
        printf("  event type=%d\n", (int)type);
        e->Release();
        if (type == want) return true;
    }
    return false;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    HMODULE mod = LoadLibraryW(L"C:\\mtgpu-woa\\mtmft13.dll");
    auto pfn = (DllGetClassObjectFn)GetProcAddress(mod, "DllGetClassObject");
    IClassFactory* cf = nullptr;
    HRESULT hr = pfn(kClsid, IID_IClassFactory, (void**)&cf);
    printf("class factory hr=0x%08lx\n", (unsigned long)hr);
    IMFTransform* mft = nullptr;
    hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&mft));
    printf("create hr=0x%08lx\n", (unsigned long)hr);
    if (!mft) return 1;

    IMFAttributes* a = nullptr;
    mft->GetAttributes(&a);
    hr = a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    printf("async unlock hr=0x%08lx\n", (unsigned long)hr);

    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, 640, 360);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, 30, 1);
    hr = mft->SetInputType(0, in, 0);
    printf("SetInputType hr=0x%08lx\n", (unsigned long)hr);

    IMFMediaType* out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, 640, 360);
    out->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, 30, 1);
    hr = mft->SetOutputType(0, out, 0);
    printf("SetOutputType hr=0x%08lx\n", (unsigned long)hr);

    // ICodecAPI options skipped in this harness

    MFT_OUTPUT_STREAM_INFO osi = {};
    mft->GetOutputStreamInfo(0, &osi);
    printf("out stream flags=0x%lx size=%lu\n", (unsigned long)osi.dwFlags, (unsigned long)osi.cbSize);

    IMFMediaEventGenerator* ev = nullptr;
    hr = mft->QueryInterface(IID_IMFMediaEventGenerator, (void**)&ev);
    printf("eventgen hr=0x%08lx\n", (unsigned long)hr);
    if (!ev) return 1;

    printf("ProcessMessage BEGIN_STREAMING\n");
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (!wait_event(ev, METransformNeedInput, 3000)) {
        printf("NO_NEED_INPUT\n");
        return 2;
    }
    printf("got NeedInput -> ProcessInput\n");

    // Build one NV12 frame.
    const UINT32 fw = 640, fh = 360;
    DWORD bufSize = fw * fh * 3 / 2;
    IMFMediaBuffer* buf = nullptr;
    MFCreateMemoryBuffer(bufSize, &buf);
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memset(p, 0x40, bufSize);
    buf->Unlock();
    buf->SetCurrentLength(bufSize);
    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buf);
    sample->SetSampleTime(0);
    sample->SetSampleDuration(333333);
    hr = mft->ProcessInput(0, sample, 0);
    printf("ProcessInput hr=0x%08lx\n", (unsigned long)hr);

    for (int i = 0; i < 48; i++) {
        if (!wait_event(ev, METransformHaveOutput, 3000)) {
            printf("NO_HAVE_OUTPUT (iter %d)\n", i);
            break;
        }
        MFT_OUTPUT_DATA_BUFFER db = {};
        db.dwStreamID = 0;
        DWORD status = 0;
        hr = mft->ProcessOutput(0, 1, &db, &status);
        printf("ProcessOutput hr=0x%08lx status=0x%lx sample=%p\n",
               (unsigned long)hr, (unsigned long)status, (void*)db.pSample);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            printf("need more input\n");
            break;
        }
        if (db.pSample) {
            DWORD len = 0;
            db.pSample->GetTotalLength(&len);
            printf("  encoded sample %lu bytes\n", (unsigned long)len);
            db.pSample->Release();
            break;
        }
        if (db.pEvents) db.pEvents->Release();
        if (!wait_event(ev, METransformNeedInput, 3000)) {
            printf("NO_NEED_INPUT_AFTER_OUTPUT\n");
            break;
        }
        hr = mft->ProcessInput(0, sample, 0);
        printf("ProcessInput#2 hr=0x%08lx\n", (unsigned long)hr);
    }

    sample->Release();
    buf->Release();
    ev->Release();
    mft->Release();
    cf->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
