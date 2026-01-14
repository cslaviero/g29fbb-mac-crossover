#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

extern "C" const IID IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}
};

static HMODULE g_real_dinput8 = NULL;
typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8CreateFn g_real_DirectInput8Create = NULL;

static CRITICAL_SECTION g_log_lock;
static int g_log_init = 0;
static int g_log_enabled = -1;
static ULONGLONG g_log_rate_ms = 0;
static ULONGLONG g_log_last_ms = 0;
static char g_proc_name[MAX_PATH] = {0};
static DWORD g_proc_pid = 0;
static ULONGLONG g_start_ms = 0;

static CRITICAL_SECTION g_udp_lock;
static int g_udp_init = 0;
static int g_udp_ready = 0;
static SOCKET g_udp_sock = INVALID_SOCKET;
static struct sockaddr_in g_udp_addr;

static void init_log() {
    if (g_log_init) return;
    g_log_init = 1;
    const char *env_log = getenv("FFB_LOG");
    if (env_log && (env_log[0] == '0' || env_log[0] == 'n' || env_log[0] == 'N' || env_log[0] == 'f' || env_log[0] == 'F')) {
        g_log_enabled = 0;
    } else {
        g_log_enabled = 1;
    }
    const char *env_rate = getenv("FFB_LOG_EVERY_MS");
    if (env_rate && env_rate[0] != '\0') {
        long rate = atol(env_rate);
        if (rate > 0) g_log_rate_ms = (ULONGLONG)rate;
    }
    if (!g_log_enabled) return;

    InitializeCriticalSection(&g_log_lock);
    g_proc_pid = GetCurrentProcessId();
    GetModuleFileNameA(NULL, g_proc_name, MAX_PATH);
    g_start_ms = GetTickCount64();
}

static void logf(const char *fmt, ...) {
    init_log();
    if (!g_log_enabled) return;

    ULONGLONG now = GetTickCount64();
    if (g_log_rate_ms > 0 && (now - g_log_last_ms) < g_log_rate_ms) {
        return;
    }

    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen("C:\\ac_ffb_proxy.log", "a");
    if (f) {
        g_log_last_ms = now;
        ULONGLONG delta = (g_start_ms == 0) ? 0 : (now - g_start_ms);
        fprintf(f, "[%llu ms][%lu:%s] ", (unsigned long long)delta, (unsigned long)g_proc_pid, g_proc_name);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void init_udp() {
    if (g_udp_init) return;
    InitializeCriticalSection(&g_udp_lock);
    g_udp_init = 1;

    const char *default_host = "127.0.0.1";
    int port = 21999;
    char host[64] = {0};
    char port_buf[16] = {0};

    DWORD host_len = GetEnvironmentVariableA("FFB_HOST", host, (DWORD)sizeof(host));
    if (host_len == 0 || host_len >= sizeof(host)) {
        strncpy(host, default_host, sizeof(host) - 1);
    }

    DWORD port_len = GetEnvironmentVariableA("FFB_PORT", port_buf, (DWORD)sizeof(port_buf));
    if (port_len > 0 && port_len < sizeof(port_buf)) {
        port = atoi(port_buf);
        if (port <= 0 || port > 65535) port = 21999;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logf("[proxy] UDP WSAStartup failed");
        g_udp_ready = 0;
        return;
    }

    g_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_sock == INVALID_SOCKET) {
        logf("[proxy] UDP socket failed: %d", WSAGetLastError());
        g_udp_ready = 0;
        return;
    }

    memset(&g_udp_addr, 0, sizeof(g_udp_addr));
    g_udp_addr.sin_family = AF_INET;
    g_udp_addr.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, host, &g_udp_addr.sin_addr) != 1) {
        logf("[proxy] UDP invalid host: %s", host);
        g_udp_ready = 0;
        return;
    }

    g_udp_ready = 1;
    logf("[proxy] UDP target %s:%d", host, port);
}

