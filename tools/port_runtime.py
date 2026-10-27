"""Make the upstream host adaptations portable, writing only build outputs.

Runs after ShaolinMonksRecomp's tools/prepare_recomp_runtime.py. That script
adapts the pinned SDK for the Windows host; this one removes the Win32-only
parts of those adapted files and patches the few SDK runtime files whose
POSIX paths do not suit Horizon. Neither checkout is changed.
"""
import argparse
import re
from pathlib import Path


# AssembleRaw with a stream whose elements Vulkan cannot read where they are copied to offsets it can
ASSEMBLE_RAW = """// whether Vulkan reads every element of a stream where the title put it
bool StreamAligned(const State& s, const Inputs& inputs, int stream) {
    const VertexShader& layout = inputs.layout;
    for (int k = 0; k < inputs.nregs; k++) {
        const auto& in = layout.inputs[size_t(inputs.regs[k])];
        if (in.stream != stream) continue;
        const uint32_t align = VulkanAlign(HostFormat(in.type).format);
        if (StrideOf(s, layout, in) % align || in.offset % align) return false;
    }
    return true;
}

// an element's bytes: the common sizes as fixed copies, not a library call per element per vertex
inline void CopyElement(uint8_t* dst, const uint8_t* src, uint32_t n) {
    switch (n) {
        case 4: std::memcpy(dst, src, 4); break;
        case 6: std::memcpy(dst, src, 6); break;
        case 8: std::memcpy(dst, src, 8); break;
        case 12: std::memcpy(dst, src, 12); break;
        case 16: std::memcpy(dst, src, 16); break;
        default: std::memcpy(dst, src, n); break;
    }
}

// the stream's vertices copied with each element at a dword offset, in range order (dense) or draw order; the
// elements' new offsets go to offsets
int AddRepackedBuffer(const State& s, const Inputs& inputs, int stream, const DrawRange& d, bool dense,
                      Assembled& out, uint16_t* offsets) {
    using namespace kernel;
    const VertexShader& layout = inputs.layout;
    const auto& st = s.streams[size_t(stream)];
    uint32_t stride = 0, from[16], size[16], to = 0;
    int regs[16], n = 0;
    for (int k = 0; k < inputs.nregs; k++) {
        const int reg = inputs.regs[k];
        const auto& in = layout.inputs[size_t(reg)];
        if (in.stream != stream) continue;
        stride = StrideOf(s, layout, in);
        const uint32_t reads = HostFormat(in.type).bytes;
        to = (to + 3) & ~3u;
        regs[n] = reg;
        from[n] = in.offset;
        size[n] = std::min(reads, TypeBytes(in.type));  // the rest of what the format reads stays 0
        offsets[reg] = uint16_t(to);
        to += reads;
        n++;
    }
    const uint32_t packed = (to + 3) & ~3u;
    const uint8_t* data = static_cast<const uint8_t*>(HostPtr(mem::VirtualFromPhysical(R32(st.vb + kResData))));
    const uint32_t count = dense ? d.span : d.count;
    const int slot = int(out.buffer_count);
    auto& g = out.gathered[size_t(slot)];
    g.assign(size_t(count) * packed, 0);
    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* src = data + size_t(dense ? d.first_vertex + v : DrawVertex(s, d, v)) * stride;
        uint8_t* dst = &g[size_t(v) * packed];
        for (int i = 0; i < n; i++) CopyElement(dst + offsets[regs[i]], src + from[i], size[i]);
    }
    out.buffers[out.buffer_count++] = {g.data(), count * packed, packed};
    return slot;
}

bool AssembleRaw(const State& s, const Inputs& inputs, const DrawRange& d, Assembled& out) {
    bool dense = uint64_t(d.span) <= uint64_t(d.count) * kDenseSpanPerVertex + kDenseSpanSlack;
    int slot[16];
    std::fill(std::begin(slot), std::end(slot), -1);
    uint16_t offsets[16];
    for (int k = 0; k < inputs.nregs; k++) offsets[inputs.regs[k]] = uint16_t(inputs.layout.inputs[size_t(inputs.regs[k])].offset);
    for (int k = 0; k < inputs.nregs; k++) {
        int reg = inputs.regs[k];
        const auto& in = inputs.layout.inputs[size_t(reg)];
        if (slot[in.stream] < 0) {
            slot[in.stream] = StreamAligned(s, inputs, in.stream)
                                  ? AddRawBuffer(s, inputs.layout, in, d, dense, out)
                                  : AddRepackedBuffer(s, inputs, in.stream, d, dense, out, offsets);
        }
        HostElement he = HostFormat(in.type);
        out.elements[size_t(reg)] = {he.format, uint8_t(slot[in.stream]), offsets[reg]};
        out.decode |= uint64_t(he.decode) << (reg * 4);
    }
    out.vertex_count = dense ? d.span : d.count;
    if (NativePrimitive(d.prim)) return DrawNative(d, dense, out);
    auto local = [&](uint32_t i) { return dense ? DrawVertex(s, d, i) - d.first_vertex : i; };
    ListIndices(d.prim, d.count, local, out.indices, out.topology);
    return UseIndexList(out);
}

"""


