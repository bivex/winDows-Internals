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
#using <System.Windows.Forms.dll>

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

// ----------------------------------------------------------------------------
// 5. Pointer Anatomy: gcroot, pin_ptr, and interior_ptr
// ----------------------------------------------------------------------------
struct NativeHolder {
    msclr::gcroot<String^> m_managedStr;
    int m_nativeId;
};

extern "C" __declspec(dllexport) int TestPointerAnatomy() {
    printf("[MixedLib!TestPointerAnatomy] Starting Pointer Anatomy Test...\n");
    
    // 1. gcroot<T>: Allocate native struct on C++ heap holding managed string
    NativeHolder* pHolder = new NativeHolder();
    pHolder->m_managedStr = gcnew String("Hello from Managed Heap via gcroot!");
    pHolder->m_nativeId = 42;
    
    printf("  [gcroot] NativeHolder at %p, sizeof(gcroot) = %zu, nativeId = %d\n", 
           pHolder, sizeof(pHolder->m_managedStr), pHolder->m_nativeId);
    
    // 2. pin_ptr<T>: Pin managed array in memory
    array<int>^ arr = gcnew array<int>(10);
    arr[0] = 12345;
    pin_ptr<int> pinnedPtr = &arr[0];
    int* pRaw = pinnedPtr;
    printf("  [pin_ptr] Managed array pinned at %p, raw value = %d\n", pRaw, *pRaw);
    
    // 3. interior_ptr<T>: Managed byref pointer inside array
    cli::interior_ptr<int> interiorPtr = &arr[1];
    *interiorPtr = 67890;
    printf("  [interior_ptr] Interior pointer at %p, value = %d\n", interiorPtr, *interiorPtr);
    
    delete pHolder;
    return 1;
}

// ----------------------------------------------------------------------------
// 6. GUI: Managed WinForms MessageBox via C++/CLI
// Called through native VTableFixup dispatch slot. Exercises CLR Fusion
// assembly resolution (System.Windows.Forms from GAC) under manual mapping.
// ----------------------------------------------------------------------------
extern "C" __declspec(dllexport) int TestGuiMessageBox() {
    printf("[MixedLib!TestGuiMessageBox] showing managed WinForms MessageBox...\n");
    ::System::Windows::Forms::DialogResult r =
        ::System::Windows::Forms::MessageBox::Show(
            "Hello from a MANUALLY MAPPED C++/CLI (IJW) assembly!\n\n"
            "Execution flow:\n"
            "1. CppCliLoader maps MixedLib manually (no LoadLibrary)\n"
            "2. ntdll!RtlRbInsertNodeEx inserts ghost LDR entry\n"
            "3. Native entry stub tail-jumps to mscoree!_CorDllMain\n"
            "4. CLR converts VTableFixup slot to IJW thunk\n"
            "5. First crossover call JIT-compiles ManagedClass\n"
            "6. WinForms assembly loaded via Fusion GAC\n"
            "7. Managed MessageBox displayed on desktop!",
            "C++/CLI IJW Manual Loader (ARM64)",
            ::System::Windows::Forms::MessageBoxButtons::OK,
            ::System::Windows::Forms::MessageBoxIcon::Information,
            ::System::Windows::Forms::MessageBoxDefaultButton::Button1,
            ::System::Windows::Forms::MessageBoxOptions::ServiceNotification);
    printf("[MixedLib!TestGuiMessageBox] DialogResult = %d\n", (int)r);
    return (int)r;
}