static void udp_send(const char *msg) {
    init_udp();
    if (!g_udp_ready || g_udp_sock == INVALID_SOCKET) return;

    EnterCriticalSection(&g_udp_lock);
    int len = (int)strlen(msg);
    int r = sendto(g_udp_sock, msg, len, 0, (const struct sockaddr *)&g_udp_addr, sizeof(g_udp_addr));
    LeaveCriticalSection(&g_udp_lock);
    if (r == SOCKET_ERROR) {
        logf("[proxy] UDP sendto failed: %d", WSAGetLastError());
    }
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void send_const_force(int magnitude) {
    int scaled = (magnitude * 100) / 10000;
    scaled = clamp_int(scaled, -100, 100);
    if (scaled == 0) {
        udp_send("STOP");
        return;
    }
    char msg[32];
    _snprintf(msg, sizeof(msg), "CONST %d", scaled);
    udp_send(msg);
}

static void send_stop() {
    udp_send("STOP");
}

static void guid_to_string(const GUID &g, char *out, size_t out_len) {
    _snprintf(
        out, out_len,
        "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]
    );
}

static void ensure_real_loaded() {
    if (g_real_dinput8) return;
    wchar_t sysdir[MAX_PATH] = {0};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    wchar_t path[MAX_PATH] = {0};
    _snwprintf(path, MAX_PATH, L"%s\\dinput8.dll", sysdir);
    g_real_dinput8 = LoadLibraryW(path);
    if (!g_real_dinput8) {
        logf("[proxy] failed to load real dinput8.dll");
        return;
    }
    g_real_DirectInput8Create = (DirectInput8CreateFn)GetProcAddress(g_real_dinput8, "DirectInput8Create");
    if (!g_real_DirectInput8Create) {
        logf("[proxy] failed to get DirectInput8Create");
    }
}

class DirectInputEffectProxy : public IDirectInputEffect {
public:
    DirectInputEffectProxy(IDirectInputEffect *real, const GUID &guid)
        : refCount(1), realEffect(real), effectGuid(guid), lastForce(0), hasForce(false) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInputEffect) {
            *ppv = static_cast<IDirectInputEffect *>(this);
            AddRef();
            return S_OK;
        }
        return realEffect->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        realEffect->AddRef();
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        realEffect->Release();
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid) override {
        return realEffect->Initialize(hinst, dwVersion, rguid);
    }

    STDMETHODIMP GetEffectGuid(LPGUID pguid) override {
        return realEffect->GetEffectGuid(pguid);
    }

    STDMETHODIMP GetParameters(LPDIEFFECT peff, DWORD dwFlags) override {
        return realEffect->GetParameters(peff, dwFlags);
    }

    STDMETHODIMP SetParameters(LPCDIEFFECT peff, DWORD dwFlags) override {
        if (peff) {
            char gbuf[64];
            guid_to_string(effectGuid, gbuf, sizeof(gbuf));
            logf("[proxy] SetParameters guid=%s flags=0x%08lx duration=%lu gain=%lu trigger=%lu delay=%lu axes=%lu cbType=%lu",
                 gbuf,
                 dwFlags,
                 peff->dwDuration,
                 peff->dwGain,
                 peff->dwTriggerButton,
                 peff->dwStartDelay,
                 peff->cAxes,
                 peff->cbTypeSpecificParams);

            if (peff->cAxes > 0 && peff->rglDirection) {
                logf("[proxy] SetParameters direction[0]=%ld", peff->rglDirection[0]);
            }

            if (IsEqualGUID(effectGuid, GUID_ConstantForce) && peff->lpvTypeSpecificParams) {
                const DICONSTANTFORCE *cf = (const DICONSTANTFORCE *)peff->lpvTypeSpecificParams;
                logf("[proxy] SetParameters ConstantForce magnitude=%ld", cf->lMagnitude);
                lastForce = (int)cf->lMagnitude;
                hasForce = true;
                send_const_force(lastForce);
            } else if (IsEqualGUID(effectGuid, GUID_RampForce) && peff->lpvTypeSpecificParams &&
                       peff->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
                const DIRAMPFORCE *rf = (const DIRAMPFORCE *)peff->lpvTypeSpecificParams;
                logf("[proxy] SetParameters RampForce start=%ld end=%ld", rf->lStart, rf->lEnd);
            } else if ((IsEqualGUID(effectGuid, GUID_Spring) || IsEqualGUID(effectGuid, GUID_Damper) ||
                        IsEqualGUID(effectGuid, GUID_Friction) || IsEqualGUID(effectGuid, GUID_Inertia)) &&
                       peff->lpvTypeSpecificParams && peff->cbTypeSpecificParams >= sizeof(DICONDITION)) {
                const DICONDITION *cond = (const DICONDITION *)peff->lpvTypeSpecificParams;
                logf("[proxy] SetParameters Condition posCoeff=%ld negCoeff=%ld posSat=%lu negSat=%lu dead=%ld",
                     cond->lPositiveCoefficient,
                     cond->lNegativeCoefficient,
                     cond->dwPositiveSaturation,
                     cond->dwNegativeSaturation,
                     cond->lDeadBand);
            } else if ((IsEqualGUID(effectGuid, GUID_Sine) || IsEqualGUID(effectGuid, GUID_Square) ||
                        IsEqualGUID(effectGuid, GUID_Triangle) || IsEqualGUID(effectGuid, GUID_SawtoothUp) ||
                        IsEqualGUID(effectGuid, GUID_SawtoothDown)) &&
                       peff->lpvTypeSpecificParams && peff->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
                const DIPERIODIC *per = (const DIPERIODIC *)peff->lpvTypeSpecificParams;
                logf("[proxy] SetParameters Periodic mag=%lu offset=%ld phase=%lu period=%lu",
                     per->dwMagnitude, per->lOffset, per->dwPhase, per->dwPeriod);
            }
        }
        HRESULT hr = realEffect->SetParameters(peff, dwFlags);
        logf("[proxy] SetParameters -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }

    STDMETHODIMP Start(DWORD dwIterations, DWORD dwFlags) override {
        logf("[proxy] Effect Start iterations=%lu flags=0x%08lx", dwIterations, dwFlags);
        if (IsEqualGUID(effectGuid, GUID_ConstantForce) && hasForce) {
            send_const_force(lastForce);
        }
        HRESULT hr = realEffect->Start(dwIterations, dwFlags);
        logf("[proxy] Effect Start -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }

    STDMETHODIMP Stop() override {
        logf("[proxy] Effect Stop");
        if (IsEqualGUID(effectGuid, GUID_ConstantForce)) {
            send_stop();
        }
        HRESULT hr = realEffect->Stop();
        logf("[proxy] Effect Stop -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }

    STDMETHODIMP GetEffectStatus(LPDWORD pdwFlags) override {
        return realEffect->GetEffectStatus(pdwFlags);
    }

    STDMETHODIMP Download() override {
        HRESULT hr = realEffect->Download();
        logf("[proxy] Effect Download -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }

    STDMETHODIMP Unload() override {
        HRESULT hr = realEffect->Unload();
        logf("[proxy] Effect Unload -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }

    STDMETHODIMP Escape(LPDIEFFESCAPE pesc) override {
        return realEffect->Escape(pesc);
    }

private:
    LONG refCount;
    IDirectInputEffect *realEffect;
    GUID effectGuid;
    int lastForce;
    bool hasForce;
};

class DirectInputEffectFake : public IDirectInputEffect {
public:
    DirectInputEffectFake(const GUID &guid) : refCount(1), effectGuid(guid), lastForce(0), hasForce(false) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInputEffect) {
            *ppv = static_cast<IDirectInputEffect *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid) override {
        (void)hinst; (void)dwVersion; (void)rguid;
        logf("[proxy] FakeEffect Initialize");
        return DI_OK;
    }

    STDMETHODIMP GetEffectGuid(LPGUID pguid) override {
        if (!pguid) return E_POINTER;
        *pguid = effectGuid;
        return DI_OK;
    }

    STDMETHODIMP GetParameters(LPDIEFFECT peff, DWORD dwFlags) override {
        (void)dwFlags;
        logf("[proxy] FakeEffect GetParameters");
        if (!peff) return E_POINTER;
        return DI_OK;
    }

    STDMETHODIMP SetParameters(LPCDIEFFECT peff, DWORD dwFlags) override {
        if (peff) {
            char gbuf[64];
            guid_to_string(effectGuid, gbuf, sizeof(gbuf));
            logf("[proxy] FakeEffect SetParameters guid=%s flags=0x%08lx duration=%lu gain=%lu trigger=%lu delay=%lu axes=%lu cbType=%lu",
                 gbuf,
                 dwFlags,
                 peff->dwDuration,
                 peff->dwGain,
                 peff->dwTriggerButton,
                 peff->dwStartDelay,
                 peff->cAxes,
                 peff->cbTypeSpecificParams);

            if (peff->cAxes > 0 && peff->rglDirection) {
                logf("[proxy] FakeEffect SetParameters direction[0]=%ld", peff->rglDirection[0]);
            }

            if (IsEqualGUID(effectGuid, GUID_ConstantForce) && peff->lpvTypeSpecificParams) {
                const DICONSTANTFORCE *cf = (const DICONSTANTFORCE *)peff->lpvTypeSpecificParams;
                logf("[proxy] FakeEffect SetParameters ConstantForce magnitude=%ld", cf->lMagnitude);
                lastForce = (int)cf->lMagnitude;
                hasForce = true;
                send_const_force(lastForce);
            } else if (IsEqualGUID(effectGuid, GUID_RampForce) && peff->lpvTypeSpecificParams &&
                       peff->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
                const DIRAMPFORCE *rf = (const DIRAMPFORCE *)peff->lpvTypeSpecificParams;
                logf("[proxy] FakeEffect SetParameters RampForce start=%ld end=%ld", rf->lStart, rf->lEnd);
            } else if ((IsEqualGUID(effectGuid, GUID_Spring) || IsEqualGUID(effectGuid, GUID_Damper) ||
                        IsEqualGUID(effectGuid, GUID_Friction) || IsEqualGUID(effectGuid, GUID_Inertia)) &&
                       peff->lpvTypeSpecificParams && peff->cbTypeSpecificParams >= sizeof(DICONDITION)) {
                const DICONDITION *cond = (const DICONDITION *)peff->lpvTypeSpecificParams;
                logf("[proxy] FakeEffect SetParameters Condition posCoeff=%ld negCoeff=%ld posSat=%lu negSat=%lu dead=%ld",
                     cond->lPositiveCoefficient,
                     cond->lNegativeCoefficient,
                     cond->dwPositiveSaturation,
                     cond->dwNegativeSaturation,
                     cond->lDeadBand);
            } else if ((IsEqualGUID(effectGuid, GUID_Sine) || IsEqualGUID(effectGuid, GUID_Square) ||
                        IsEqualGUID(effectGuid, GUID_Triangle) || IsEqualGUID(effectGuid, GUID_SawtoothUp) ||
                        IsEqualGUID(effectGuid, GUID_SawtoothDown)) &&
                       peff->lpvTypeSpecificParams && peff->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
                const DIPERIODIC *per = (const DIPERIODIC *)peff->lpvTypeSpecificParams;
                logf("[proxy] FakeEffect SetParameters Periodic mag=%lu offset=%ld phase=%lu period=%lu",
                     per->dwMagnitude, per->lOffset, per->dwPhase, per->dwPeriod);
            }
        }
        return DI_OK;
    }

    STDMETHODIMP Start(DWORD dwIterations, DWORD dwFlags) override {
        logf("[proxy] FakeEffect Start iterations=%lu flags=0x%08lx", dwIterations, dwFlags);
        if (IsEqualGUID(effectGuid, GUID_ConstantForce) && hasForce) {
            send_const_force(lastForce);
        }
        return DI_OK;
    }

    STDMETHODIMP Stop() override {
        logf("[proxy] FakeEffect Stop");
        if (IsEqualGUID(effectGuid, GUID_ConstantForce)) {
            send_stop();
        }
        return DI_OK;
    }

    STDMETHODIMP GetEffectStatus(LPDWORD pdwFlags) override {
        if (!pdwFlags) return E_POINTER;
        *pdwFlags = 0;
        return DI_OK;
    }

    STDMETHODIMP Download() override {
        logf("[proxy] FakeEffect Download");
        return DI_OK;
    }

    STDMETHODIMP Unload() override {
        logf("[proxy] FakeEffect Unload");
        return DI_OK;
    }

    STDMETHODIMP Escape(LPDIEFFESCAPE pesc) override {
        (void)pesc;
        logf("[proxy] FakeEffect Escape");
        return DIERR_UNSUPPORTED;
    }

private:
    LONG refCount;
    GUID effectGuid;
    int lastForce;
    bool hasForce;
};

class DirectInputDevice8ProxyA;
class DirectInput8ProxyA;

class DirectInputDevice8ProxyW : public IDirectInputDevice8W {
public:
    DirectInputDevice8ProxyW(IDirectInputDevice8W *real) : refCount(1), realDev(real) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == IID_IDirectInputDevice8W ||
            riid == IID_IDirectInputDevice7W ||
            riid == IID_IDirectInputDevice2W ||
            riid == IID_IDirectInputDeviceW) {
            char gbuf[64];
            guid_to_string(riid, gbuf, sizeof(gbuf));
            logf("[proxy] Device QI (W) riid=%s -> wrapper", gbuf);
            *ppv = static_cast<IDirectInputDevice8W *>(this);
            AddRef();
            return S_OK;
        }
        return realDev->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        realDev->AddRef();
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        realDev->Release();
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override {
        HRESULT hr = realDev->GetCapabilities(caps);
        if (SUCCEEDED(hr) && caps) {
            caps->dwFlags |= (DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE);
            logf("[proxy] GetCapabilities -> force feedback enabled");
        } else if (FAILED(hr)) {
            logf("[proxy] GetCapabilities -> hr=0x%08lx", (unsigned long)hr);
        }
        return hr;
    }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID pvRef, DWORD dwFlags) override { return realDev->EnumObjects(cb, pvRef, dwFlags); }
    STDMETHODIMP GetProperty(REFGUID rguid, LPDIPROPHEADER ph) override { return realDev->GetProperty(rguid, ph); }
    STDMETHODIMP SetProperty(REFGUID rguid, LPCDIPROPHEADER ph) override { return realDev->SetProperty(rguid, ph); }
    STDMETHODIMP Acquire() override {
        logf("[proxy] Acquire");
        HRESULT hr = realDev->Acquire();
        logf("[proxy] Acquire -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP Unacquire() override {
        logf("[proxy] Unacquire");
        HRESULT hr = realDev->Unacquire();
        logf("[proxy] Unacquire -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP GetDeviceState(DWORD cbData, LPVOID lpvData) override { return realDev->GetDeviceState(cbData, lpvData); }
    STDMETHODIMP GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override {
        return realDev->GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags);
    }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT lpdf) override {
        logf("[proxy] SetDataFormat");
        if (lpdf) {
            logf("[proxy] SetDataFormat size=%lu data=%lu objs=%lu flags=0x%08lx",
                 lpdf->dwSize, lpdf->dwDataSize, lpdf->dwNumObjs, lpdf->dwFlags);
        }
        HRESULT hr = realDev->SetDataFormat(lpdf);
        logf("[proxy] SetDataFormat -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP SetEventNotification(HANDLE hEvent) override { return realDev->SetEventNotification(hEvent); }
    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD dwFlags) override {
        logf("[proxy] SetCooperativeLevel flags=0x%08lx", dwFlags);
        HRESULT hr = realDev->SetCooperativeLevel(hwnd, dwFlags);
        logf("[proxy] SetCooperativeLevel -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW pdidoi, DWORD dwObj, DWORD dwHow) override {
        return realDev->GetObjectInfo(pdidoi, dwObj, dwHow);
    }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEW pdidi) override {
        HRESULT hr = realDev->GetDeviceInfo(pdidi);
        if (SUCCEEDED(hr) && pdidi) {
            char gprod[64];
            char ginst[64];
            guid_to_string(pdidi->guidProduct, gprod, sizeof(gprod));
            guid_to_string(pdidi->guidInstance, ginst, sizeof(ginst));
            logf("[proxy] GetDeviceInfo devType=0x%08lx guidProduct=%s guidInstance=%s",
                 pdidi->dwDevType, gprod, ginst);
        } else if (FAILED(hr)) {
            logf("[proxy] GetDeviceInfo -> hr=0x%08lx", (unsigned long)hr);
        }
        return hr;
    }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return realDev->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid) override { return realDev->Initialize(hinst, dwVersion, rguid); }

    STDMETHODIMP CreateEffect(REFGUID rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT *ppdeff, LPUNKNOWN pUnkOuter) override {
        char gbuf[64];
        guid_to_string(rguid, gbuf, sizeof(gbuf));
        logf("[proxy] CreateEffect guid=%s", gbuf);
        HRESULT hr = realDev->CreateEffect(rguid, lpeff, ppdeff, pUnkOuter);
        logf("[proxy] CreateEffect -> hr=0x%08lx", (unsigned long)hr);
        if ((hr == DIERR_UNSUPPORTED || hr == E_NOTIMPL) && ppdeff) {
            logf("[proxy] CreateEffect unsupported; using fake effect");
            *ppdeff = new DirectInputEffectFake(rguid);
            return DI_OK;
        }
        if (SUCCEEDED(hr) && ppdeff && *ppdeff) {
            IDirectInputEffect *realEff = *ppdeff;
            *ppdeff = new DirectInputEffectProxy(realEff, rguid);
        }
        return hr;
    }

    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKW cb, LPVOID pvRef, DWORD dwEffType) override {
        struct Ctx {
            LPDIENUMEFFECTSCALLBACKW cb;
            LPVOID pvRef;
        } ctx = { cb, pvRef };

        auto thunk = [](LPCDIEFFECTINFOW info, LPVOID ref) -> BOOL {
            Ctx *c = (Ctx *)ref;
            if (info) {
                char gbuf[64];
                guid_to_string(info->guid, gbuf, sizeof(gbuf));
                logf("[proxy] EnumEffects (W) guid=%s type=0x%08lx static=0x%08lx dynamic=0x%08lx",
                     gbuf, info->dwEffType, info->dwStaticParams, info->dwDynamicParams);
            }
            return c->cb ? c->cb(info, c->pvRef) : DIENUM_CONTINUE;
        };

        return realDev->EnumEffects(thunk, &ctx, dwEffType);
    }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOW pei, REFGUID rguid) override { return realDev->GetEffectInfo(pei, rguid); }
    STDMETHODIMP GetForceFeedbackState(LPDWORD pdwOut) override { return realDev->GetForceFeedbackState(pdwOut); }
    STDMETHODIMP SendForceFeedbackCommand(DWORD dwFlags) override {
        logf("[proxy] SendForceFeedbackCommand flags=0x%08lx", dwFlags);
        HRESULT hr = realDev->SendForceFeedbackCommand(dwFlags);
        logf("[proxy] SendForceFeedbackCommand -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID pvRef, DWORD fl) override {
        return realDev->EnumCreatedEffectObjects(cb, pvRef, fl);
    }
    STDMETHODIMP Escape(LPDIEFFESCAPE pesc) override { return realDev->Escape(pesc); }
    STDMETHODIMP Poll() override { return realDev->Poll(); }
    STDMETHODIMP SendDeviceData(DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override {
        return realDev->SendDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags);
    }
    STDMETHODIMP EnumEffectsInFile(LPCWSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID pvRef, DWORD dwFlags) override {
        return realDev->EnumEffectsInFile(lpszFileName, cb, pvRef, dwFlags);
    }
    STDMETHODIMP WriteEffectToFile(LPCWSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFile, DWORD dwFlags) override {
        return realDev->WriteEffectToFile(lpszFileName, dwEntries, rgDiFile, dwFlags);
    }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATW lpdiaf, LPCWSTR lpszUserName, DWORD dwFlags) override {
        return realDev->BuildActionMap(lpdiaf, lpszUserName, dwFlags);
    }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATW lpdiaf, LPCWSTR lpszUserName, DWORD dwFlags) override {
        return realDev->SetActionMap(lpdiaf, lpszUserName, dwFlags);
    }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW lpdidi) override { return realDev->GetImageInfo(lpdidi); }

private:
    LONG refCount;
    IDirectInputDevice8W *realDev;
};

class DirectInputDevice8ProxyA : public IDirectInputDevice8A {
public:
    DirectInputDevice8ProxyA(IDirectInputDevice8A *real) : refCount(1), realDev(real) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == IID_IDirectInputDevice8A ||
            riid == IID_IDirectInputDevice7A ||
            riid == IID_IDirectInputDevice2A ||
            riid == IID_IDirectInputDeviceA) {
            char gbuf[64];
            guid_to_string(riid, gbuf, sizeof(gbuf));
            logf("[proxy] Device QI (A) riid=%s -> wrapper", gbuf);
            *ppv = static_cast<IDirectInputDevice8A *>(this);
            AddRef();
            return S_OK;
        }
        return realDev->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        realDev->AddRef();
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        realDev->Release();
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override {
        HRESULT hr = realDev->GetCapabilities(caps);
        if (SUCCEEDED(hr) && caps) {
            caps->dwFlags |= (DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE);
            logf("[proxy] GetCapabilities -> force feedback enabled");
        } else if (FAILED(hr)) {
            logf("[proxy] GetCapabilities -> hr=0x%08lx", (unsigned long)hr);
        }
        return hr;
    }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID pvRef, DWORD dwFlags) override { return realDev->EnumObjects(cb, pvRef, dwFlags); }
    STDMETHODIMP GetProperty(REFGUID rguid, LPDIPROPHEADER ph) override { return realDev->GetProperty(rguid, ph); }
    STDMETHODIMP SetProperty(REFGUID rguid, LPCDIPROPHEADER ph) override { return realDev->SetProperty(rguid, ph); }
    STDMETHODIMP Acquire() override {
        logf("[proxy] Acquire");
        HRESULT hr = realDev->Acquire();
        logf("[proxy] Acquire -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP Unacquire() override {
        logf("[proxy] Unacquire");
        HRESULT hr = realDev->Unacquire();
        logf("[proxy] Unacquire -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP GetDeviceState(DWORD cbData, LPVOID lpvData) override { return realDev->GetDeviceState(cbData, lpvData); }
    STDMETHODIMP GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override {
        return realDev->GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags);
    }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT lpdf) override {
        logf("[proxy] SetDataFormat");
        if (lpdf) {
            logf("[proxy] SetDataFormat size=%lu data=%lu objs=%lu flags=0x%08lx",
                 lpdf->dwSize, lpdf->dwDataSize, lpdf->dwNumObjs, lpdf->dwFlags);
        }
        HRESULT hr = realDev->SetDataFormat(lpdf);
        logf("[proxy] SetDataFormat -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP SetEventNotification(HANDLE hEvent) override { return realDev->SetEventNotification(hEvent); }
    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD dwFlags) override {
        logf("[proxy] SetCooperativeLevel flags=0x%08lx", dwFlags);
        HRESULT hr = realDev->SetCooperativeLevel(hwnd, dwFlags);
        logf("[proxy] SetCooperativeLevel -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA pdidoi, DWORD dwObj, DWORD dwHow) override {
        return realDev->GetObjectInfo(pdidoi, dwObj, dwHow);
    }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA pdidi) override {
        HRESULT hr = realDev->GetDeviceInfo(pdidi);
        if (SUCCEEDED(hr) && pdidi) {
            char gprod[64];
            char ginst[64];
            guid_to_string(pdidi->guidProduct, gprod, sizeof(gprod));
            guid_to_string(pdidi->guidInstance, ginst, sizeof(ginst));
            logf("[proxy] GetDeviceInfo devType=0x%08lx guidProduct=%s guidInstance=%s",
                 pdidi->dwDevType, gprod, ginst);
        } else if (FAILED(hr)) {
            logf("[proxy] GetDeviceInfo -> hr=0x%08lx", (unsigned long)hr);
        }
        return hr;
    }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return realDev->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid) override { return realDev->Initialize(hinst, dwVersion, rguid); }

    STDMETHODIMP CreateEffect(REFGUID rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT *ppdeff, LPUNKNOWN pUnkOuter) override {
        char gbuf[64];
        guid_to_string(rguid, gbuf, sizeof(gbuf));
        logf("[proxy] CreateEffect guid=%s", gbuf);
        HRESULT hr = realDev->CreateEffect(rguid, lpeff, ppdeff, pUnkOuter);
        logf("[proxy] CreateEffect -> hr=0x%08lx", (unsigned long)hr);
        if ((hr == DIERR_UNSUPPORTED || hr == E_NOTIMPL) && ppdeff) {
            logf("[proxy] CreateEffect unsupported; using fake effect");
            *ppdeff = new DirectInputEffectFake(rguid);
            return DI_OK;
        }
        if (SUCCEEDED(hr) && ppdeff && *ppdeff) {
            IDirectInputEffect *realEff = *ppdeff;
            *ppdeff = new DirectInputEffectProxy(realEff, rguid);
        }
        return hr;
    }

    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID pvRef, DWORD dwEffType) override {
        struct Ctx {
            LPDIENUMEFFECTSCALLBACKA cb;
            LPVOID pvRef;
        } ctx = { cb, pvRef };

        auto thunk = [](LPCDIEFFECTINFOA info, LPVOID ref) -> BOOL {
            Ctx *c = (Ctx *)ref;
            if (info) {
                char gbuf[64];
                guid_to_string(info->guid, gbuf, sizeof(gbuf));
                logf("[proxy] EnumEffects (A) guid=%s type=0x%08lx static=0x%08lx dynamic=0x%08lx",
                     gbuf, info->dwEffType, info->dwStaticParams, info->dwDynamicParams);
            }
            return c->cb ? c->cb(info, c->pvRef) : DIENUM_CONTINUE;
        };

        return realDev->EnumEffects(thunk, &ctx, dwEffType);
    }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA pei, REFGUID rguid) override { return realDev->GetEffectInfo(pei, rguid); }
    STDMETHODIMP GetForceFeedbackState(LPDWORD pdwOut) override { return realDev->GetForceFeedbackState(pdwOut); }
    STDMETHODIMP SendForceFeedbackCommand(DWORD dwFlags) override {
        logf("[proxy] SendForceFeedbackCommand flags=0x%08lx", dwFlags);
        HRESULT hr = realDev->SendForceFeedbackCommand(dwFlags);
        logf("[proxy] SendForceFeedbackCommand -> hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID pvRef, DWORD fl) override {
        return realDev->EnumCreatedEffectObjects(cb, pvRef, fl);
    }
    STDMETHODIMP Escape(LPDIEFFESCAPE pesc) override { return realDev->Escape(pesc); }
    STDMETHODIMP Poll() override { return realDev->Poll(); }
    STDMETHODIMP SendDeviceData(DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override {
        return realDev->SendDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags);
    }
    STDMETHODIMP EnumEffectsInFile(LPCSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID pvRef, DWORD dwFlags) override {
        return realDev->EnumEffectsInFile(lpszFileName, cb, pvRef, dwFlags);
    }
    STDMETHODIMP WriteEffectToFile(LPCSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFile, DWORD dwFlags) override {
        return realDev->WriteEffectToFile(lpszFileName, dwEntries, rgDiFile, dwFlags);
    }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags) override {
        return realDev->BuildActionMap(lpdiaf, lpszUserName, dwFlags);
    }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags) override {
        return realDev->SetActionMap(lpdiaf, lpszUserName, dwFlags);
    }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA lpdidi) override { return realDev->GetImageInfo(lpdidi); }

