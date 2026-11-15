// mtmft_sync_proxy.dll — an asynchronous IMFTransform facade around the MT
// (MediaTek) H.264 hardware MFT.
//
// Why: the MT MFT is async (MF_TRANSFORM_ASYNC=1) and FFmpeg's h264_mf (used
// by Sunshine) requires hardware MFTs to be async, but its async event loop
// does not work with this MFT's event pattern. This proxy exposes the same
// async contract but:
//   * owns an IMFMediaEventGenerator and mirrors the inner MFT's
//     NeedInput / HaveOutput / DrainComplete events into it,
//   * forwards ProcessInput/ProcessOutput/ProcessMessage to the inner MFT,
//   * does not expose ICodecAPI, which is where the inner MFT can block
//     during encoder open.
//
// Inner MFT DLL path: registry value MTMFT_DLL under
//   HKLM\SOFTWARE\Classes\CLSID\{60F44560-...}\InprocServer32
// (default C:\mtgpu-woa\mtmft13.dll).

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <cstdarg>

static void plog(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\mtgpu-woa\\proxy.log", "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const CLSID kInnerClsid = {0x60F44560, 0x5A20, 0x4857,
                                  {0xBF, 0xEF, 0xD2, 0x97, 0x73, 0xCB, 0x80, 0x40}};

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFn)(REFCLSID, REFIID, void**);

static std::wstring inner_dll_path() {
    wchar_t buf[MAX_PATH] = L"C:\\mtgpu-woa\\mtmft13.dll";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Classes\\CLSID\\{60F44560-5A20-4857-BFEF-D29773CB8040}\\InprocServer32",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, cb = sizeof(buf);
        RegQueryValueExW(k, L"MTMFT_DLL", nullptr, &type, (LPBYTE)buf, &cb);
        RegCloseKey(k);
    }
    return buf;
}

