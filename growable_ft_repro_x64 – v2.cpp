#include <Windows.h>
#include <winnt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using NTSTATUS = LONG;
using AddGrowFn = NTSTATUS (NTAPI*)(void**, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
using GrowFn = void (NTAPI*)(void*, DWORD);
using DeleteGrowFn = void (NTAPI*)(void*);
using LookupFn = PRUNTIME_FUNCTION (NTAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
using VirtualUnwindFn = PEXCEPTION_ROUTINE (NTAPI*)(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64, PKNONVOLATILE_CONTEXT_POINTERS);

extern "C" __declspec(dllexport) volatile DWORD64 g_vu_stage = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_image_base = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_control_pc = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_function_entry = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_pre_rip = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_pre_rsp = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_pre_rbp = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_post_rip = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_post_rsp = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_post_rbp = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_establisher = 0;
extern "C" __declspec(dllexport) volatile DWORD64 g_vu_handler = 0;

#pragma pack(push, 1)
struct UWCODE { UCHAR CodeOffset; UCHAR Op; };
struct UWINFO {
    UCHAR VersionFlags;
    UCHAR PrologSize;
    UCHAR CodeCount;
    UCHAR FrameReg;
    UWCODE Codes[2];
};
#pragma pack(pop)

static DWORD ReadArgMs(int argc, char** argv, int index, DWORD fallback) {
    if (argc <= index) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(argv[index], &end, 0);
    return (end && *end == '\0') ? static_cast<DWORD>(v) : fallback;
}

int main(int argc, char** argv) {
    const DWORD preGrowMs = ReadArgMs(argc, argv, 1, 30000);
    const DWORD postGrowMs = ReadArgMs(argc, argv, 2, 30000);

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 10;

    auto addGrow = reinterpret_cast<AddGrowFn>(::GetProcAddress(ntdll, "RtlAddGrowableFunctionTable"));
    auto grow = reinterpret_cast<GrowFn>(::GetProcAddress(ntdll, "RtlGrowFunctionTable"));
    auto delGrow = reinterpret_cast<DeleteGrowFn>(::GetProcAddress(ntdll, "RtlDeleteGrowableFunctionTable"));
    auto lookup = reinterpret_cast<LookupFn>(::GetProcAddress(ntdll, "RtlLookupFunctionEntry"));
    auto virtualUnwind = reinterpret_cast<VirtualUnwindFn>(::GetProcAddress(ntdll, "RtlVirtualUnwind"));
    if (!addGrow || !grow || !delGrow || !lookup || !virtualUnwind) return 11;

    BYTE* region = static_cast<BYTE*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!region) return 12;

    PRUNTIME_FUNCTION table = static_cast<PRUNTIME_FUNCTION>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RUNTIME_FUNCTION) * 2));
    if (!table) return 13;

    const BYTE stub[] = { 0x55, 0x48, 0x8B, 0xEC, 0x5D, 0xC3 };
    std::memcpy(region + 0x100, stub, sizeof(stub));
    std::memcpy(region + 0x120, stub, sizeof(stub));

    auto* uw = reinterpret_cast<UWINFO*>(region + 0x200);
    uw->VersionFlags = 1;
    uw->PrologSize = 4;
    uw->CodeCount = 2;
    uw->FrameReg = 5;
    uw->Codes[0] = { 4, static_cast<UCHAR>((0 << 4) | 3) };
    uw->Codes[1] = { 1, static_cast<UCHAR>((5 << 4) | 0) };

    table[0].BeginAddress = 0x100;
    table[0].EndAddress = 0x106;
    table[0].UnwindData = 0x200;
    table[1].BeginAddress = 0x120;
    table[1].EndAddress = 0x126;
    table[1].UnwindData = 0x200;

    ::FlushInstructionCache(::GetCurrentProcess(), region, 0x1000);

    void* dynamicTable = nullptr;
    const NTSTATUS st = addGrow(&dynamicTable, table, 1, 2, reinterpret_cast<ULONG_PTR>(region), reinterpret_cast<ULONG_PTR>(region + 0x300));

    std::printf("PID=%lu\n", ::GetCurrentProcessId());
    std::printf("RtlAddGrowableFunctionTable status=0x%08lX handle=%p region=%p\n", static_cast<unsigned long>(st), dynamicTable, region);
    if (st < 0 || !dynamicTable) return 20;

    DWORD64 imageBase = 0;
    auto before = lookup(reinterpret_cast<DWORD64>(region + 0x120), &imageBase, nullptr);
    std::printf("before grow lookup(second)=%p imageBase=%p\n", before, reinterpret_cast<void*>(imageBase));
    std::printf("sleep-before-grow-ms=%lu\n", static_cast<unsigned long>(preGrowMs));
    ::Sleep(preGrowMs);

    std::printf("calling RtlGrowFunctionTable(handle=%p, newCount=2)\n", dynamicTable);
    grow(dynamicTable, 2);
    imageBase = 0;
    auto after = lookup(reinterpret_cast<DWORD64>(region + 0x120), &imageBase, nullptr);
    std::printf("after grow lookup(second)=%p imageBase=%p\n", after, reinterpret_cast<void*>(imageBase));

    CONTEXT synthetic{};
    synthetic.ContextFlags = CONTEXT_FULL;
    DWORD64 unwindStack[4]{};
    unwindStack[0] = reinterpret_cast<DWORD64>(region + 0x2e0);
    unwindStack[1] = reinterpret_cast<DWORD64>(region + 0x2f0);
    synthetic.Rsp = reinterpret_cast<DWORD64>(&unwindStack[0]);
    synthetic.Rbp = synthetic.Rsp;
    synthetic.Rip = reinterpret_cast<DWORD64>(region + 0x124);

    g_vu_stage = 1;
    g_vu_image_base = imageBase;
    g_vu_control_pc = synthetic.Rip;
    g_vu_function_entry = reinterpret_cast<DWORD64>(after);
    g_vu_pre_rip = synthetic.Rip;
    g_vu_pre_rsp = synthetic.Rsp;
    g_vu_pre_rbp = synthetic.Rbp;

    PVOID handlerData = nullptr;
    DWORD64 establisherFrame = 0;
    const auto exceptionHandler = virtualUnwind(0, imageBase, synthetic.Rip, after, &synthetic, &handlerData, &establisherFrame, nullptr);
    g_vu_post_rip = synthetic.Rip;
    g_vu_post_rsp = synthetic.Rsp;
    g_vu_post_rbp = synthetic.Rbp;
    g_vu_establisher = establisherFrame;
    g_vu_handler = reinterpret_cast<DWORD64>(exceptionHandler);
    g_vu_stage = 2;
    std::printf("virtual unwind input: rip=%p rsp=%p rbp=%p ret=%p saved_rbp=%p\n",
        reinterpret_cast<void*>(region + 0x124),
        reinterpret_cast<void*>(reinterpret_cast<DWORD64>(&unwindStack[0])),
        reinterpret_cast<void*>(reinterpret_cast<DWORD64>(&unwindStack[0])),
        reinterpret_cast<void*>(unwindStack[1]),
        reinterpret_cast<void*>(unwindStack[0]));
    std::printf("virtual unwind output: rip=%p rsp=%p rbp=%p estab=%p handler=%p\n",
        reinterpret_cast<void*>(synthetic.Rip),
        reinterpret_cast<void*>(synthetic.Rsp),
        reinterpret_cast<void*>(synthetic.Rbp),
        reinterpret_cast<void*>(establisherFrame),
        reinterpret_cast<void*>(exceptionHandler));

    std::printf("sleep-after-grow-ms=%lu\n", static_cast<unsigned long>(postGrowMs));
    ::Sleep(postGrowMs);

    std::printf("calling RtlDeleteGrowableFunctionTable(handle=%p)\n", dynamicTable);
    delGrow(dynamicTable);
    ::HeapFree(::GetProcessHeap(), 0, table);
    ::VirtualFree(region, 0, MEM_RELEASE);
    std::puts("done");
    return 0;
}