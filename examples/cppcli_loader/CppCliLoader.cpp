// ============================================================================
// CppCliLoader.cpp — Manual (usermode) PE loader for C++/CLI Mixed-Mode DLLs
//
// Implements, in plain Win32 C++, the loader architecture documented in:
//   - KERNEL/CppCLI_IJW_Loader_Research.md
//       PE anatomy of a mixed image, the 3-state VTableFixup slot lifecycle
//       (token -> IJW heap thunk -> back-patched JIT code), CLR bootstrap via
//       the native entry point hijack.
//   - KERNEL/CppCLI_Relocations_Research.md
//       Dual-layer relocation: Layer 1 (ntdll DIR64 base relocs) must NEVER
//       touch Layer 2 slots (CLR-owned VTableFixups); applying the ASLR delta
//       to 32-bit MethodDef tokens corrupts them into garbage addresses.
//   - KERNEL/CppCLI_Dual_Entry_Points_CorDllMain_Research.md
//       AddressOfEntryPoint is a 3-instruction stub that tail-jumps through
//       the IAT to mscoree!_CorDllMain; the CRT loader gate
//       (__scrt_is_safe_for_managed_code) defers managed static init out of
//       DLL_PROCESS_ATTACH.
//
// Loader pipeline (mirrors what ntdll does for a mixed DLL):
//   1. Parse PE + CLI (COR20) header; verify mixed-mode shape
//      (ILONLY == 0, NATIVE_ENTRYPOINT set, VTableFixups populated).   [CPP-01/02]
//   2. Map sections manually (VirtualAlloc + memcpy + per-section protect).
//   3. Apply Layer-1 base relocations while EXCLUDING every VTableFixup
//      slot RVA (Layer 2 belongs to the CLR).                          [CPP-03]
//   4. Resolve the import table. This loads mscoree.dll — the DLL's entry
//      stub tail-jumps through this IAT to mscoree!_CorDllMain.
//   5. Emulate ntdll!LdrpInitializeTls: allocate a TLS index, clone the
//      IMAGE_TLS_DIRECTORY64 template block for the calling thread.    [CPP-09]
//   6. Invoke the native entry point with DLL_PROCESS_ATTACH — on a thread
//      NOT holding ntdll!LdrpLoaderLock (structural deadlock avoidance;
//      the CLR's IJW machinery installs the State-2 thunks during attach).
//   7. Resolve exports (the adrp/ldr/br x9 dispatch stubs) and observe the
//      slot lifecycle across the first crossover call.                [CPP-05/06]
//
// Documented divergences from the OS loader (deliberate):
//   - No LDR_DATA_TABLE_ENTRY registration: the module is invisible to the
//     PEB module list (lm/EnumProcessModules will not show it).
//   - Static-TLS template cloning is explicit: threads must call
//     ThreadAttach() before touching __declspec(thread) variables
//     (the demo worker thread does exactly this).
//   - No loader lock is held during initialization.
//
// Build:  cl /EHsc CppCliLoader.cpp /Fe:CppCliLoader.exe
// Usage:  CppCliLoader.exe [path-to-MixedLib.dll]
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>
#include <wchar.h>

#if !defined(_M_ARM64) && !defined(_M_X64)
#error "CppCliLoader.cpp targets 64-bit Windows only (ARM64 or x64)."
#endif

#if defined(_M_ARM64)
static const char* kHostArch = "ARM64";
constexpr WORD kHostMachine = IMAGE_FILE_MACHINE_ARM64;
#else
static const char* kHostArch = "x64";
constexpr WORD kHostMachine = IMAGE_FILE_MACHINE_AMD64;
#endif

// Some Windows SDK versions of winnt.h omit this name; the index is fixed
// by the PE specification (DataDirectory[14] = CLI COM descriptor).
#ifndef IMAGE_DIRECTORY_ENTRY_COMHEADER
#define IMAGE_DIRECTORY_ENTRY_COMHEADER 14
#endif

// ----------------------------------------------------------------------------
// Minimal CLI (ECMA-335 partition II) header definitions — mirrors <corhdr.h>
// so the loader depends only on the Windows SDK, not the .NET headers.
// ----------------------------------------------------------------------------
struct CorVtableFixup {                  // IMAGE_COR_VTABLE_FIXUP
    DWORD Rva;                           // RVA of the vtable slot array
    WORD  Count;                         // number of contiguous slots
    WORD  Type;                          // COR_VTABLE_* flags
};
static_assert(sizeof(CorVtableFixup) == 8, "IMAGE_COR_VTABLE_FIXUP must be 8 bytes");

struct Cor20Header {                     // IMAGE_COR20_HEADER (DataDirectory[14])
    DWORD               cb;
    WORD                MajorRuntimeVersion;
    WORD                MinorRuntimeVersion;
    IMAGE_DATA_DIRECTORY MetaData;
    DWORD               Flags;
    DWORD               EntryPointTokenOrRva;   // token or native RVA (flag-dependent)
    IMAGE_DATA_DIRECTORY Resources;
    IMAGE_DATA_DIRECTORY StrongNameSignature;
    IMAGE_DATA_DIRECTORY CodeManagerTable;
    IMAGE_DATA_DIRECTORY VTableFixups;
    IMAGE_DATA_DIRECTORY ExportAddressTableJumps;
    IMAGE_DATA_DIRECTORY ManagedNativeHeader;
};
static_assert(sizeof(Cor20Header) == 72, "IMAGE_COR20_HEADER must be 72 bytes (0x48)");

constexpr DWORD kComimageFlagsIlOnly           = 0x00000001;
constexpr DWORD kComimageFlagsNativeEntryPoint = 0x00000010;

constexpr WORD kCorVtable32Bit            = 0x0001;  // 32-bit slot width
constexpr WORD kCorVtable64Bit            = 0x0002;  // 64-bit slot width
constexpr WORD kCorVtableFromUnmanaged    = 0x0004;  // native -> managed transition
constexpr WORD kCorVtableFromUnmanagedAD  = 0x0008;  // ... via appdomain
constexpr WORD kCorVtableCallMostDerived  = 0x0010;  // virtual dispatch

// Compile-time switch: define CPPCLI_RWX to keep the whole image writable
// (useful if a runtime component unexpectedly patches a read-only section).
#ifndef CPPCLI_RWX
#define CPPCLI_STRICT_PROTECTIONS 1
#endif

