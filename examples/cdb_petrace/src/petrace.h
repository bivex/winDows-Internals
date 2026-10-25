// ============================================================================
// petrace.h — PE Path Tracer DbgEng Extension Header & Data Structures
// ============================================================================

#pragma once

#include <windows.h>
#include <dbgeng.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <set>

#pragma comment(lib, "dbgeng.lib")

// ----------------------------------------------------------------------------
// PE Directory Constants
// ----------------------------------------------------------------------------
#ifndef IMAGE_DIRECTORY_ENTRY_COMHEADER
#define IMAGE_DIRECTORY_ENTRY_COMHEADER 14
#endif

// ----------------------------------------------------------------------------
// ARM64 Exception Unwind Data Structures (from winnt.h)
// ----------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct _PE_ARM64_RUNTIME_FUNCTION {
    DWORD BeginAddress;
    union {
        DWORD UnwindData;
        struct {
            DWORD Flag : 2;
            DWORD FunctionLength : 11;
            DWORD RegF : 3;
            DWORD RegI : 4;
            DWORD H : 1;
            DWORD CR : 2;
            DWORD FrameSize : 9;
        } Packed;
    };
} PE_ARM64_RUNTIME_FUNCTION;

typedef struct _PE_COR20_HEADER {
    DWORD cb;
    WORD  MajorRuntimeVersion;
    WORD  MinorRuntimeVersion;
    IMAGE_DATA_DIRECTORY MetaData;
    DWORD Flags;
    DWORD EntryPointTokenOrRVA;
    IMAGE_DATA_DIRECTORY Resources;
    IMAGE_DATA_DIRECTORY StrongNameSignature;
    IMAGE_DATA_DIRECTORY CodeManagerTable;
    IMAGE_DATA_DIRECTORY VTableFixups;
    IMAGE_DATA_DIRECTORY ExportAddressTableJumps;
    IMAGE_DATA_DIRECTORY ManagedNativeHeader;
} PE_COR20_HEADER;

typedef struct _PE_COR_VTABLEFIXUP {
    DWORD RVA;
    WORD  Count;
    WORD  Type;
} PE_COR_VTABLEFIXUP;
#pragma pack(pop)

// ----------------------------------------------------------------------------
// DbgEng Context Wrapper
// ----------------------------------------------------------------------------
struct DbgEngContext {
    IDebugClient4* Client = nullptr;
    IDebugControl4* Control = nullptr;
    IDebugDataSpaces4* DataSpaces = nullptr;
    IDebugSymbols3* Symbols = nullptr;
    IDebugSystemObjects* SystemObjects = nullptr;

    bool Init(PDEBUG_CLIENT4 inClient) {
        Client = inClient;
        if (!Client) return false;

        Client->QueryInterface(__uuidof(IDebugControl4), (void**)&Control);
        Client->QueryInterface(__uuidof(IDebugDataSpaces4), (void**)&DataSpaces);
        Client->QueryInterface(__uuidof(IDebugSymbols3), (void**)&Symbols);
        Client->QueryInterface(__uuidof(IDebugSystemObjects), (void**)&SystemObjects);

        return (Control && DataSpaces && Symbols);
    }

    void Release() {
        if (Control) { Control->Release(); Control = nullptr; }
        if (DataSpaces) { DataSpaces->Release(); DataSpaces = nullptr; }
        if (Symbols) { Symbols->Release(); Symbols = nullptr; }
        if (SystemObjects) { SystemObjects->Release(); SystemObjects = nullptr; }
    }

    void Out(const char* fmt, ...) {
        if (!Control) return;
        char buf[2048] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Control->Output(DEBUG_OUTPUT_NORMAL, "%s", buf);
    }

    bool ReadMemory(ULONG64 offset, void* buffer, ULONG size, ULONG* bytesRead = nullptr) {
        if (!DataSpaces) return false;
        ULONG got = 0;
        HRESULT hr = DataSpaces->ReadVirtual(offset, buffer, size, &got);
        if (bytesRead) *bytesRead = got;
        return (hr == S_OK && got == size);
    }

    std::string DisassembleInstruction(ULONG64 offset, ULONG64* nextOffset = nullptr) {
        if (!Control) return "";
        char disasm[512] = {};
        ULONG disasmSize = 0;
        ULONG64 endOff = 0;
        HRESULT hr = Control->Disassemble(offset, 0, disasm, sizeof(disasm), &disasmSize, &endOff);
        if (nextOffset) *nextOffset = (hr == S_OK) ? endOff : (offset + 4);
        if (FAILED(hr)) return "<disasm error>";

        // Strip trailing newlines
        while (disasmSize > 0 && (disasm[disasmSize - 1] == '\r' || disasm[disasmSize - 1] == '\n')) {
            disasm[--disasmSize] = '\0';
        }
        return std::string(disasm);
    }

    std::string GetSymbolName(ULONG64 offset, ULONG64* displacement = nullptr) {
        if (!Symbols) return "";
        char name[512] = {};
        ULONG nameSize = 0;
        ULONG64 disp = 0;
        HRESULT hr = Symbols->GetNameByOffset(offset, name, sizeof(name), &nameSize, &disp);
        if (displacement) *displacement = disp;
        if (FAILED(hr) || name[0] == '\0') return "";

        if (disp != 0) {
            char full[600] = {};
            snprintf(full, sizeof(full), "%s+0x%llX", name, (unsigned long long)disp);
            return std::string(full);
        }
        return std::string(name);
    }

    ULONG64 ResolveModuleBase(const char* moduleOrAddr);
};

// ----------------------------------------------------------------------------
// Subsystem Analyzer Prototypes
// ----------------------------------------------------------------------------
HRESULT AnalyzePePaths(DbgEngContext& ctx, const char* args);
HRESULT AnalyzeControlFlowGraph(DbgEngContext& ctx, const char* args);
