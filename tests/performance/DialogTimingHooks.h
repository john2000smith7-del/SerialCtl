#pragma once
// Diagnostic builds only. No disk I/O, additional threads, or message pumping.
#include <windows.h>
#include <array>
#include <cstdio>
namespace serialctl { namespace dialog_timing {
enum Stage { Init, Configure, FieldStyle, ComboPopup, ComboEdit, Layout,
             ComboPaint, CheckPaint, Rounded, ChildPaint, Viewport, Count };
inline constexpr const wchar_t *Names[] = {L"Init",L"Configure",L"FieldStyle",L"ComboPopup",L"ComboEdit",L"Layout",
    L"ComboPaint",L"CheckPaint",L"Rounded",L"ChildPaint",L"Viewport"};
struct Totals { LONGLONG ticks = 0, own = 0; unsigned calls = 0; };
struct Trace { HWND window = nullptr; unsigned generation = 0; std::array<Totals,Count> totals{}; };
inline thread_local Trace trace;
class Scope;
inline thread_local Scope *current = nullptr;
class Scope {
    Stage stage_; unsigned generation_; LONGLONG start_ = 0, children_ = 0; Scope *parent_ = nullptr;
public:
    explicit Scope(Stage stage) : stage_(stage), generation_(trace.generation) {
        if (!trace.window) return;
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q); start_ = q.QuadPart;
        parent_ = current; current = this;
    }
    ~Scope() {
        if (!start_) return;
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q); const auto elapsed = q.QuadPart-start_;
        current = parent_;
        if (generation_ != trace.generation) return;
        auto &total=trace.totals[stage_]; total.ticks+=elapsed; total.own+=elapsed-children_; ++total.calls;
        if (parent_) parent_->children_+=elapsed;
    }
};
inline void Begin(HWND window) { trace.window=window; ++trace.generation; trace.totals={}; }
inline void Publish(HWND window) {
    if (trace.window!=window) return;
    for (unsigned i=0;i<Count;++i) {
        wchar_t key[96]{};
        swprintf_s(key,L"SerialCtl.Profile.%s.Ticks",Names[i]);
        SetPropW(window,key,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(trace.totals[i].ticks)));
        swprintf_s(key,L"SerialCtl.Profile.%s.Own",Names[i]);
        SetPropW(window,key,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(trace.totals[i].own)));
        swprintf_s(key,L"SerialCtl.Profile.%s.Calls",Names[i]);
        SetPropW(window,key,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(trace.totals[i].calls)));
    }
    trace.window=nullptr;
}
inline void Dump(HWND window,const char *name,int sample) {
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (unsigned i=0;i<Count;++i) {
        wchar_t key[96]{};
        swprintf_s(key,L"SerialCtl.Profile.%s.Ticks",Names[i]);
        const auto ticks=reinterpret_cast<UINT_PTR>(GetPropW(window,key));
        swprintf_s(key,L"SerialCtl.Profile.%s.Own",Names[i]);
        const auto own=reinterpret_cast<UINT_PTR>(GetPropW(window,key));
        swprintf_s(key,L"SerialCtl.Profile.%s.Calls",Names[i]);
        const auto calls=reinterpret_cast<UINT_PTR>(GetPropW(window,key));
        char stage[64]{}; WideCharToMultiByte(CP_UTF8,0,Names[i],-1,stage,64,nullptr,nullptr);
        printf("PROFILE,%s,%d,stage=%s,total_us=%.3f,own_us=%.3f,calls=%llu\n",name,sample,stage,
            ticks*1000000.0/frequency.QuadPart,own*1000000.0/frequency.QuadPart,static_cast<unsigned long long>(calls));
    }
}
}}
#define SERIALCTL_DIALOG_TRACE_SCOPE(stage) ::serialctl::dialog_timing::Scope dialogTraceScope{::serialctl::dialog_timing::stage}
#define SERIALCTL_DIALOG_TRACE_BEGIN(window) ::serialctl::dialog_timing::Begin(window)
#define SERIALCTL_DIALOG_TRACE_PUBLISH(window) ::serialctl::dialog_timing::Publish(window)