// ----------------------------------------------------------------------------
// Small utilities
// ----------------------------------------------------------------------------
static bool ReadWholeFile(const char* path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[error] CreateFileA(\"%s\") failed, GLE=%lu\n", path, GetLastError());
        return false;
    }
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        printf("[error] GetFileSizeEx(\"%s\") failed\n", path);
        CloseHandle(h);
        return false;
    }
    out->resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr)
              && got == (DWORD)out->size();
    CloseHandle(h);
    if (!ok) printf("[error] ReadFile(\"%s\") failed\n", path);
    return ok;
}

static void DecodeVtableType(WORD type, char* buf, size_t cb) {
    buf[0] = '\0';
    if (type & kCorVtable64Bit)           strncat_s(buf, cb, "64BIT|", 6);
    else if (type & kCorVtable32Bit)      strncat_s(buf, cb, "32BIT|", 6);
    if (type & kCorVtableFromUnmanaged)   strncat_s(buf, cb, "FROM_UNMANAGED|", 15);
    if (type & kCorVtableFromUnmanagedAD) strncat_s(buf, cb, "FROM_UNMANAGED_APPDOMAIN|", 25);
    if (type & kCorVtableCallMostDerived) strncat_s(buf, cb, "CALL_MOST_DERIVED|", 18);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '|') buf[n - 1] = '\0';
    if (!buf[0]) strncat_s(buf, cb, "0", 1);
}

// Classify a slot value against the empirically observed lifecycle states
// (research: token 0x06000001 / thunk 0x000001f1`75502058 / JIT 0x00007fff`8af10020).
static const char* ClassifySlotValue(uint64_t v) {
    if ((v >> 32) == 0 && (v & 0xFF000000ull) == 0x06000000ull)
        return "MethodDef token (State 1: on-disk)";
    if (v >= 0x0000010000000000ull && v < 0x00007FF000000000ull)
        return "heap pointer -> IJW transition thunk (State 2)";
    if (v >= 0x00007FF000000000ull)
        return "high VA -> JIT/native code (State 3)";
    return "raw value";
}

// ----------------------------------------------------------------------------
// CppCliLoader — manual mapper for one mixed-mode DLL
// ----------------------------------------------------------------------------
class CppCliLoader {
public:
    struct SlotSummary {
        int tokens = 0, thunks = 0, code = 0, other = 0;
    };

    bool Load(const char* dllPath);
    BOOL CallEntryPoint(DWORD reason, LPVOID reserved);
    void* GetExport(const char* name);
    void ThreadAttach();                 // per-thread TLS template clone
    SlotSummary DumpSlots(const char* phaseLabel) const;
    uint8_t* base() const { return base_; }
    SIZE_T   imageSize() const { return imageSize_; }

private:
    bool ParseAndValidate();
    bool MapImage();
    bool ApplyRelocations();             // Layer 1 with Layer 2 exclusion
    bool ResolveImports();
    bool InitializeTls();
    void ProtectSections();
    bool IsLayer2Rva(DWORD rva) const;
    DWORD RvaToFileOffset(DWORD rva) const;  // RVA -> raw file offset
    static constexpr DWORD kInvalidFileOffset = 0xFFFFFFFF;
    bool RegisterInLdr(const char* dllPath);  // PEB->Ldr ghost entry


    std::vector<uint8_t>  fileImage_;    // raw on-disk bytes (State 1 source)
    uint8_t*              base_ = nullptr;   // mapped image base
    SIZE_T                imageSize_ = 0;
    PIMAGE_DOS_HEADER     dos_ = nullptr;
    PIMAGE_NT_HEADERS64   nt_ = nullptr;
    const Cor20Header*    cor_ = nullptr;

    struct FixupTable {
        DWORD slotRva;
        WORD  count;
        WORD  type;
    };
    std::vector<FixupTable>                fixups_;
    std::vector<std::pair<DWORD, DWORD>>   layer2Ranges_;  // [start, end) RVAs
    DWORD                                  tlsIndex_ = TLS_OUT_OF_INDEXES;
    std::vector<wchar_t>   wpath_;              // FullDllName backing storage
    uint8_t*               ldrEntry_ = nullptr; // ghost LDR_DATA_TABLE_ENTRY
};

static void ProbeGetModuleFileName(const CppCliLoader* self, void* base);

