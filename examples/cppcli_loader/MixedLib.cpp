// ============================================================================
// MixedLib.cpp - C++/CLI Mixed-Mode (IJW - "It Just Works") Research Library
//
// Demonstrates:
// 1. Dual-TLS: Native Win32 TLS (__declspec(thread)) and Managed CLR TLS ([ThreadStatic]).
// 2. Forward Crossover: Native caller -> VTableFixup -> IJW Thunk -> JIT -> Managed method.
// 3. Reverse Crossover: Managed method -> Native helper without P/Invoke marshalers.
// 4. Mixed-Mode SEH: Managed exception caught by native C++ try/catch via clr!UMThunkStub.
//
// Build: cl /clr /LD MixedLib.cpp /Fe:MixedLib.dll
// ============================================================================

#include <vcclr.h>
#include <stdio.h>
#include <windows.h>

using namespace System;

// ----------------------------------------------------------------------------
// 1. Native Win32 Thread-Local Storage (TLS)
// Populates PE DataDirectory[9] (IMAGE_TLS_DIRECTORY64).
// Bound in CIL as an RVA-mapped field: .field static int32 g_nativeThreadLocal at D_0000xxxx
// Cloned per-thread by Windows kernel loader (ntdll!LdrpInitializeTls).
// ----------------------------------------------------------------------------
__declspec(thread) int g_nativeThreadLocal = 1234;

// ----------------------------------------------------------------------------
// 2. Pure Native Function (Reverse Crossover Target)
// Called directly from managed code (__clrcall -> __cdecl).
// Triggers JIT-emitted cooperative -> preemptive GC mode transition.
// ----------------------------------------------------------------------------
extern "C" __declspec(dllexport) int NativeHelper(int x) {
    printf("    [NativeHelper] pure native function executing, x = %d\n", x);
    return x * 2;
}

// ----------------------------------------------------------------------------
// 3. Managed C++/CLI Ref Class
// ----------------------------------------------------------------------------
public ref class ManagedClass {
public:
    // Managed TLS: Allocated dynamically via clr!JIT_GetNonGCThreadStaticBase_Helper.
    // Static class constructor (.cctor) runs ONCE per AppDomain.
    [ThreadStatic]
    static int s_managedThreadLocal = 5678;

    // Managed method called from native crossover
    static int Compute(int x) {
        Console::WriteLine("  [ManagedClass::Compute] entering managed context, input = " + x);
        
        // Reverse Crossover: Managed calling pure native function!
        int nativeResult = NativeHelper(x);
        Console::WriteLine("  [ManagedClass::Compute] native helper returned: " + nativeResult);
        
        return nativeResult;
    }

    // Managed method throwing exception across interop boundary
    static void ThrowException() {
        Console::WriteLine("  [ManagedClass::ThrowException] throwing System::InvalidOperationException...");
        throw gcnew System::InvalidOperationException("IJW_MANAGED_EXCEPTION_PAYLOAD");
    }

    // Multi-threaded TLS accessor
    static void TestManagedTls(int threadId) {
        s_managedThreadLocal += threadId;
        Console::WriteLine("  [ManagedClass] Thread {0}: s_managedThreadLocal = {1}", 
                           threadId, s_managedThreadLocal);
    }
};

// ----------------------------------------------------------------------------
// 4. Exported Native-Facing Functions (VTableFixup Entry Points)
// ----------------------------------------------------------------------------

// Forward Crossover: Native calls managed method through VTableFixups slot
extern "C" __declspec(dllexport) int NativeCallsManaged(int x) {
    printf("[MixedLib!NativeCallsManaged] entering from native, calling ManagedClass::Compute(%d)...\n", x);
    int res = ManagedClass::Compute(x);
    printf("[MixedLib!NativeCallsManaged] returned from managed: %d\n", res);
    return res;
}

// Mixed-Mode SEH: Catches managed exception in native C++ frame via clr!UMThunkStub
extern "C" __declspec(dllexport) int TestMixedSeh(int x) {
    printf("[MixedLib!TestMixedSeh] entering native try block...\n");
    try {
        ManagedClass::ThrowException();
        return 0; // Unreachable
    } catch (System::InvalidOperationException^ e) {
        printf("[MixedLib!TestMixedSeh] CAUGHT managed exception in native handler: %s\n", 
               "System.InvalidOperationException");
        return 999;
    }
}

// Dual-TLS Multi-Threaded Validation
extern "C" __declspec(dllexport) void TestBothTls(int threadId) {
    g_nativeThreadLocal += threadId;
    printf("[MixedLib!TestBothTls] OS Thread 0x%X (ID %d): g_nativeThreadLocal = %d (&g_native = %p)\n", 
           GetCurrentThreadId(), threadId, g_nativeThreadLocal, &g_nativeThreadLocal);
    ManagedClass::TestManagedTls(threadId);
}