private:
    LONG refCount;
    IDirectInputDevice8A *realDev;
};

class DirectInput8ProxyW : public IDirectInput8W {
public:
    DirectInput8ProxyW(IDirectInput8W *real) : refCount(1), realDI(real) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInput8W || riid == IID_IDirectInput8) {
            *ppv = static_cast<IDirectInput8W *>(this);
            AddRef();
            return S_OK;
        }
        return realDI->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        realDI->AddRef();
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        realDI->Release();
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8W *lplpDirectInputDevice, LPUNKNOWN pUnkOuter) override {
        char gbuf[64];
        guid_to_string(rguid, gbuf, sizeof(gbuf));
        logf("[proxy] CreateDevice (W) guid=%s", gbuf);
        HRESULT hr = realDI->CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter);
        logf("[proxy] CreateDevice (W) -> hr=0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice) {
            *lplpDirectInputDevice = new DirectInputDevice8ProxyW(*lplpDirectInputDevice);
        }
        return hr;
    }

    STDMETHODIMP EnumDevices(DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags) override {
        return realDI->EnumDevices(dwDevType, lpCallback, pvRef, dwFlags);
    }
    STDMETHODIMP GetDeviceStatus(REFGUID rguidInstance) override { return realDI->GetDeviceStatus(rguidInstance); }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return realDI->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion) override { return realDI->Initialize(hinst, dwVersion); }
    STDMETHODIMP FindDevice(REFGUID rguidClass, LPCWSTR ptszName, LPGUID pguidInstance) override {
        return realDI->FindDevice(rguidClass, ptszName, pguidInstance);
    }
    STDMETHODIMP EnumDevicesBySemantics(LPCWSTR ptszUserName, LPDIACTIONFORMATW lpdiActionFormat,
                                       LPDIENUMDEVICESBYSEMANTICSCBW lpCallback, LPVOID pvRef, DWORD dwFlags) override {
        return realDI->EnumDevicesBySemantics(ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags);
    }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSW lpdiCDParams,
                                  DWORD dwFlags, LPVOID pvRefData) override {
        return realDI->ConfigureDevices(lpdiCallback, lpdiCDParams, dwFlags, pvRefData);
    }