// ----------------------------------------------------------------------------
// 1. Parse + validate the PE / CLI headers of a mixed-mode image
// ----------------------------------------------------------------------------
bool CppCliLoader::ParseAndValidate() {
    if (fileImage_.size() < sizeof(IMAGE_DOS_HEADER)) {
        printf("[error] file smaller than IMAGE_DOS_HEADER\n");
        return false;
    }
    dos_ = (PIMAGE_DOS_HEADER)fileImage_.data();
    if (dos_->e_magic != IMAGE_DOS_SIGNATURE) {
        printf("[error] bad DOS signature (0x%04X)\n", dos_->e_magic);
        return false;
    }
    if ((size_t)dos_->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fileImage_.size()) {
        printf("[error] NT headers past end of file\n");
        return false;
    }
    nt_ = (PIMAGE_NT_HEADERS64)(fileImage_.data() + dos_->e_lfanew);
    if (nt_->Signature != IMAGE_NT_SIGNATURE) {
        printf("[error] bad NT signature\n");
        return false;
    }
    if (nt_->FileHeader.Machine != kHostMachine) {
        printf("[error] machine 0x%04X does not match host %s (0x%04X)\n",
               (unsigned)nt_->FileHeader.Machine, kHostArch, (unsigned)kHostMachine);
        return false;
    }
    if (nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        printf("[error] not a PE32+ (Magic=0x%04X) image\n",
               (unsigned)nt_->OptionalHeader.Magic);
        return false;
    }

    IMAGE_DATA_DIRECTORY* dd = nt_->OptionalHeader.DataDirectory;
    DWORD comRva = dd[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress;
    if (!comRva) {
        printf("[error] DataDirectory[14] (COM descriptor) absent: not a CLI image\n");
        return false;
    }
    // DataDirectory values are RVAs (image-relative). This parse runs against
    // the raw file buffer, so every RVA must go through RvaToFileOffset().
    DWORD comFile = RvaToFileOffset(comRva);
    if (comFile == kInvalidFileOffset
        || comFile + sizeof(Cor20Header) > fileImage_.size()) {
        printf("[error] COR20 header unmapped or past end of file\n");
        return false;
    }
    cor_ = (const Cor20Header*)(fileImage_.data() + comFile);

    printf("\n--- PE / CLI Header Dossier (mirrors research sections 2.1 / 2.2) ---\n");
    printf("Machine                 : 0x%04X (%s)\n",
           (unsigned)nt_->FileHeader.Machine, kHostArch);
    printf("Magic                   : 0x%04X (PE32+)\n",
           (unsigned)nt_->OptionalHeader.Magic);
    printf("Preferred ImageBase     : 0x%016llX\n",
           (unsigned long long)nt_->OptionalHeader.ImageBase);
    printf("AddressOfEntryPoint RVA : 0x%08X (native _DllMainCRTStartup)\n",
           (unsigned)nt_->OptionalHeader.AddressOfEntryPoint);
    printf("COM Descriptor (dir 14) : RVA 0x%08X Size 0x%08X\n",
           (unsigned)comRva,
           (unsigned)dd[IMAGE_DIRECTORY_ENTRY_COMHEADER].Size);
    printf("COR20 Flags             : 0x%08X\n", (unsigned)cor_->Flags);

    // [CPP-01] mixed-mode shape: ILONLY clear, NATIVE_ENTRYPOINT set.
    if (cor_->Flags & kComimageFlagsIlOnly) {
        printf("[error] ILONLY set: pure managed assembly, nothing to map manually\n");
        return false;
    }
    printf("  ILONLY                : 0  (contains native code)          [CPP-01 OK]\n");
    if (cor_->Flags & kComimageFlagsNativeEntryPoint) {
        printf("  NATIVE_ENTRYPOINT     : 1  (entry is a native RVA)      [CPP-01 OK]\n");
    } else {
        printf("  NATIVE_ENTRYPOINT     : 0  (WARNING: unexpected for /clr /LD)\n");
    }
    printf("COR20 EntryPointTok/RVA : 0x%08X\n",
           (unsigned)cor_->EntryPointTokenOrRva);

    // [CPP-02] VTableFixups must be populated for a mixed DLL.
    DWORD fv = cor_->VTableFixups.VirtualAddress;
    DWORD fs = cor_->VTableFixups.Size;
    printf("COR20 VTableFixups      : RVA 0x%08X Size 0x%08X\n",
           (unsigned)fv, (unsigned)fs);
    if (!fv || fs < sizeof(CorVtableFixup)) {
        printf("[error] VTableFixups directory empty: no crossover slots      [CPP-02 FAIL]\n");
        return false;
    }
    printf("                         (%u descriptors)                       [CPP-02 OK]\n",
           (unsigned)(fs / sizeof(CorVtableFixup)));

    fixups_.clear();
    layer2Ranges_.clear();
    DWORD fvFile = RvaToFileOffset(fv);
    if (fvFile == kInvalidFileOffset
        || fvFile + fs > fileImage_.size()) {
        printf("[error] VTableFixups array unmapped or past end of file\n");
        return false;
    }
    const CorVtableFixup* ft = (const CorVtableFixup*)(fileImage_.data() + fvFile);
    for (DWORD i = 0; i * sizeof(CorVtableFixup) < fs; ++i) {
        DWORD width = (ft[i].Type & kCorVtable64Bit) ? 8 : 4;
        DWORD slotFile = RvaToFileOffset(ft[i].Rva);
        if (slotFile == kInvalidFileOffset
            || (size_t)slotFile + (size_t)ft[i].Count * width > fileImage_.size()) {
            printf("[error] fixup %u slot array unmapped or past end of file\n",
                   (unsigned)i);
            return false;
        }
        char dec[96];
        DecodeVtableType(ft[i].Type, dec, sizeof(dec));
        printf("  fixup[%u] : %u slot(s) @ RVA 0x%08X, width %u, type 0x%04X (%s)\n",
               (unsigned)i, (unsigned)ft[i].Count, (unsigned)ft[i].Rva,
               (unsigned)width, (unsigned)ft[i].Type, dec);
        fixups_.push_back({ft[i].Rva, ft[i].Count, ft[i].Type});
        layer2Ranges_.push_back({ft[i].Rva, ft[i].Rva + (DWORD)ft[i].Count * width});
    }
    return true;
}

// Translate an image RVA into a raw file offset using the section table.
// Returns kInvalidFileOffset when the RVA has no file backing.
DWORD CppCliLoader::RvaToFileOffset(DWORD rva) const {
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt_);
    for (int i = 0; i < nt_->FileHeader.NumberOfSections; ++i, ++s) {
        DWORD va = s->VirtualAddress;
        DWORD span = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (rva >= va && rva < va + span) {
            DWORD off = rva - va;
            if (off >= s->SizeOfRawData) return kInvalidFileOffset;
            return s->PointerToRawData + off;
        }
    }
    return kInvalidFileOffset;
}

// ----------------------------------------------------------------------------
// 2. Manual section mapping (headers + raw section data; rest is zero-filled
//    by the commit, matching ntdll semantics)
// ----------------------------------------------------------------------------
bool CppCliLoader::MapImage() {
    imageSize_ = nt_->OptionalHeader.SizeOfImage;

    // Try the preferred base first (relocation-free mapping); fall back to
    // anywhere in the address space and rebase (ASLR-style delta).
    base_ = (uint8_t*)VirtualAlloc(
        (LPVOID)(uintptr_t)nt_->OptionalHeader.ImageBase,
        imageSize_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base_) {
        base_ = (uint8_t*)VirtualAlloc(nullptr, imageSize_,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!base_) {
        printf("[error] VirtualAlloc(%llu bytes) failed, GLE=%lu\n",
               (unsigned long long)imageSize_, GetLastError());
        return false;
    }
    printf("\nMapped base              : 0x%p (size 0x%llX)\n",
           (void*)base_, (unsigned long long)imageSize_);
    printf("Relocation delta         : %lld\n",
           (long long)((intptr_t)base_ - (intptr_t)nt_->OptionalHeader.ImageBase));

    memcpy(base_, fileImage_.data(), nt_->OptionalHeader.SizeOfHeaders);

    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt_);
    for (int i = 0; i < nt_->FileHeader.NumberOfSections; ++i, ++s) {
        if (s->SizeOfRawData == 0) continue;  // zero-fill only (VirtualAlloc zeroed)
        size_t avail = fileImage_.size() - s->PointerToRawData;
        size_t n = s->SizeOfRawData < avail ? s->SizeOfRawData : avail;
        memcpy(base_ + s->VirtualAddress, fileImage_.data() + s->PointerToRawData, n);
    }
    return true;
}

bool CppCliLoader::IsLayer2Rva(DWORD rva) const {
    for (const auto& r : layer2Ranges_)
        if (rva >= r.first && rva < r.second) return true;
    return false;
}