class ProxyMFT : public IMFTransform, public IMFMediaEventGenerator {
public:
    ProxyMFT() = default;
    ~ProxyMFT() { shutdown(); }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IMFTransform) {
            *ppv = static_cast<IMFTransform*>(this);
        } else if (riid == IID_IMFMediaEventGenerator) {
            *ppv = static_cast<IMFMediaEventGenerator*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    // ---- init ----
    HRESULT init() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!mf_started_) {
            MFStartup(MF_VERSION);
            mf_started_ = true;
        }
        HMODULE mod = LoadLibraryW(inner_dll_path().c_str());
        if (!mod) return HRESULT_FROM_WIN32(GetLastError());
        auto pfn = (DllGetClassObjectFn)GetProcAddress(mod, "DllGetClassObject");
        if (!pfn) return E_FAIL;
        IClassFactory* cf = nullptr;
        HRESULT hr = pfn(kInnerClsid, IID_IClassFactory, (void**)&cf);
        if (FAILED(hr) || !cf) return hr;
        hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&inner_));
        cf->Release();
        if (FAILED(hr) || !inner_) return hr;

        IMFAttributes* ia = nullptr;
        if (SUCCEEDED(inner_->GetAttributes(&ia)) && ia) {
            ia->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            MFCreateAttributes(&attrs_, 16);
            if (attrs_) {
                attrs_->CopyAllItems(ia);
                attrs_->SetUINT32(MF_TRANSFORM_ASYNC, TRUE);
                attrs_->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            ia->Release();
        } else {
            return E_FAIL;
        }
        hr = inner_->QueryInterface(IID_IMFMediaEventGenerator, (void**)&inner_events_);
        if (FAILED(hr)) return hr;
        inner_->GetOutputStreamInfo(0, &osi_);
        plog("init ok");
        pump_ = std::thread([this] { pump_loop(); });
        return S_OK;
    }

    // ---- IMFMediaEventGenerator ----
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** ev) {
        if (!ev) return E_POINTER;
        *ev = nullptr;
        std::unique_lock<std::mutex> lk(qmtx_);
        if (queue_.empty() && (flags & MF_EVENT_FLAG_NO_WAIT)) {
            return MF_E_NO_EVENTS_AVAILABLE;
        }
        plog("GetEvent wait q=%zu", queue_.size());
        qcv_.wait(lk, [this] { return !queue_.empty() || stop_; });
        if (queue_.empty()) return MF_E_SHUTDOWN;
        *ev = queue_.front();
        queue_.pop_front();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult*, IMFMediaEvent** ev) {
        if (!ev) return E_POINTER;
        *ev = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue) {
        IMFMediaEvent* e = nullptr;
        HRESULT hr = MFCreateMediaEvent(met, guidExtendedType, hrStatus, pvValue, &e);
        if (FAILED(hr) || !e) return hr;
        if (met == METransformNeedInput) {
            e->SetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, 0);
        }
        {
            std::lock_guard<std::mutex> lk(qmtx_);
            queue_.push_back(e);
        }
        qcv_.notify_all();
        return S_OK;
    }

    // ---- IMFTransform ----
    HRESULT STDMETHODCALLTYPE GetStreamLimits(DWORD* a, DWORD* b, DWORD* c, DWORD* d) override {
        return inner_ ? inner_->GetStreamLimits(a, b, c, d) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* a, DWORD* b) override {
        return inner_ ? inner_->GetStreamCount(a, b) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO* i) override {
        return inner_ ? inner_->GetInputStreamInfo(id, i) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO* i) override {
        if (!inner_) return MF_E_NOT_INITIALIZED;
        HRESULT hr = inner_->GetOutputStreamInfo(id, i);
        if (SUCCEEDED(hr)) osi_ = *i;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(IMFAttributes** a) override {
        if (!a) return E_POINTER;
        if (!attrs_) return MF_E_NOT_INITIALIZED;
        attrs_->AddRef();
        *a = attrs_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputStreamAttributes(DWORD id, IMFAttributes** a) override {
        if (!a) return E_POINTER;
        *a = nullptr;
        return inner_ ? inner_->GetInputStreamAttributes(id, a) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetOutputStreamAttributes(DWORD id, IMFAttributes** a) override {
        if (!a) return E_POINTER;
        *a = nullptr;
        return inner_ ? inner_->GetOutputStreamAttributes(id, a) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE DeleteInputStream(DWORD id) override {
        return inner_ ? inner_->DeleteInputStream(id) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE AddInputStreams(DWORD n, DWORD* ids) override {
        return inner_ ? inner_->AddInputStreams(n, ids) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetInputAvailableType(DWORD id, DWORD i, IMFMediaType** t) override {
        return inner_ ? inner_->GetInputAvailableType(id, i, t) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetOutputAvailableType(DWORD id, DWORD i, IMFMediaType** t) override {
        return inner_ ? inner_->GetOutputAvailableType(id, i, t) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD id, IMFMediaType* t, DWORD f) override {
        return inner_ ? inner_->SetInputType(id, t, f) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD id, IMFMediaType* t, DWORD f) override {
        return inner_ ? inner_->SetOutputType(id, t, f) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD id, IMFMediaType** t) override {
        return inner_ ? inner_->GetInputCurrentType(id, t) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD id, IMFMediaType** t) override {
        return inner_ ? inner_->GetOutputCurrentType(id, t) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD id, DWORD* f) override {
        return inner_ ? inner_->GetInputStatus(id, f) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetOutputStatus(DWORD* f) override {
        return inner_ ? inner_->GetOutputStatus(f) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE SetOutputBounds(LONGLONG a, LONGLONG b) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessEvent(DWORD id, IMFMediaEvent* ev) override {
        return inner_ ? inner_->ProcessEvent(id, ev) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ProcessMessage(MFT_MESSAGE_TYPE msg, ULONG_PTR p) override {
        if (!inner_) return MF_E_NOT_INITIALIZED;
        if (msg == MFT_MESSAGE_COMMAND_FLUSH) {
            std::lock_guard<std::mutex> lk(qmtx_);
            for (auto* e : queue_) e->Release();
            queue_.clear();
        }
        plog("ProcessMessage %d", (int)msg);
        return inner_->ProcessMessage(msg, p);
    }
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD id, IMFSample* s, DWORD f) override {
        if (!inner_) return MF_E_NOT_INITIALIZED;
        HRESULT hr = inner_->ProcessInput(id, s, f);
        plog("ProcessInput hr=%08lx", (unsigned long)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD f, DWORD n, MFT_OUTPUT_DATA_BUFFER* b, DWORD* st) override {
        if (!inner_) return MF_E_NOT_INITIALIZED;
        HRESULT hr = inner_->ProcessOutput(f, n, b, st);
        plog("ProcessOutput hr=%08lx", (unsigned long)hr);
        return hr;
    }

private:
    void pump_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_) {
            IMFMediaEvent* ev = nullptr;
            HRESULT hr = inner_events_ ? inner_events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev)
                                       : E_FAIL;
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(2);
                continue;
            }
            if (FAILED(hr)) {
                Sleep(5);
                continue;
            }
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            plog("inner event %d", (int)type);
            QueueEvent(type, GUID_NULL, S_OK, nullptr);
            ev->Release();
        }
    }

    void shutdown() {
        stop_ = true;
        qcv_.notify_all();
        if (pump_.joinable()) pump_.join();
        {
            std::lock_guard<std::mutex> lk(qmtx_);
            for (auto* e : queue_) e->Release();
            queue_.clear();
        }
        if (inner_events_) { inner_events_->Release(); inner_events_ = nullptr; }
        if (inner_) { inner_->Release(); inner_ = nullptr; }
        if (attrs_) { attrs_->Release(); attrs_ = nullptr; }
    }

    std::atomic<ULONG> ref_{1};
    IMFTransform* inner_ = nullptr;
    IMFMediaEventGenerator* inner_events_ = nullptr;
    IMFAttributes* attrs_ = nullptr;
    MFT_OUTPUT_STREAM_INFO osi_ = {};
    std::thread pump_;
    std::atomic<bool> stop_{false};
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::deque<IMFMediaEvent*> queue_;
    static bool mf_started_;
};

bool ProxyMFT::mf_started_ = false;

class ProxyClassFactory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new ProxyMFT();
        HRESULT hr = p->init();
        if (FAILED(hr)) {
            p->Release();
            return hr;
        }
        hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
};

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE
DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, kInnerClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* cf = new ProxyClassFactory();
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}