def replace_once(text, old, new, name):
    if text.count(old) != 1:
        raise ValueError(f"{name}: source changed at required port adaptation: {old[:70]!r}")
    return text.replace(old, new, 1)


def strip_win32_includes(text):
    lines = []
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if s in ("#include <windows.h>", "#include <shobjidl.h>", "#undef CreateSymbolicLink"):
            continue
        lines.append(line)
    return "".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--adapted", required=True, type=Path, help="prepare_recomp_runtime.py output")
    parser.add_argument("--upstream", required=True, type=Path, help="ShaolinMonksRecomp checkout")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    def write(name, text):
        path = args.out / name
        # keep timestamps when nothing changed, so reconfiguring does not rebuild
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")

    # upstream's Linux layer stands in for a few Win32 calls with platform_compat.h, whose Linux headers Horizon
    # lacks: the port's own takes its place (the ported files find it first, in their own folder)
    write("platform_compat.h", '#pragma once\n#include "port_compat.h"\n')

    # game behaviour fixes from upstream, carried over unchanged apart from Win32 includes
    files = strip_win32_includes((args.adapted / "files.cpp").read_text(encoding="utf-8"))
    # each file system call logs the errors it hands the title, with errno: the SD card's file system and
    # newlib differ from a PC's in ways only the title's reaction shows
    files = replace_once(files, '#include "kernel/exports.h"\n', '''#include "kernel/exports.h"
#undef KERNEL_EXPORT
#define KERNEL_EXPORT(ord, name, nargs)                                                                   \\
    static uint32_t kimpl_##name(X86Context& ctx, const ::kernel::Args& a);                              \\
    static void kentry_##name(X86Context& ctx, uint8_t*) {                                               \\
        ::kernel::Args args{ctx};                                                                        \\
        errno = 0;                                                                                       \\
        uint32_t r = kimpl_##name(ctx, args);                                                            \\
        static std::atomic<int> warned{0};                                                               \\
        if (r >= 0x80000000u && warned.fetch_add(1) < 20) LOGW("%s -> %08X (errno %d)", #name, r, errno); \\
        ::kernel::Return(ctx, r, nargs);                                                                 \\
    }                                                                                                    \\
    static const bool kreg_##name = (::kernel::RegisterExport(ord, #name, kentry_##name), true);         \\
    static uint32_t kimpl_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)
''', "files.cpp")
    files = files.replace("        if (ec) { IoStatus(iosb, openshao::storage::kIoError, 0); return openshao::storage::kIoError; }",
                          "        if (ec) { LOGW(\"files: line %d: %s\", __LINE__, ec.message().c_str());"
                          " IoStatus(iosb, openshao::storage::kIoError, 0); return openshao::storage::kIoError; }")
    # Horizon refuses to open a file a second time while it is open for writing, and newlib's stat, truncate and
    # rename by path all open it: on a file the title holds, they go through its own handle instead
    files = replace_once(files, "static uint32_t Finish(File& f,", '''static uint64_t HeldSize(File& f, std::error_code& ec) {
    if (!f.fp) return fs::file_size(f.host, ec);
    struct stat st;
    if (fstat(fileno(f.fp), &st)) {
        ec = std::error_code(errno, std::generic_category());
        return 0;
    }
    ec.clear();
    return uint64_t(st.st_size);
}

static void HeldResize(File& f, uint64_t size, std::error_code& ec) {
    if (!f.fp) return fs::resize_file(f.host, size, ec);
    if (ftruncate(fileno(f.fp), off_t(size))) ec = std::error_code(errno, std::generic_category());
    else ec.clear();
}

// closed for the rename, then opened again under its new name (or its old one, when the rename fails)
static void HeldRename(File& f, const fs::path& to, std::error_code& ec) {
    if (!f.fp) return fs::rename(f.host, to, ec);
    std::fclose(f.fp);
    fs::rename(f.host, to, ec);
    f.fp = std::fopen((ec ? f.host : to).string().c_str(), "r+b");
    if (!f.fp && !ec) ec = std::error_code(errno, std::generic_category());
}

// the save backup reads the file, which the title holds open: closed for the copy, then opened again
static uint32_t HeldBackup(File& f) {
    if (!f.fp) return openshao::storage::BeforeWrite(f.host);
    std::fflush(f.fp);
    std::fclose(f.fp);
    const uint32_t status = openshao::storage::BeforeWrite(f.host);
    f.fp = std::fopen(f.host.string().c_str(), "r+b");
    return f.fp ? status : openshao::storage::kIoError;
}

static uint32_t Finish(File& f,''', "files.cpp")
    files = files.replace("openshao::storage::BeforeWrite(f->host)", "HeldBackup(*f)")
    # each save file operation, the first few hundred, to set a hardware run beside a PC one
    files = replace_once(files, '#include "kernel/exports.h"\n', '''#include "kernel/exports.h"
// GENERIC_WRITE, GENERIC_ALL, FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES, DELETE
static bool WantsWrite(uint32_t access) { return access & (0x40000000u | 0x10000000u | 0x2u | 0x4u | 0x10u | 0x100u | 0x10000u); }

static std::atomic<int> g_save_trace{0};
static uint32_t Fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
#define SAVE_TRACE(host, ...) \\
    do { if (openshao::storage::IsSave(host) && g_save_trace.fetch_add(1) < 400) LOGR("save trace: " __VA_ARGS__); } while (0)
''', "files.cpp")
    files = replace_once(files, '''    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());''', '''    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    SAVE_TRACE(f->host, "create %s mode %s disposition %u access %08X options %08X -> handle %08X", name.c_str(), mode,
               disposition, a[1], options, R32(handle_out));''', "files.cpp")
    files = replace_once(files, '''    (void)options;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);''', '''    (void)options;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    SAVE_TRACE(f->host, "open %s %s -> handle %08X", name.c_str(), f->dir ? "folder" : f->fp ? "file" : "?", R32(handle_out));''', "files.cpp")
    files = replace_once(files, "    if (!write && len && !n) return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);",
                         '    SAVE_TRACE(f->host, "%s handle %08X at %llu, %u of %u bytes, fnv %08X", write ? "write" : "read", h, '
                         '(unsigned long long)pos, n, len, pages ? 0u : Fnv(HostPtr(buf), n));\n'
                         "    if (!write && len && !n) return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);", "files.cpp")
    files = replace_once(files, '''        size = fs::file_size(f->host, ec);
        if (ec) {''', '''        size = fs::file_size(f->host, ec);
        SAVE_TRACE(f->host, "query handle %08X class %u: size %llu%s", h, cls, (unsigned long long)size, ec ? " (failed)" : "");
        if (ec) {''', "files.cpp")
    # A file the title opens only to read is opened read only. Horizon lets nothing else open a file held for
    # writing, not even stat: a save the title reads looked unreadable to its own size and time queries.
    files = replace_once(files, "static uint32_t Finish(File& f,", '''// size and time from the handle where the title holds the file open
static void FillHeldNetworkInfo(uint32_t out, File& f) {
    struct stat st;
    if (!f.fp || fstat(fileno(f.fp), &st)) return FillNetworkInfo(out, f.host);
    const uint64_t t = uint64_t(st.st_mtime) * 10000000ull + 116444736000000000ull;  // FILETIME
    FillNetworkInfo(out, t, uint64_t(st.st_size), false);
}

static uint32_t Finish(File& f,''', "files.cpp")
    files = replace_once(files, '    if (t.read_only) mode = "rb";\n',
                         '    if (t.read_only || (!std::strcmp(mode, "r+b") && !WantsWrite(a[1]))) mode = "rb";\n', "files.cpp")
    files = replace_once(files, '        f->fp = std::fopen(host.c_str(), t.read_only ? "rb" : "r+b");',
                         '        f->fp = std::fopen(host.c_str(), t.read_only || !WantsWrite(a[1]) ? "rb" : "r+b");', "files.cpp")
    files = replace_once(files, '            else FillNetworkInfo(out, f->host);\n            IoStatus(iosb, kSuccess, 0x38);',
                         '            else FillHeldNetworkInfo(out, *f);\n            IoStatus(iosb, kSuccess, 0x38);', "files.cpp")
    # The disk the title sees is no bigger than a retail Xbox's E: partition: titles of the time work out free
    # space in 32 bits, and an SD card's hundreds of gigabytes wrap around to any value. A host disk that is
    # nearly full still reads as nearly full.
    files = replace_once(files, "        capacity = space.capacity / 16384; available = space.available / 16384; free = space.free / 16384;\n",
                         "        capacity = space.capacity / 16384; available = space.available / 16384; free = space.free / 16384;\n"
                         "        constexpr uint64_t kXboxUnits = 4882432000ull / 16384;  // E:, in 16 KiB units\n"
                         "        capacity = std::min(capacity, kXboxUnits);\n"
                         "        available = std::min(available, capacity);\n"
                         "        free = std::min(free, capacity);\n"
                         "        static std::atomic<int> told{0};\n"
                         "        if (told.fetch_add(1) < 10)\n"
                         '            LOGR("files: free space for %s: %llu of %llu units of 16 KiB", file->xbox.c_str(),\n'
                         "                 (unsigned long long)available, (unsigned long long)capacity);\n", "files.cpp")
    # when the title saves, once for each file it opens and writes
    files = replace_once(files, "    bool delete_on_close = false;\n    bool backed_up = false;",
                         "    bool delete_on_close = false;\n    bool backed_up = false;\n    bool save_reported = false;", "files.cpp")
    files = replace_once(files, '        if (status) { LOGE("save write flush failed: %s (%08X)", f->host.string().c_str(), status); '
                         'return Finish(*f, status, n, event, apc, apc_ctx, iosb); }\n    }',
                         '        if (status) { LOGE("save write flush failed: %s (%08X)", f->host.string().c_str(), status); '
                         'return Finish(*f, status, n, event, apc, apc_ctx, iosb); }\n'
                         '        if (!f->save_reported) { f->save_reported = true; LOGR("save: writing %s", f->xbox.c_str()); }\n    }',
                         "files.cpp")
    files = replace_once(files, "f->entry ? f->entry->size : fs::file_size(f->host, ec);",
                         "f->entry ? f->entry->size : HeldSize(*f, ec);", "files.cpp")
    files = replace_once(files, "        size = fs::file_size(f->host, ec);", "        size = HeldSize(*f, ec);", "files.cpp")
    files = replace_once(files, "                uint64_t size = fs::file_size(f->host, ec);",
                         "                uint64_t size = HeldSize(*f, ec);", "files.cpp")
    files = replace_once(files, "                    fs::resize_file(f->host, R64(in), ec);",
                         "                    HeldResize(*f, R64(in), ec);", "files.cpp")
    files = replace_once(files, "            fs::rename(f->host, host, ec);", "            HeldRename(*f, host, ec);", "files.cpp")
    files = "#include <sys/stat.h>\n#include <unistd.h>\n" + files
    write("files.cpp", '#include "port_compat.h"\n#include <atomic>\n#include <cerrno>\n' + files)

    # Vulkan reads a vertex element at any address its component size divides (a packed one: its whole size), where
    # D3D12 wants strides in dwords: the models' 18 byte vertices are read as they are rather than converted
    vertex = (args.adapted / "vertex.cpp").read_text(encoding="utf-8")
    vertex = replace_once(vertex, """        bool readable = he.format && stride && stride % kStrideAlign == 0 && in.offset < kElementOffsetEnd &&
                        in.offset % std::min<uint32_t>(he.bytes, 4) == 0;""",
                          """        bool readable = he.format && stride && in.offset < kElementOffsetEnd;""", "vertex.cpp")
    vertex = replace_once(vertex, "bool RawReadable(const State& s, const Inputs& inputs) {", """uint32_t VulkanAlign(uint8_t format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return 1;
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16G16B16A16_SINT:
            return 2;
        default:
            return 4;
    }
}

bool RawReadable(const State& s, const Inputs& inputs) {""", "vertex.cpp")
    begin = vertex.index("bool AssembleRaw(const State& s, const Inputs& inputs, const DrawRange& d, Assembled& out) {")
    end = vertex.index("// a vertex range at most this far over the draw's count is converted whole")
    vertex = vertex[:begin] + ASSEMBLE_RAW + vertex[end:]
    # ListIndices appends each primitive's indices one by one rather than as an initializer_list range insert
    vertex = replace_once(vertex, "template <typename F>\nvoid ListIndices(",
                          "template <typename... T>\ninline void Add(std::vector<uint32_t>& ix, T... v) {\n    (ix.push_back(v), ...);\n}\n\n"
                          "template <typename F>\nvoid ListIndices(", "vertex.cpp")
    vertex, n = re.subn(r"ix\.insert\(ix\.end\(\), \{(.*?)\}\);", r"Add(ix, \1);", vertex)
    if n != 9:
        raise ValueError(f"vertex.cpp: {n} index inserts, 9 expected")
    write("vertex.cpp", '#include "port_compat.h"\n' + strip_win32_includes(vertex))

    for name in ("boot.cpp", "xapi.cpp", "dsound.cpp"):
        write(name, '#include "port_compat.h"\n' + strip_win32_includes((args.adapted / name).read_text(encoding="utf-8")))

    # Headers the SDK fixes below change, written to include/ ahead of the SDK's own in every target's include path.
    def write_header(name, text):
        (args.out / "include" / name).parent.mkdir(parents=True, exist_ok=True)
        write("include/" + name, text)

    # XboxXBERecomp 7beac8c, "Alpha test only when enabled": a pixel shader that can discard turns off early depth
    # tests, so it discards only with the alpha test on
    shaders_h = (args.sdk / "runtime/gpu/shaders.h").read_text(encoding="utf-8")
    shaders_h = replace_once(shaders_h, "    uint32_t quad = 0;       // a screen quad: QuadHlsl\n",
                             "    uint32_t quad = 0;       // a screen quad: QuadHlsl\n"
                             "    uint32_t alphatest = 0;  // alpha test on: only then may the shader discard, which turns off early depth tests\n",
                             "shaders.h")
    write_header("gpu/shaders.h", shaders_h)
    shaders = strip_win32_includes((args.adapted / "shaders.cpp").read_text(encoding="utf-8"))
    shaders = replace_once(shaders, "    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;\n",
                           "    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;\n    k.alphatest = s.simple[kMAlphaTestEnable >> 2] != 0;\n",
                           "shaders.cpp")
    shaders = replace_once(shaders, '    body += "alphatest(outc.a);\\n" + Hooks().Output() + "return outc;\\n}\\n";\n',
                           '    if (k.alphatest) body += "alphatest(outc.a);\\n";\n'
                           '    body += Hooks().Output() + "return outc;\\n}\\n";\n', "shaders.cpp")
    write("shaders.cpp", '#include "port_compat.h"\n' + shaders)

    # XboxXBERecomp 3a084a5, "Kernel waits wake only the threads blocked on the object": a signal woke every blocked
    # wait in the title, which kept its file reader threads busy
    kernel_h = (args.sdk / "runtime/kernel/kernel.h").read_text(encoding="utf-8")
    kernel_h = replace_once(kernel_h, "struct Object {\n", "struct Waiter;\n\nstruct Object {\n", "kernel.h")
    kernel_h = replace_once(kernel_h, "    uint32_t recursion = 0;\n    std::string name;\n",
                            "    uint32_t recursion = 0;\n    std::vector<Waiter*> waiters;  // Waits blocked on it\n    std::string name;\n",
                            "kernel.h")
    write_header("kernel/kernel.h", kernel_h)
    objects = (args.sdk / "runtime/kernel/objects.cpp").read_text(encoding="utf-8")
    objects = replace_once(objects, "namespace kernel {\n\nnamespace {\n\nstd::mutex g_dispatch;\nstd::condition_variable g_cv;\n",
                           "namespace kernel {\n\nstruct Waiter {\n    std::condition_variable cv;\n};\n\nnamespace {\n\n"
                           "std::mutex g_dispatch;\nstd::vector<Waiter*> g_waiters;  // every blocked Wait, for SignalChanged\n", "objects.cpp")
    objects = replace_once(objects, "void SignalChanged() { g_cv.notify_all(); }\n", """void SignalChanged() {
    for (Waiter* w : g_waiters) w->cv.notify_one();
}

// only the threads blocked on this object; waking every waiter on each signal kept file reader threads busy
static void Wake(Object& o) {
    for (Waiter* w : o.waiters) w->cv.notify_one();
}

// a Wait's place on its objects' waiter lists while it blocks, under the dispatcher lock
struct Queued {
    const std::vector<ObjectRef>& objs;
    Waiter w;
    bool on = false;
    void Add() {
        for (auto& o : objs) {
            if (o) o->waiters.push_back(&w);
        }
        g_waiters.push_back(&w);
        on = true;
    }
    void Remove() {
        if (!on) return;
        for (auto& o : objs) {
            if (o) std::erase(o->waiters, &w);
        }
        std::erase(g_waiters, &w);
        on = false;
    }
    ~Queued() { Remove(); }
};
""", "objects.cpp")
    objects = replace_once(objects, "    std::unique_lock<std::mutex> lock(g_dispatch);\n    for (;;) {\n",
                           "    std::unique_lock<std::mutex> lock(g_dispatch);\n    Queued queued{objs};\n    for (;;) {\n", "objects.cpp")
    objects = replace_once(objects, "            if (pending) {\n                lock.unlock();\n",
                           "            if (pending) {\n                queued.Remove();\n                lock.unlock();\n", "objects.cpp")
    objects = replace_once(objects, "            g_cv.wait_for(lock, nanoseconds(std::min<int64_t>(deadline - now, 50000000)));\n",
                           "            if (!queued.on) queued.Add();\n"
                           "            queued.w.cv.wait_for(lock, nanoseconds(std::min<int64_t>(deadline - now, 50000000)));\n", "objects.cpp")
    objects = replace_once(objects, "            g_cv.wait_for(lock, milliseconds(50));\n",
                           "            if (!queued.on) queued.Add();\n            queued.w.cv.wait_for(lock, milliseconds(50));\n", "objects.cpp")
    objects = replace_once(objects, "            SetSignal(*e.timer, 1);\n            g_cv.notify_all();\n",
                           "            SetSignal(*e.timer, 1);\n            Wake(*e.timer);\n", "objects.cpp")
    if objects.count("g_cv.notify_all();") != 4:
        raise ValueError("objects.cpp: the signals of SetEvent, PulseEvent, ReleaseSemaphore and ReleaseMutant changed")
    objects = objects.replace("g_cv.notify_all();", "Wake(o);")
    write("objects.cpp", objects)

    # XboxXBERecomp 16bf1cd, "Cut render and game thread work per draw and per bind": the format table by byte
    texture = (args.sdk / "runtime/gpu/texture.cpp").read_text(encoding="utf-8")
    texture = replace_once(texture, """const FormatInfo& Format(uint32_t f) {
    for (auto& e : kFormats)
        if (e.id == f) return e.info;
    return kUnknown;
}""", """// by format byte: asked many times a draw
const FormatInfo& Format(uint32_t f) {
    static const auto table = [] {
        std::array<const FormatInfo*, 256> t;
        t.fill(&kUnknown);
        for (auto& e : kFormats) t[e.id] = &e.info;
        return t;
    }();
    return f < table.size() ? *table[f] : kUnknown;
}""", "texture.cpp")
    write("texture.cpp", "#include <array>\n" + texture)

    d3d = strip_win32_includes((args.adapted / "d3d8.cpp").read_text(encoding="utf-8"))
    d3d = replace_once(d3d, "const uint64_t tick=GetTickCount64();", "const uint64_t tick=port::TickMs();", "d3d8.cpp")
    # the quick menu's cheats and save slots act once per frame, on the title's thread (port/common/cheats.h)
    d3d = replace_once(d3d, "HLE_STDCALL(D3DDevice_Swap, 1) {\n", "HLE_STDCALL(D3DDevice_Swap, 1) {\n    port::cheats::OnFrame(ctx);\n",
                       "d3d8.cpp")
    # writes to the simple states and vertex shader constants mark what the render thread compares (port/common/state_dirty.h)
    d3d, n = re.subn(r"g_state\.simple\[([^\]]+)\]", r"port::state::Simple(g_state, \1)", d3d)
    if n != 5:
        raise ValueError(f"d3d8.cpp: {n} simple state writes, 5 expected")
    d3d, n = re.subn(r"g_state\.vsc\[([^\]]+)\]", r"port::state::Vsc(g_state, \1)", d3d)
    if n != 7:
        raise ValueError(f"d3d8.cpp: {n} vertex shader constant writes, 7 expected")
    d3d = replace_once(d3d, "    gpu::ApplyPixelConstants(g_state, reg, n);\n",
                       "    gpu::ApplyPixelConstants(g_state, reg, n);\n    port::state::PixelConstants();\n", "d3d8.cpp")
    d3d = replace_once(d3d, "    g_state = gpu::State{};\n", "    g_state = gpu::State{};\n    port::state::All();\n", "d3d8.cpp")
    # and every other write to what SendState compares, marked after the statement (the whole member: a write in a
    # one-line loop or condition marks it once, or when nothing was written, which only costs a comparison)
    marked = ("rs", "tss", "viewport", "scissor_count", "scissors", "ss_offset_x", "ss_offset_y", "transforms", "material",
              "lights", "light_enable", "index_buffer", "index_data", "base_vertex", "vs_handle", "psc", "zscale")
    write_re = re.compile(r"\bg_state\.(%s)\b(?:\[(?:[^\[\]]|\[[^\]]*\])*\])*(?:\.data\(\))?\s*=(?!=)|memcpy\(g_state\.(%s)\b"
                          % ("|".join(marked), "|".join(marked)))
    sync_begin = d3d.index("void SyncDeferred() {")
    sync_end = d3d.index("\n}\n", sync_begin)
    lines, at, n = [], 0, 0
    for line in d3d.split("\n"):
        m = write_re.search(line)
        if m and line.rstrip().endswith(";") and not sync_begin <= at < sync_end:
            indent = line[:len(line) - len(line.lstrip())]
            line += f"\n{indent}port::state::Mark(g_state, g_state.{m.group(1) or m.group(2)});"
            n += 1
        lines.append(line)
        at += len(line.split("\n")[0]) + 1
    if n != 27:
        raise ValueError(f"d3d8.cpp: {n} state writes to mark, 27 expected")
    d3d = "\n".join(lines)
    # the deferred render and texture stage states, copied from the title's memory before each draw, marked only where
    # they changed
    d3d = replace_once(d3d, """    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
""", """    // whole blocks compared first: on most draws nothing changed
    constexpr uint32_t kDeferred = gpu::kRsComplexFirst - gpu::kRsDeferredFirst;
    if (std::memcmp(&g_state.rs[gpu::kRsDeferredFirst], HostPtr(kRenderState + gpu::kRsDeferredFirst * 4), kDeferred * 4))
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) {
        const uint32_t v = R32(kRenderState + i * 4);
        if (g_state.rs[i] == v) continue;
        g_state.rs[i] = v;
        port::state::Mark(g_state, g_state.rs[i]);
    }
    for (int s = 0; s < 4; s++)
        if (std::memcmp(g_state.tss[s].data(), HostPtr(kTextureState + s * kStageStates * 4), gpu::kTssBumpEnvMat00 * 4))
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) {
            const uint32_t v = R32(kTextureState + (s * kStageStates + i) * 4);
            if (g_state.tss[s][i] == v) continue;
            g_state.tss[s][i] = v;
            port::state::Mark(g_state, g_state.tss[s][i]);
        }
""", "d3d8.cpp")
    # XboxXBERecomp 16bf1cd: binding the same texture, stream or indices again skips the AddRef and Release that
    # would cancel out
    for new, old, between in (("tex", "old", "    SetDev(kDevTextures + stage * 4, tex);\n"),
                              ("vb", "old", "    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);\n"
                                            "    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);\n"),
                              ("ib", "old", "    W32(kIndexData, ib ? R32(ib + 4) : 0);\n    SetDev(kDevIndexBuffer, ib);\n"
                                            "    SetDev(kDevBaseVertex, a[1]);\n")):
        d3d = replace_once(d3d, f"    AddRef(ctx, {new});\n{between}    Release(ctx, {old});\n",
                           f"    bool same = {new} == {old};\n    if (!same) AddRef(ctx, {new});\n{between}"
                           f"    if (!same) Release(ctx, {old});\n", "d3d8.cpp")
    write("d3d8.cpp", '#include "port_compat.h"\n#include "cheats.h"\n#include "state_dirty.h"\n' + d3d)

    # upstream's settings model, with its Win32 file replacement swapped for the port's
    settings = strip_win32_includes((args.upstream / "src/launcher_settings.cpp").read_text(encoding="utf-8"))
    settings = replace_once(settings, 'temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L".tmp";',
                            'temporary += "." + std::to_string(port::TickMs()) + ".tmp";', "launcher_settings.cpp")
    settings = replace_once(settings, "if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))",
                            "if (!port::ReplaceFile(temporary, path))", "launcher_settings.cpp")
    write("launcher_settings.cpp", '#include "port_compat.h"\n' + settings)

    # the render thread compares draw state with SSE4.1, 13 KB of it on every draw: NEON on the Switch, memcmp elsewhere
    rt = strip_win32_includes((args.sdk / "runtime/gpu/render_thread.cpp").read_text(encoding="utf-8"))
    rt = replace_once(rt, "#include <smmintrin.h>\n", "#if defined(__aarch64__)\n#include <arm_neon.h>\n#endif\n", "render_thread.cpp")
    begin = rt.index("bool Same(const uint8_t* a, const uint8_t* b, size_t n) {")
    end = rt.index("\n}\n", begin) + 3
    rt = rt[:begin] + (
        "inline bool Same(const uint8_t* a, const uint8_t* b, size_t n) {\n"
        "#if defined(__aarch64__)\n"
        "    size_t i = 0;\n"
        "    uint8x16_t diff = vdupq_n_u8(0);\n"
        "    for (; i + 16 <= n; i += 16) diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));\n"
        "    if (vmaxvq_u8(diff)) return false;\n"
        "    return i == n || !std::memcmp(a + i, b + i, n - i);\n"
        "#else\n"
        "    return !std::memcmp(a, b, n);\n"
        "#endif\n"
        "}\n") + rt[end:]
    rt = replace_once(rt,
        "        for (const Range& r : ranges_)\n"
        "            for (size_t off = r.off, end = r.off + r.len; off < end; off += 64)\n"
        "                Delta(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, std::min<size_t>(64, end - off));\n",
        r"""        for (const Range& r : ranges_) {
            const size_t end = r.off + r.len;
            // only the range's 64 byte steps that overlap a marked piece, found a word of marks at a time
            const size_t first = r.off >> 6, last = (end - 1) >> 6;
            size_t done = r.off;  // the range is sent up to here
            for (size_t w = first >> 6; w <= last >> 6; w++) {
                uint64_t bits = port::state::g_dirty[w];
                if (w == first >> 6) bits &= ~0ull << (first & 63);
                if (w == last >> 6 && (last & 63) != 63) bits &= (2ull << (last & 63)) - 1;
                while (bits) {
                    const size_t piece = w << 6 | size_t(__builtin_ctzll(bits));
                    bits &= bits - 1;
                    const size_t from = std::max(piece << 6, r.off), to = std::min((piece + 1) << 6, end);
                    for (size_t off = std::max(done, r.off + (from - r.off) / 64 * 64); off < to; off += 64) {
                        Delta(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, std::min<size_t>(64, end - off));
                        done = off + 64;
                    }
                }
            }
        }
        // XBE_STATECHECK: after that, whatever still differs was written unmarked (or skipped by the walk above)
        if (port::state::Check())
            for (const Range& r : ranges_)
                for (size_t off = r.off, end = r.off + r.len; off < end; off += 64) {
                    const size_t n = std::min<size_t>(64, end - off);
                    if (Same(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, n)) continue;
                    LOG_FIRST(16, "state check: bytes %zu to %zu changed unmarked", off, off + n);
                    Delta(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, n);
                }
        port::state::Sent();
""", "render_thread.cpp")
    # the header and use lookups' caches, in front of their hash maps: 256 entries, a draw's resources missed 64
    for kind in ("headers", "uses"):
        rt = replace_once(rt, f"recent_{kind}_[(header >> 3) & 63]", f"recent_{kind}_[(header >> 3) & 255]", "render_thread.cpp")
    rt = replace_once(rt, "    std::array<Recent<HeaderCopy>, 64> recent_headers_{};\n    std::array<Recent<uint64_t>, 64> recent_uses_{};\n",
                      "    std::array<Recent<HeaderCopy>, 256> recent_headers_{};\n    std::array<Recent<uint64_t>, 256> recent_uses_{};\n",
                      "render_thread.cpp")
    rt = replace_once(rt, "                    std::memcpy(s + head[0], p + 8, head[1]);\n",
                      "                    std::memcpy(s + head[0], p + 8, head[1]);\n"
                      "                    port::state::Applied(head[0], head[1]);\n", "render_thread.cpp")
    rt = replace_once(rt, "        cur_ = NewBatch();\n        std::thread([this] { Run(); }).detach();\n",
                      "        cur_ = NewBatch();\n        port::state::g_vsc_counted = true;\n"
                      "        std::thread([this] { Run(); }).detach();\n", "render_thread.cpp")
    rt = replace_once(rt,
        '        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);\n'
        '        if (auto set = reinterpret_cast<SetDescription>(\n'
        '                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))\n'
        '            set(GetCurrentThread(), L"render");\n',
        '        port::OnRenderThread();\n', "render_thread.cpp")
    write("render_thread.cpp", '#include "port_compat.h"\n#include "state_dirty.h"\n' + rt)

    # the Linux test beds' pad script, with Black and White: the Nexus code is entered holding White
    pads = (args.sdk / "harness/pad_script.cpp").read_text(encoding="utf-8")
    pads = replace_once(pads, "// UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR (left stick up, down, left, right)",
                        "// UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT RT LS LSD LSL LSR (left stick up, down, left, right)",
                        "pad_script.cpp")
    pads = replace_once(pads, '        else if (b == "LT") p.analog[6] = 255;\n',
                        '        else if (b == "BLACK") p.analog[4] = 255;\n        else if (b == "WHITE") p.analog[5] = 255;\n'
                        '        else if (b == "LT") p.analog[6] = 255;\n', "pad_script.cpp")
    write("pad_script.cpp", pads)

    # x86 intrinsics are included but not used past the Windows branch
    dispatch = (args.sdk / "runtime/cpu/dispatch.cpp").read_text(encoding="utf-8")
    dispatch = replace_once(dispatch, "#else\n#include <x86intrin.h>\n#endif", "#endif", "dispatch.cpp")
    write("dispatch.cpp", dispatch)

    # devkitA64's GCC stops on a 1-byte aligned double passed as an argument; read it whole first
    crt = (args.sdk / "runtime/xdk/crt_math.cpp").read_text(encoding="utf-8")
    crt = replace_once(crt, "    X87_ST(0) = std::pow(double(X86_MEMF64(ctx.esp + 4)), double(X86_MEMF64(ctx.esp + 12)));",
                       "    double x, y;\n"
                       "    std::memcpy(&x, base + ctx.esp + 4, 8);\n"
                       "    std::memcpy(&y, base + ctx.esp + 12, 8);\n"
                       "    X87_ST(0) = std::pow(x, y);", "crt_math.cpp")
    write("crt_math.cpp", "#include <cstring>\n" + crt)

    # Recompiled code nests host frames as deep as the guest's calls. The SDK
    # reserves 64 MB per thread; on Horizon a thread stack is committed heap.
    threads = (args.sdk / "runtime/kernel/threads.cpp").read_text(encoding="utf-8")
    threads = replace_once(threads, "    pthread_attr_setstacksize(&attr, 64u << 20);",
                           "    pthread_attr_setstacksize(&attr, port::kGuestThreadStack);\n"
                           "    port::BeforeGuestThread(&attr);", "threads.cpp")
    threads = replace_once(threads, "    pthread_create(&th, &attr, [](void* p) -> void* {\n",
                           "    pthread_create(&th, &attr, [](void* p) -> void* {\n        port::OnGuestThread();\n",
                           "threads.cpp")
    threads = replace_once(threads, "    pthread_setname_np(pthread_self(), name);", "    port::NameThread(name);", "threads.cpp")
    write("threads.cpp", '#include "port_compat.h"\n' + threads)

    # A release build logs errors only. The port keeps warnings as well, as hardware runs are its only window
    # into a run, and lets a crash handler flush what is buffered without taking the lock. Info lines stay out
    # after boot: the title's input and draw state alone are dozens a second, and SD writes hold up its thread.
    log = (args.sdk / "runtime/core/log.cpp").read_text(encoding="utf-8")
    log = replace_once(log, "    if (!kVerbose && lv < Level::Error) return;",
                       "    // boot's info lines too, before the title's own start them coming by the dozen\n"
                       "    if (!kVerbose && lv < Level::Warn &&\n"
                       "        (lv < Level::Info || std::chrono::steady_clock::now() - g_start > std::chrono::seconds(5)))\n"
                       "        return;", "log.cpp")
    log = replace_once(log, "void Flush() {", "void FlushUnlocked() {\n    if (g_file) std::fflush(g_file);\n}\n\nvoid Flush() {", "log.cpp")
    write("log.cpp", log)

    # Guest memory: the 4 GB window and the two aliased views of physical RAM
    memory = (args.sdk / "runtime/kernel/memory.cpp").read_text(encoding="utf-8")
    memory = replace_once(memory, "#else\n#include <sys/mman.h>\n#endif",
                          "#elif defined(__SWITCH__)\n#include \"nx/guest_memory.h\"\n#else\n#include <sys/mman.h>\n#endif",
                          "memory.cpp")
    memory = replace_once(memory, "#else\n    void* p = mmap(reinterpret_cast<void*>(0x100000000ull)",
                          "#elif defined(__SWITCH__)\n"
                          "    g_base = nx::gm::Init(size, kContiguousBase, kWriteCombineBase, kPhysicalSize);\n"
                          "#else\n    void* p = mmap(reinterpret_cast<void*>(0x100000000ull)", "memory.cpp")
    memory = replace_once(memory, "#else\n    if (mprotect(g_base + first, size_t(end - first), PROT_READ | PROT_WRITE)) {",
                          "#elif defined(__SWITCH__)\n"
                          "    if (!nx::gm::Commit(first, size_t(end - first))) {\n"
                          "        LOGE(\"memory: commit %08X+%X failed\", first, uint32_t(end - first));\n"
                          "        return false;\n    }\n"
                          "#else\n    if (mprotect(g_base + first, size_t(end - first), PROT_READ | PROT_WRITE)) {",
                          "memory.cpp")
    memory = replace_once(memory, "#else\n    madvise(g_base + first, size_t(end - first), MADV_DONTNEED);",
                          "#elif defined(__SWITCH__)\n"
                          "    nx::gm::Decommit(first, size_t(end - first));\n"
                          "#else\n    madvise(g_base + first, size_t(end - first), MADV_DONTNEED);", "memory.cpp")
    write("memory.cpp", memory)


if __name__ == "__main__":
    main()