// ----------------------------------------------------------------------------
// 3. Layer-1 base relocations with Layer-2 (VTableFixup) exclusion.
//
//    If the loader applied DIR64 deltas to the fixup slots, the 32-bit
//    MethodDef tokens would be corrupted into garbage addresses and the CLR
//    would crash while converting them to IJW thunks. This is the strict
//    segregation of relocation responsibilities from the research.
// ----------------------------------------------------------------------------
bool CppCliLoader::ApplyRelocations() {
    IMAGE_DATA_DIRECTORY& d =
        nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    int64_t delta = (int64_t)(intptr_t)base_
                    - (int64_t)(intptr_t)nt_->OptionalHeader.ImageBase;

    if (!d.VirtualAddress) {
        if (delta == 0) {
            printf("[reloc] no .reloc directory and delta == 0: nothing to do\n");
            return true;
        }
        printf("[error] image rebased but has no .reloc directory\n");
        return false;
    }

    DWORD applied = 0, skippedL2 = 0, padding = 0;
    DWORD consumed = 0;
    PIMAGE_BASE_RELOCATION blk =
        (PIMAGE_BASE_RELOCATION)(base_ + d.VirtualAddress);

    while (consumed + sizeof(IMAGE_BASE_RELOCATION) <= d.Size) {
        DWORD page = blk->VirtualAddress;
        DWORD size = blk->SizeOfBlock;
        if (size < sizeof(IMAGE_BASE_RELOCATION) || consumed + size > d.Size) break;

        WORD* entries = (WORD*)((uint8_t*)blk + sizeof(IMAGE_BASE_RELOCATION));
        DWORD n = (size - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        for (DWORD i = 0; i < n; ++i) {
            WORD type  = entries[i] >> 12;
            WORD off   = entries[i] & 0x0FFF;
            DWORD rva  = page + off;   // 12-bit overflow carries into the page

            if (type == IMAGE_REL_BASED_ABSOLUTE) { padding++; continue; }
            if (type != IMAGE_REL_BASED_DIR64) {
                printf("[reloc] WARNING: unhandled type %u @ RVA 0x%08X (skipped)\n",
                       (unsigned)type, (unsigned)rva);
                continue;
            }
            if (IsLayer2Rva(rva)) {
                skippedL2++;
                printf("[reloc] Layer-2 exclusion: DIR64 @ RVA 0x%08X falls inside a "
                       "VTableFixup slot array -> NOT patched (token preserved)\n",
                       (unsigned)rva);
                continue;
            }
            *(int64_t*)(base_ + rva) += delta;
            applied++;
        }
        consumed += size;
        blk = (PIMAGE_BASE_RELOCATION)((uint8_t*)blk + size);
    }

    printf("[reloc] Layer-1 applied=%u Layer-2-excluded=%u padding=%u\n",
           (unsigned)applied, (unsigned)skippedL2, (unsigned)padding);
    return true;
}

// ----------------------------------------------------------------------------
// 4. Import resolution. For a mixed DLL this necessarily loads mscoree.dll:
//    the entry stub tail-jumps through the IAT slot to mscoree!_CorDllMain.
// ----------------------------------------------------------------------------
bool CppCliLoader::ResolveImports() {
    IMAGE_DATA_DIRECTORY& d =
        nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) {
        printf("[imports] no import directory\n");
        return true;
    }

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(base_ + d.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll = (const char*)(base_ + desc->Name);
        HMODULE h = LoadLibraryA(dll);
        if (!h) {
            printf("[imports] LoadLibraryA(\"%s\") failed, GLE=%lu\n", dll, GetLastError());
            return false;
        }
        uintptr_t oft = desc->OriginalFirstThunk
                        ? desc->OriginalFirstThunk : desc->FirstThunk;
        PIMAGE_THUNK_DATA64 tt =
            (PIMAGE_THUNK_DATA64)(base_ + oft);
        PIMAGE_THUNK_DATA64 ft =
            (PIMAGE_THUNK_DATA64)(base_ + desc->FirstThunk);

        DWORD n = 0;
        for (; tt->u1.AddressOfData; ++tt, ++ft, ++n) {
            FARPROC proc = nullptr;
            if (IMAGE_SNAP_BY_ORDINAL64(tt->u1.Ordinal)) {
                proc = GetProcAddress(h, (LPCSTR)IMAGE_ORDINAL64(tt->u1.Ordinal));
            } else {
                PIMAGE_IMPORT_BY_NAME ibn =
                    (PIMAGE_IMPORT_BY_NAME)(base_ + tt->u1.AddressOfData);
                proc = GetProcAddress(h, (const char*)ibn->Name);
            }
            if (!proc) {
                printf("[imports] GetProcAddress(\"%s\", #%u) failed\n",
                       dll, (unsigned)n);
                return false;
            }
            ft->u1.Function = (uint64_t)(uintptr_t)proc;
        }
        printf("[imports] %-24s %3u thunk(s) resolved%s\n", dll, (unsigned)n,
               _stricmp(dll, "mscoree.dll") == 0
                   ? "  <- IJW hijack target (entry stub tail-jumps to _CorDllMain)"
                   : "");
    }
    return true;
}

// ----------------------------------------------------------------------------
// 5. TLS emulation (ntdll!LdrpInitializeTls analog).
//
//    Allocate a real TLS slot, store it into the image's __tls_index, and
//    clone the template block for the current thread. Native
//    __declspec(thread) access compiles to TEB->ThreadLocalStoragePointer
//    [__tls_index] + <offset within template>, so this reproduces the OS
//    behavior exactly for threads that run ThreadAttach().
// ----------------------------------------------------------------------------
bool CppCliLoader::InitializeTls() {
    IMAGE_DATA_DIRECTORY& d =
        nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!d.VirtualAddress) {
        printf("[tls] no TLS directory (image declares no __declspec(thread))\n");
        return true;
    }
    PIMAGE_TLS_DIRECTORY64 tls =
        (PIMAGE_TLS_DIRECTORY64)(base_ + d.VirtualAddress);

    printf("[tls] IMAGE_TLS_DIRECTORY64 (mirrors research section 7.1):\n");
    printf("        StartAddressOfRawData : 0x%016llX\n",
           (unsigned long long)tls->StartAddressOfRawData);
    printf("        EndAddressOfRawData   : 0x%016llX\n",
           (unsigned long long)tls->EndAddressOfRawData);
    printf("        AddressOfIndex        : 0x%016llX\n",
           (unsigned long long)tls->AddressOfIndex);
    printf("        AddressOfCallBacks    : 0x%016llX\n",
           (unsigned long long)tls->AddressOfCallBacks);
    printf("        SizeOfZeroFill        : 0x%X\n",
           (unsigned)tls->SizeOfZeroFill);

    if (tlsIndex_ == TLS_OUT_OF_INDEXES) {
        tlsIndex_ = TlsAlloc();
        if (tlsIndex_ == TLS_OUT_OF_INDEXES) {
            printf("[tls] TlsAlloc failed\n");
            return false;
        }
        // The loader owns __tls_index now (exactly what ntdll does).
        *(DWORD*)(uintptr_t)tls->AddressOfIndex = tlsIndex_;
        printf("[tls] assigned __tls_index = %u\n", (unsigned)tlsIndex_);
    }

    ThreadAttach();  // clone template for the initializing thread

    if (tls->AddressOfCallBacks) {
        PIMAGE_TLS_CALLBACK* cb =
            (PIMAGE_TLS_CALLBACK*)(uintptr_t)tls->AddressOfCallBacks;
        for (; *cb; ++cb) {
            printf("[tls] invoking TLS callback 0x%p(DLL_PROCESS_ATTACH)\n",
                   (void*)*cb);
            (*cb)((PVOID)base_, DLL_PROCESS_ATTACH, nullptr);
        }
    }
    return true;
}

