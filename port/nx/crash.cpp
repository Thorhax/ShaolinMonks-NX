// A CPU exception on any thread: the registers and a backtrace go to logs/crash.log as offsets into the NRO's
// code (look them up in the linker map or with addr2line on the .elf), then the process aborts, so Atmosphère
// still writes its crash report. An abort, a failed assert or an uncaught C++
// exception write the reason and the caller there too.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#include "runtime_paths.h"


namespace logx {
void FlushUnlocked();
}

extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
// also when no debugger is attached
u32 __nx_exception_ignoredebug = 1;
}

namespace {

char g_path[512];
bool g_inside = false;

u64 g_code = 0, g_code_end = 0;

bool InCode(u64 a) { return a >= g_code && a < g_code_end; }

// where the NRO's code is, for an address outside it: the module and offset are what Atmosphère reports too
void Address(FILE* f, const char* name, u64 a) {
    if (InCode(a)) std::fprintf(f, "%-4s %016llx  nro+%llx\n", name, (unsigned long long)a, (unsigned long long)(a - g_code));
    else std::fprintf(f, "%-4s %016llx\n", name, (unsigned long long)a);
}

bool Readable(u64 a) {
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, a))) return false;
    return (info.perm & Perm_R) && info.type != MemType_Unmapped && a + 16 <= info.addr + info.size;
}

// the reason a thread gave up, with the code that called in, then Atmosphère's report
[[noreturn]] void GiveUp(const char* reason, const char* detail, u64 caller) {
    if (!g_inside && g_path[0]) {
        g_inside = true;
        logx::FlushUnlocked();
        if (FILE* f = std::fopen(g_path, "w")) {
            std::fprintf(f, "Shaolin Monks NX stopped: %s\n%s\nnro base %p\n\n", reason, detail ? detail : "",
                         reinterpret_cast<void*>(g_code));
            Address(f, "from", caller);
            // the caller's callers, from the frame records
            std::fprintf(f, "\nframes:\n");
            u64 fp = u64(__builtin_frame_address(0));
            for (int depth = 0; depth < 64 && fp && !(fp & 7) && Readable(fp); depth++) {
                const u64* rec = reinterpret_cast<const u64*>(fp);
                Address(f, "ret", rec[1]);
                if (rec[0] <= fp) break;
                fp = rec[0];
            }
            std::fclose(f);
        }
    }
    diagAbortWithResult(MAKERESULT(Module_HomebrewAbi, 1));
}

void Terminate() {
    const char* what = "no exception";
    char text[512];
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            std::snprintf(text, sizeof(text), "%s: %s", typeid(x).name(), x.what());
            what = text;
        } catch (...) {
            what = "an exception of another type";
        }
    }
    GiveUp("std::terminate", what, u64(__builtin_return_address(0)));
}

}  // namespace

namespace nx {
// called once the log folder exists
void InstallCrashLog() {
    std::snprintf(g_path, sizeof(g_path), "%s/crash.log", shaolin::paths::Logs().string().c_str());
    // the NRO's code is one mapping, starting at the module base the linker map's addresses are relative to
    MemoryInfo info;
    u32 page;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<u64>(&InstallCrashLog)))) {
        g_code = info.addr;
        g_code_end = info.addr + info.size;
    }
    std::set_terminate(Terminate);
}
}  // namespace nx

// newlib's, which libnx ends with an svcBreak that leaves no trace in the log
extern "C" void __assert_func(const char* file, int line, const char* func, const char* expr) {
    char text[512];
    std::snprintf(text, sizeof(text), "%s:%d %s: %s", file, line, func ? func : "", expr);
    GiveUp("assert", text, u64(__builtin_return_address(0)));
}

extern "C" void __wrap_abort() { GiveUp("abort", nullptr, u64(__builtin_return_address(0))); }

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    if (!g_inside && g_path[0]) {
        g_inside = true;
        logx::FlushUnlocked();
        if (FILE* f = std::fopen(g_path, "w")) {
            std::fprintf(f, "Shaolin Monks NX crash\nexception %X esr %08X  thread %08X\nnro base %p\n\n", ctx->error_desc,
                         ctx->esr, (unsigned)threadGetCurHandle(), reinterpret_cast<void*>(g_code));
            Address(f, "pc", ctx->pc.x);
            Address(f, "lr", ctx->lr.x);
            Address(f, "far", ctx->far.x);
            Address(f, "sp", ctx->sp.x);
            Address(f, "fp", ctx->fp.x);
            for (int i = 0; i < 29; i++) {
                char name[8];
                std::snprintf(name, sizeof(name), "x%d", i);
                Address(f, name, ctx->cpu_gprs[i].x);
            }
            // the frame records, while they lead somewhere readable
            std::fprintf(f, "\nframes:\n");
            u64 fp = ctx->fp.x;
            for (int depth = 0; depth < 64 && fp && !(fp & 7) && Readable(fp); depth++) {
                const u64* rec = reinterpret_cast<const u64*>(fp);
                Address(f, "ret", rec[1]);
                if (rec[0] <= fp) break;
                fp = rec[0];
            }
            // and any code address left on the stack, for frames built without a frame record
            std::fprintf(f, "\nstack:\n");
            const u64 sp = ctx->sp.x & ~u64(7);
            for (u64 a = sp; a < sp + 0x2000 && Readable(a); a += 8) {
                const u64 v = *reinterpret_cast<const u64*>(a);
                if (InCode(v)) std::fprintf(f, "sp+%04llx  nro+%llx\n", (unsigned long long)(a - sp), (unsigned long long)(v - g_code));
            }
            std::fclose(f);
        }
    }
    // returning would run the faulting instruction again, forever: end the process, which Atmosphère reports
    diagAbortWithResult(MAKERESULT(Module_HomebrewAbi, 2));
}
