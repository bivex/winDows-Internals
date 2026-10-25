// ============================================================================
// probe_image_resource.cpp — Deep Investigation of LOAD_LIBRARY_AS_IMAGE_RESOURCE
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    LPCWSTR path = L"C:\\test_com_loader\\ComComponent.dll";

    printf("====================================================================\n");
    printf("  PROBING LOAD_LIBRARY_AS_IMAGE_RESOURCE (0x00000020)\n");
    printf("====================================================================\n");

    // 1. Call LoadLibraryExW with LOAD_LIBRARY_AS_IMAGE_RESOURCE
    printf("[probe] 1. Calling LoadLibraryExW with LOAD_LIBRARY_AS_IMAGE_RESOURCE...\n");
    HMODULE hModResource = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    printf("[probe]    Returned Handle : %p\n", (void*)hModResource);
    printf("[probe]    Handle Tag Bits : 0x%zx (%s)\n",
           (uintptr_t)hModResource & 0x3,
           ((uintptr_t)hModResource & 0x3) == 0x2 ? "Tagged: Image Resource (bit 1 set)!" :
           ((uintptr_t)hModResource & 0x3) == 0x1 ? "Tagged: Datafile (bit 0 set)!" : "Untagged");

    uintptr_t cleanBase = (uintptr_t)hModResource & ~0x3;
    printf("[probe]    Unmasked Base   : 0x%016llX\n", (unsigned long long)cleanBase);

    // 2. Query Memory Protection
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery((LPCVOID)cleanBase, &mbi, sizeof(mbi));
    printf("[probe] 2. VirtualQuery on Base:\n");
    printf("[probe]    AllocationProtect : 0x%lX (%s)\n",
           mbi.AllocationProtect,
           (mbi.AllocationProtect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ?
           "EXECUTABLE" : "NON-EXECUTABLE (Read-Only / Copy-on-Write)");
    printf("[probe]    Current Protect   : 0x%lX\n", mbi.Protect);
    printf("[probe]    Type              : 0x%lX (%s)\n",
           mbi.Type, (mbi.Type == MEM_IMAGE) ? "MEM_IMAGE" : (mbi.Type == MEM_MAPPED) ? "MEM_MAPPED" : "MEM_PRIVATE");

    // 3. Attempt GetProcAddress
    printf("[probe] 3. Calling GetProcAddress on Resource Module:\n");
    SetLastError(0);
    FARPROC pFn = GetProcAddress(hModResource, "DllGetClassObject");
    DWORD gle = GetLastError();
    printf("[probe]    GetProcAddress(\"DllGetClassObject\") -> %p (GLE = %lu %s)\n",
           (void*)pFn, gle,
           (pFn == nullptr) ? "[CONFIRMED: Cannot resolve exports on resource view!]" : "[UNEXPECTED]");

    // 4. Clean Unload
    printf("[probe] 4. Calling FreeLibrary:\n");
    BOOL bFree = FreeLibrary(hModResource);
    printf("[probe]    FreeLibrary -> %s\n", bFree ? "TRUE" : "FALSE");

    printf("====================================================================\n");
    return 0;
}