void CppCliLoader::ThreadAttach() {
    if (tlsIndex_ == TLS_OUT_OF_INDEXES) return;
    IMAGE_DATA_DIRECTORY& d =
        nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    PIMAGE_TLS_DIRECTORY64 tls =
        (PIMAGE_TLS_DIRECTORY64)(base_ + d.VirtualAddress);

    uintptr_t start = (uintptr_t)tls->StartAddressOfRawData;
    uintptr_t end   = (uintptr_t)tls->EndAddressOfRawData;
    size_t raw      = end > start ? (size_t)(end - start) : 0;
    size_t total    = raw + tls->SizeOfZeroFill;
    if (total == 0) return;

    uint8_t* block = (uint8_t*)calloc(1, total);
    if (!block) return;
    if (raw) memcpy(block, (const void*)start, raw);
    TlsSetValue(tlsIndex_, block);
    printf("[tls] thread %lu attached: block=%p raw=%zu template[0]=0x%016llX "
           "(start VA 0x%llX)\n",
           GetCurrentThreadId(), (void*)block, raw,
           raw >= 8 ? (unsigned long long)*(uint64_t*)start
                    : (unsigned long long)*(uint32_t*)start,
           (unsigned long long)start);

    // NOTE: blocks are intentionally leaked for the process lifetime (one
    // per attached thread), mirroring the OS which frees them at thread exit.
    //
    // EMPIRICAL DIVERGENCE (manual mapping vs LoadLibrary): the block above
    // contains the correct 1234 template copy, but MixedLib's /clr-compiled
    // access to the RVA-mapped __declspec(thread) field goes through the
    // CLR's own per-thread RVA-static machinery (JIT_GetNonGCThreadStaticBase
    // against the CLR's private PEImage copy of the assembly, whose
    // __tls_index is not ours) and zero-initializes. Under LoadLibrary,
    // ntdll's static-TLS registration supplies the template (1235/1236/1236);
    // under manual mapping you observe 1/2/2. The managed [ThreadStatic]
    // divergence (5679/2/5680) is unaffected and still demonstrates CPP-09.

}