private:
    LONG refCount;
    IDirectInput8W *realDI;
};

class DirectInput8ProxyA : public IDirectInput8A {
public:
    DirectInput8ProxyA(IDirectInput8A *real) : refCount(1), realDI(real) {}

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectInput8A || riid == IID_IDirectInput8) {
            *ppv = static_cast<IDirectInput8A *>(this);
            AddRef();
            return S_OK;
        }
        return realDI->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        realDI->AddRef();
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        realDI->Release();
        ULONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice, LPUNKNOWN pUnkOuter) override {
        char gbuf[64];
        guid_to_string(rguid, gbuf, sizeof(gbuf));
        logf("[proxy] CreateDevice (A) guid=%s", gbuf);
        HRESULT hr = realDI->CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter);
        logf("[proxy] CreateDevice (A) -> hr=0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice) {
            *lplpDirectInputDevice = new DirectInputDevice8ProxyA(*lplpDirectInputDevice);
        }
        return hr;
    }
    STDMETHODIMP EnumDevices(DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) override {
        return realDI->EnumDevices(dwDevType, lpCallback, pvRef, dwFlags);
    }
    STDMETHODIMP GetDeviceStatus(REFGUID rguidInstance) override { return realDI->GetDeviceStatus(rguidInstance); }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return realDI->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion) override { return realDI->Initialize(hinst, dwVersion); }
    STDMETHODIMP FindDevice(REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance) override {
        return realDI->FindDevice(rguidClass, ptszName, pguidInstance);
    }
    STDMETHODIMP EnumDevicesBySemantics(LPCSTR ptszUserName, LPDIACTIONFORMATA lpdiActionFormat,
                                       LPDIENUMDEVICESBYSEMANTICSCBA lpCallback, LPVOID pvRef, DWORD dwFlags) override {
        return realDI->EnumDevicesBySemantics(ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags);
    }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSA lpdiCDParams,
                                  DWORD dwFlags, LPVOID pvRefData) override {
        return realDI->ConfigureDevices(lpdiCallback, lpdiCDParams, dwFlags, pvRefData);
    }

private:
    LONG refCount;
    IDirectInput8A *realDI;
};

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID *ppvOut, LPUNKNOWN punkOuter) {
    ensure_real_loaded();
    if (!g_real_DirectInput8Create) return E_FAIL;

    logf("[proxy] DirectInput8Create called");
    HRESULT hr = g_real_DirectInput8Create(hinst, dwVersion, riid, ppvOut, punkOuter);
    if (FAILED(hr) || !ppvOut || !*ppvOut) return hr;

    if (riid == IID_IDirectInput8W || riid == IID_IDirectInput8) {
        IDirectInput8W *real = (IDirectInput8W *)*ppvOut;
        *ppvOut = new DirectInput8ProxyW(real);
        logf("[proxy] Wrapped IDirectInput8W");
    } else if (riid == IID_IDirectInput8A) {
        IDirectInput8A *real = (IDirectInput8A *)*ppvOut;
        *ppvOut = new DirectInput8ProxyA(real);
        logf("[proxy] Wrapped IDirectInput8A");
    } else {
        logf("[proxy] Unknown riid, returning real interface");
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