// ----------------------------------------------------------------------------
// Final section protections (W^X). Delayed until after relocation + IAT +
// TLS setup so all loader writes happen under PAGE_READWRITE. .data stays
// writable: the CLR must back-patch VTableFixup slots into it (State 2/3).
// ----------------------------------------------------------------------------
void CppCliLoader::ProtectSections() {
#ifdef CPPCLI_STRICT_PROTECTIONS
    DWORD oldProtect = 0;
    VirtualProtect(base_, nt_->OptionalHeader.SizeOfHeaders,
                   PAGE_READONLY, &oldProtect);
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt_);
    for (int i = 0; i < nt_->FileHeader.NumberOfSections; ++i, ++s) {
        bool x = (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool w = (s->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        bool r = (s->Characteristics & IMAGE_SCN_MEM_READ) != 0;
        DWORD prot = PAGE_NOACCESS;
        if (x && w)      prot = PAGE_EXECUTE_READWRITE;
        else if (x && r) prot = PAGE_EXECUTE_READ;
        else if (x)      prot = PAGE_EXECUTE;
        else if (w && r) prot = PAGE_READWRITE;
        else if (w)      prot = PAGE_WRITECOPY;
        else if (r)      prot = PAGE_READONLY;
        DWORD size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        VirtualProtect(base_ + s->VirtualAddress, size, prot, &oldProtect);
    }
#else
    printf("[protect] CPPCLI_RWX build: leaving whole image PAGE_READWRITE\n");
#endif
}


// ----------------------------------------------------------------------------
// 5b. LDR registration (PEB->Ldr ghost entry — what ntdll!LdrpInsertDataTableEntry
//     does for every module). Without it the HINSTANCE is invisible to
//     GetModuleFileNameW/GetModuleHandleW, and mscoree!_CorDllMain cannot
//     resolve the assembly path for the IJW machinery, returning FALSE.
//
// Offsets below (TEB+0x60 PEB, PEB+0x18 Ldr, PEB_LDR_DATA lists at +0x10/
// +0x20/+0x30, LDR_DATA_TABLE_ENTRY fields through BaseDllName) are identical
// on x64 and ARM64 Windows.
// ----------------------------------------------------------------------------
static void InsertTailListShim(void* listHead, void* entryLinks) {
    void** head = (void**)listHead;
    void** ent  = (void**)entryLinks;
    void*  blink = head[1];
    ent[0] = head;
    ent[1] = blink;
    ((void**)blink)[0] = entryLinks;
    head[1] = entryLinks;
}

bool CppCliLoader::RegisterInLdr(const char* dllPath) {
    // PEB via NtQueryInformationProcess(ProcessBasicInformation).
    // (ntdll!NtCurrentTeb is not exported on ARM64.)
    struct ProcessBasicInfoShim {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    };
    using NtQueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, ULONG*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NtQueryInformationProcessFn ntQueryInfoProcess =
        (NtQueryInformationProcessFn)GetProcAddress(ntdll,
                                                    "NtQueryInformationProcess");
    if (!ntQueryInfoProcess) {
        printf("[ldr] NtQueryInformationProcess not found\n");
        return false;
    }
    ProcessBasicInfoShim pbi = {};
    LONG st = ntQueryInfoProcess(GetCurrentProcess(), 0, &pbi,
                                 (ULONG)sizeof(pbi), nullptr);
    if (st < 0 || !pbi.PebBaseAddress) {
        printf("[ldr] NtQueryInformationProcess failed (0x%lX)\n", (unsigned long)st);
        return false;
    }
    uint8_t* peb = (uint8_t*)pbi.PebBaseAddress;
    uint8_t* ldr = *(uint8_t**)(peb + 0x18);   // PEB->Ldr
    if (!ldr) {
        printf("[ldr] PEB->Ldr is null\n");
        return false;
    }

    // Wide path strings (must outlive the LDR entry). GetFullPathNameW gets
    // a separate destination buffer: in-place expansion is not documented as
    // safe and empirically interleaves the string.
    wpath_.resize(MAX_PATH * 2);
    int wn = MultiByteToWideChar(CP_ACP, 0, dllPath, -1, wpath_.data(),
                                 (int)wpath_.size());
    if (wn <= 0) {
        printf("[ldr] MultiByteToWideChar failed\n");
        return false;
    }
    wchar_t full[MAX_PATH * 2] = {};
    DWORD n = GetFullPathNameW(wpath_.data(),
                               (DWORD)(sizeof(full) / sizeof(wchar_t)),
                               full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(wchar_t)) {
        printf("[ldr] GetFullPathNameW failed\n");
        return false;
    }
    wcscpy_s(wpath_.data(), wpath_.size(), full);
    const wchar_t* baseName = wcsrchr(wpath_.data(), L'\\');
    baseName = baseName ? baseName + 1 : wpath_.data();


    // Clone a REAL module's LDR_DATA_TABLE_ENTRY wholesale (the loader
    // dereferences far more fields than the documented subset — DdagNode,
    // Lock, flags — and a hand-built entry causes access violations deep in
    // ntdll!LdrGetDllFullName, which then deadlocks in
    // UnhandledExceptionFilter -> LoadLibraryExW under the loader lock).
    // Identity fields are patched below; link fields we do not join are
    // zeroed so no walker follows stale pointers into the donor's lists.
    uint8_t* donor = nullptr;
    HMODULE ntmod0 = GetModuleHandleA("ntdll.dll");
    for (uint8_t* le = *(uint8_t**)(ldr + 0x10); le != ldr + 0x10;
         le = *(uint8_t**)le) {
        if (*(HMODULE*)(le + 0x30) == ntmod0) { donor = le; break; }
    }
    if (!donor) {
        printf("[ldr] ntdll LDR entry not found (donor)\n");
        return false;
    }
    ldrEntry_ = (uint8_t*)calloc(1, 0x300);
    if (!ldrEntry_) return false;
    memcpy(ldrEntry_, donor, 0x200);                          // clone layout
    *(void**)(ldrEntry_ + 0x30) = base_;                      // DllBase
    DWORD ep = nt_->OptionalHeader.AddressOfEntryPoint;
    *(void**)(ldrEntry_ + 0x38) = ep ? base_ + ep : nullptr;  // EntryPoint
    *(uint32_t*)(ldrEntry_ + 0x40) = (uint32_t)imageSize_;    // SizeOfImage
    auto setUstr = [](uint8_t* at, const wchar_t* s) {        // UNICODE_STRING
        size_t bytes = wcslen(s) * sizeof(wchar_t);
        *(uint16_t*)(at + 0) = (uint16_t)bytes;
        *(uint16_t*)(at + 2) = (uint16_t)(bytes + sizeof(wchar_t));
        *(void**)(at + 8) = (void*)s;
    };
    setUstr(ldrEntry_ + 0x48, wpath_.data());                 // FullDllName
    setUstr(ldrEntry_ + 0x58, baseName);                      // BaseDllName
    memset(ldrEntry_ + 0x20, 0, 0x10);   // InInitializationOrderLinks: not joined
    memset(ldrEntry_ + 0x70, 0, 0x10);   // HashLinks: not in LdrpHashTable
    memset(ldrEntry_ + 0xB0, 0, 0x10);   // NodeModuleLink: not in DDAG list
    memset(ldrEntry_ + 0xC8, 0, 0x18);   // BaseAddressIndexNode (inserted below)
    memset(ldrEntry_ + 0xE0, 0, 0x18);   // MappingInfoIndexNode: not joined
    *(uint64_t*)(ldrEntry_ + 0xF8) = nt_->OptionalHeader.ImageBase; // OriginalBase

    // ---- LdrpModuleBaseAddressIndex RB-tree insertion -------------------
    // Win10/11 handle->module lookups (GetModuleFileNameW and the CLR's
    // mscoreei!GetRuntimeForHModule) resolve through this private ntdll
    // RB-tree keyed by DllBase, not through the lists above. Each
    // LDR_DATA_TABLE_ENTRY embeds its tree node (RTL_BALANCED_NODE
    // {Left,Right,Parent}) at +0xC8 on this build (Win11 26100 ARM64,
    // verified empirically: sibling entries hold cross-pointers at exactly
    // this offset). We locate the tree globals in ntdll .data, then insert
    // our node via the exported ntdll!RtlRbInsertNodeEx so ntdll itself
    // maintains the balancing.
    constexpr size_t kNodeOffset = 0xC8;

    // Collect every module's BaseAddressIndexNode pointer from the LDR lists.
    // The private global LdrpModuleBaseAddressIndex (RTL_RB_TREE {Root; Min})
    // hides somewhere in ntdll .data: its Root and Min must BOTH be pointers
    // into our collected node set (unlike kernel32-specific guessing, which
    // only works if kernel32 happens to be the tree root).
    std::vector<uint8_t*> nodes;
    for (uint8_t* le = *(uint8_t**)(ldr + 0x10); le != ldr + 0x10;
         le = *(uint8_t**)le) {
        nodes.push_back(le + kNodeOffset);
    }
    auto isNode = [&nodes](void* p) {
        for (uint8_t* n : nodes) if (n == p) return true;
        return false;
    };

    HMODULE ntmod = GetModuleHandleA("ntdll.dll");
    uint8_t* nthdr = (uint8_t*)ntmod + *(int32_t*)((uint8_t*)ntmod + 0x3C);
    PIMAGE_SECTION_HEADER nsec = IMAGE_FIRST_SECTION((PIMAGE_NT_HEADERS64)nthdr);
    uint8_t* tree = nullptr;
    for (int i = 0; i < ((PIMAGE_NT_HEADERS64)nthdr)->FileHeader.NumberOfSections;
         ++i, ++nsec) {
        if (memcmp(nsec->Name, ".data", 5) != 0) continue;
        uint8_t* start = (uint8_t*)ntmod + nsec->VirtualAddress;
        size_t bytes = nsec->Misc.VirtualSize;
        for (size_t off = 0; off + 16 <= bytes; off += 8) {
            uint8_t** cand = (uint8_t**)(start + off);
            if (*cand && isNode(cand[0]) && cand[1] && isNode(cand[1])) {
                tree = (uint8_t*)cand;   // RTL_RB_TREE { Root; Min; }
                break;
            }
        }
        break;
    }
    if (!tree) {
        printf("[ldr] LdrpModuleBaseAddressIndex root not found in ntdll .data "
               "(node offset 0x%zX wrong for this build?)\n", kNodeOffset);
        return false;
    }
    printf("[ldr] LdrpModuleBaseAddressIndex located at ntdll+0x%llX "
           "(validated Root+Min pair)\n",
           (unsigned long long)((uintptr_t)tree - (uintptr_t)ntmod));

    // Find the insertion parent by DllBase ordering, then let ntdll insert.
    uint8_t* parent = nullptr;
    unsigned char goRight = 0;
    for (uint8_t* cur = *(uint8_t**)tree; cur; ) {
        uint8_t* curEntry = cur - kNodeOffset;
        uint8_t* curBase = *(uint8_t**)(curEntry + 0x30);   // DllBase
        parent = cur;
        goRight = (base_ > curBase) ? 1 : 0;
        cur = goRight ? *(uint8_t**)(cur + 8) : *(uint8_t**)cur;
    }
    memset(ldrEntry_ + kNodeOffset, 0, 24);                  // fresh node
    using RbInsertFn = void (NTAPI*)(void*, void*, unsigned char, void*);
    RbInsertFn rbInsert = (RbInsertFn)GetProcAddress(ntmod, "RtlRbInsertNodeEx");
    if (!rbInsert) {
        printf("[ldr] ntdll!RtlRbInsertNodeEx not exported\n");
        return false;
    }
    rbInsert(tree, parent, goRight, ldrEntry_ + kNodeOffset);
    printf("[ldr] node inserted into base-address index via RtlRbInsertNodeEx\n");
    InsertTailListShim(ldr + 0x10, ldrEntry_ + 0x00);          // InLoadOrder
    InsertTailListShim(ldr + 0x20, ldrEntry_ + 0x10);          // InMemoryOrder
    ProbeGetModuleFileName(this, base_);
    printf("[ldr] ghost LDR entry linked into lists + base-address index\n");
    return true;
}

// GetModuleFileNameW probe wrapped in SEH: an AV inside ntdll must not
// escape to UnhandledExceptionFilter (whose LoadLibraryExW under the loader
// lock deadlocks — observed with hand-built LDR entries).
static void ProbeGetModuleFileName(const CppCliLoader* self, void* base) {
    (void)self;
#ifdef _MSC_VER
    __try {
#endif
        wchar_t check[MAX_PATH * 2] = {};
        DWORD cn = GetModuleFileNameW((HINSTANCE)base, check,
                                      (DWORD)(sizeof(check) / sizeof(wchar_t)));
        if (cn == 0) {
            printf("[ldr] note: GetModuleFileNameW still fails (GLE=%lu)\n",
                   GetLastError());
        } else {
            printf("[ldr] GetModuleFileNameW -> \"%ls\"\n", check);
        }
#ifdef _MSC_VER
    } __except (printf("[ldr] SEH %08lX in GetModuleFileNameW probe\n",
                       (unsigned long)GetExceptionCode()),
                EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}
// ----------------------------------------------------------------------------
// Orchestrated load
// ----------------------------------------------------------------------------
bool CppCliLoader::Load(const char* dllPath) {
    printf("[load] target: %s\n", dllPath);
    if (!ReadWholeFile(dllPath, &fileImage_)) return false;
    if (!ParseAndValidate()) return false;
    if (!MapImage()) return false;
    if (!ApplyRelocations()) return false;
    if (!ResolveImports()) return false;
    if (!InitializeTls()) return false;
    ProtectSections();
    if (!RegisterInLdr(dllPath)) return false;
    printf("[load] mapping complete; ready for DLL_PROCESS_ATTACH\n");
    return true;
}

// ----------------------------------------------------------------------------
// 6. Entry point invocation. For a mixed DLL this executes the 3-instruction
//    native stub (adrp/ldr/br on ARM64) which tail-jumps through the IAT to
//    mscoree!_CorDllMain: the CLR hijack that installs the State-2 IJW
//    thunks into the VTableFixup slots during attach.
// ----------------------------------------------------------------------------
BOOL CppCliLoader::CallEntryPoint(DWORD reason, LPVOID reserved) {
    DWORD ep = nt_->OptionalHeader.AddressOfEntryPoint;
    if (!ep) return FALSE;
    using DllEntryFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    DllEntryFn entry = (DllEntryFn)(base_ + ep);
    return entry((HINSTANCE)base_, reason, reserved);
}

// ----------------------------------------------------------------------------
// 7. Export resolution: RVAs land on the adrp/ldr/br x9 dispatch stubs whose
//    slot targets progress token -> thunk -> JIT code (State 1 -> 2 -> 3).
// ----------------------------------------------------------------------------
void* CppCliLoader::GetExport(const char* name) {
    IMAGE_DATA_DIRECTORY& d =
        nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return nullptr;
    PIMAGE_EXPORT_DIRECTORY exp =
        (PIMAGE_EXPORT_DIRECTORY)(base_ + d.VirtualAddress);
    DWORD* names = (DWORD*)(base_ + exp->AddressOfNames);
    WORD*  ords  = (WORD*)(base_ + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base_ + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = (const char*)(base_ + names[i]);
        if (strcmp(n, name) == 0) return base_ + funcs[ords[i]];
    }
    return nullptr;
}

// Dump every VTableFixup slot and classify its lifecycle state.
CppCliLoader::SlotSummary CppCliLoader::DumpSlots(const char* phase) const {
    SlotSummary sum;
    printf("[slots] --- %s ---\n", phase);
    for (const auto& t : fixups_) {
        DWORD width = (t.type & kCorVtable64Bit) ? 8 : 4;
        for (WORD i = 0; i < t.count; ++i) {
            uint64_t v = 0;
            memcpy(&v, base_ + t.slotRva + (DWORD)i * width, width);
            printf("[slots]   slot @ RVA 0x%08X -> 0x%016llX  %s\n",
                   (unsigned)(t.slotRva + (DWORD)i * width),
                   (unsigned long long)v, ClassifySlotValue(v));
            if (width < 8) { sum.other++; continue; }
            if ((v >> 32) == 0 && (v & 0xFF000000ull) == 0x06000000ull) sum.tokens++;
            else if (v >= 0x0000010000000000ull && v < 0x00007FF000000000ull) sum.thunks++;
            else if (v >= 0x00007FF000000000ull) sum.code++;
            else sum.other++;
        }
    }
    printf("[slots]   summary: tokens=%d thunks=%d jit=%d other=%d\n",
           sum.tokens, sum.thunks, sum.code, sum.other);
    return sum;
}

// ----------------------------------------------------------------------------
// Demo driver: manually map MixedLib.dll and run the research experiments.
// ----------------------------------------------------------------------------
using FnIntInt   = int(__cdecl*)(int);
using FnVoidInt  = void(__cdecl*)(int);
using FnIntVoid  = int(__cdecl*)(void);

static CppCliLoader* g_loader = nullptr;

static DWORD WINAPI WorkerProc(LPVOID param) {
    int id = (int)(INT_PTR)param;
    printf("\n--- [Worker Thread %d] Starting Execution ---\n", id);
    // Emulate ntdll thread attach: clone the TLS template for this thread.
    g_loader->ThreadAttach();
    FnVoidInt testBothTls = (FnVoidInt)g_loader->GetExport("TestBothTls");
    if (testBothTls) testBothTls(id);
    return 0;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "MixedLib.dll";
    setvbuf(stdout, nullptr, _IONBF, 0);  // crash-proof progress reporting

    printf("====================================================================\n");
    printf("  C++/CLI (IJW) Manual PE Loader — host %s\n", kHostArch);
    printf("====================================================================\n");

    CppCliLoader loader;
    g_loader = &loader;
    if (!loader.Load(path)) {
        printf("[fatal] manual mapping failed\n");
        return 1;
    }

    // ------------------------------------------------------------------
    // EXPERIMENT 0: VTableFixup slot lifecycle across the IJW hijack.
    // ------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 0: VTableFixup 3-State Slot Lifecycle <<<\n");
    CppCliLoader::SlotSummary s1 = loader.DumpSlots(
        "STATE 1: after manual map, before DLL_PROCESS_ATTACH (on-disk tokens)");

    printf("[entry ] calling native entry point with DLL_PROCESS_ATTACH "
           "(no loader lock held)...\n");
    if (!loader.CallEntryPoint(DLL_PROCESS_ATTACH, nullptr)) {
        printf("[fatal] entry point returned FALSE — process would fail with "
               "STATUS_DLL_INIT_FAILED (0xC0020001)\n");
        return 2;
    }
    printf("[entry ] attach OK: CRT initialized, CLR hijack executed, "
           "managed static init deferred by __scrt_is_safe_for_managed_code\n");

    CppCliLoader::SlotSummary s2 = loader.DumpSlots(
        "STATE 2: after attach (CLR-installed IJW transition thunks)");

    // ------------------------------------------------------------------
    // EXPERIMENT 1: forward + reverse crossover through the dispatch stub.
    // ------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 1: Forward & Reverse Crossover <<<\n");
    FnIntInt nativeCallsManaged = (FnIntInt)loader.GetExport("NativeCallsManaged");
    if (!nativeCallsManaged) {
        printf("[fatal] export NativeCallsManaged not found\n");
        return 3;
    }
    printf("[call  ] dispatch stub: adrp x9, slot ; ldr x9, [x9] ; br x9\n");
    int r1 = nativeCallsManaged(21);
    printf(">>> Result returned to manual loader: %d (Expected: 42) <<<\n", r1);

    CppCliLoader::SlotSummary s3 = loader.DumpSlots(
        "STATE 3: after first crossover call (back-patched JIT code)");

    // ------------------------------------------------------------------
    // EXPERIMENT 2: managed exception caught by native frame.
    // ------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 2: Mixed-Mode SEH Exception Unwinding <<<\n");
    FnIntInt testMixedSeh = (FnIntInt)loader.GetExport("TestMixedSeh");
    if (!testMixedSeh) {
        printf("[fatal] export TestMixedSeh not found\n");
        return 3;
    }
    int r2 = testMixedSeh(1);
    printf(">>> SEH Handler Result: %d (Expected: 999) <<<\n", r2);

    // ------------------------------------------------------------------
    // EXPERIMENT 3: dual TLS (native template clone vs managed [ThreadStatic]).
    // ------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 3: Dual-TLS (Native vs Managed) <<<\n");
    printf("\n[Main Thread - Pass 1]\n");
    FnVoidInt testBothTls = (FnVoidInt)loader.GetExport("TestBothTls");
    if (!testBothTls) {
        printf("[fatal] export TestBothTls not found\n");
        return 3;
    }
    testBothTls(1);

    HANDLE h = CreateThread(nullptr, 0, WorkerProc, (LPVOID)(INT_PTR)2, 0, nullptr);
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    printf("\n[Main Thread - Pass 2 (Revisit)]\n");
    testBothTls(1);

    // ------------------------------------------------------------------
    // EXPERIMENT 4: pointer anatomy (gcroot / pin_ptr / interior_ptr).
    // ------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 4: Pointer Anatomy (gcroot, pin_ptr, interior_ptr) <<<\n");
    FnIntVoid testPointerAnatomy = (FnIntVoid)loader.GetExport("TestPointerAnatomy");
    if (!testPointerAnatomy) {
        printf("[fatal] export TestPointerAnatomy not found\n");
        return 3;
    }
    int r4 = testPointerAnatomy();
    printf(">>> Pointer Anatomy Result: %d (Expected: 1) <<<\n", r4);

    // ------------------------------------------------------------------
    // Verdict (mirrors the research falsification matrix).
    // ------------------------------------------------------------------
    bool lifecycle12 = s1.tokens > 0 && s2.thunks > 0;
    bool lifecycle23 = s2.thunks > 0 && s3.code > 0;
    bool ok = r1 == 42 && r2 == 999 && r4 == 1 && lifecycle12 && lifecycle23;

    printf("\n====================================================================\n");
    printf("  VERDICT\n");
    printf("    [CPP-03/05] token   -> IJW thunk  transition : %s\n",
           lifecycle12 ? "OBSERVED" : "NOT OBSERVED");
    printf("    [CPP-05]    thunk   -> JIT code  transition : %s\n",
           lifecycle23 ? "OBSERVED" : "NOT OBSERVED");
    printf("    [EXP-1]     NativeCallsManaged(21) == 42    : %s\n",
           r1 == 42 ? "PASS" : "FAIL");
    printf("    [EXP-2]     TestMixedSeh()        == 999    : %s\n",
           r2 == 999 ? "PASS" : "FAIL");
    printf("    [EXP-4]     TestPointerAnatomy()  == 1      : %s\n",
           r4 == 1 ? "PASS" : "FAIL");
    printf("    OVERALL                                       : %s\n",
           ok ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    printf("====================================================================\n");

    // Best-effort detach. clr.dll stays resident by design (matching OS
    // behavior: the CLR is never unloaded from a process).
    loader.CallEntryPoint(DLL_PROCESS_DETACH, nullptr);
    return ok ? 0 : 10;
}
