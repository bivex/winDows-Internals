● TLS-before-main mechanism: what I found

  I investigated how TLS is set up before main/wWinMain in this conhost.exe.

  Short version:

    • conhost.exe does have a PE TLS directory
    • it uses static TLS storage
    • it appears to have no PE TLS callback functions
    • it also has no CRT dynamic TLS init callback active
    • so the loader still allocates/copies TLS for each thread, but there is no extra TLS callback code running before `wWinMain` in this specific binary

  Evidence from the image

  From the PE headers:

    • TLS directory RVA: 0x9E190
    • entrypoint: 0x1F760 → conhost!wWinMainCRTStartup

  The TLS directory lives at:

    • conhost!tls_used = 0x00007ff603aae190

  Decoded from memory, it contains:

    • StartAddressOfRawData = conhost!tls_start = 0x00007ff603abadb0
    • EndAddressOfRawData   = conhost!tls_end   = 0x00007ff603abadb8
    • AddressOfIndex        = conhost!tls_index = 0x00007ff603ad2a28
    • AddressOfCallbacks    = 0x00007ff603aafac0

  So the static TLS template size is:

    • tls_end - tls_start = 8 bytes

  That is tiny, but real.

  Also:

    • conhost!tls_index = 0

  So the loader assigned this module TLS slot 0.

  What the static TLS contains

  At conhost!tls_start I dumped the template:

    • first 8 bytes: 0x80000000_00000000

  WinDbg also resolved tls_start next to:

    • conhost!Init_thread_epoch

  That strongly suggests this TLS block is used for MSVC/CRT per-thread initialization bookkeeping, not for some big application-owned TLS structure.

  PE TLS callbacks: apparently none

  The TLS callback array pointer is:

    • AddressOfCallbacks = 0x00007ff603aafac0

  Dumping that area showed:

    • first several entries are 0

  So for this image, the PE TLS callback array is effectively empty.

  That means:

    • the OS loader still recognizes the image as a TLS user
    • but it has no callback functions to invoke from that array

  CRT dynamic TLS callback: also not active here

  I also checked the CRT-side hook:

    • conhost!_scrt_get_dyn_tls_init_callback returns &conhost!_dyn_tls_init_callback
    • conhost!_dyn_tls_init_callback = 0
    • conhost!_dyn_tls_dtor_callback = 0

  So the MSVC CRT startup code does this:

    1. get &_dyn_tls_init_callback
    2. check whether *_dyn_tls_init_callback != 0
    3. only then call it

  In this binary that pointer is null, so the path is skipped.

  Why the CRT checks a TLS init callback at all

  This is the subtle part.

  There are two related but different TLS mechanisms here:

  1. Loader / PE TLS
  This is the OS-level mechanism driven by the PE TLS directory:

    • static TLS template
    • TLS slot index
    • optional TLS callback array

  2. CRT dynamic thread-local initialization
  MSVC also supports dynamic initialization for thread_local / TLS-backed C++ objects.

  In __scrt_common_main_seh, I already saw the CRT logic:

    • it fetches _dyn_tls_init_callback
    • if non-null, it calls it with reason 2 = DLL_THREAD_ATTACH

  That manual call exists to make sure the primary thread gets dynamic thread-local initialization when needed.

  But in this conhost.exe build:

    • _dyn_tls_init_callback == 0

  So there is no dynamic TLS init routine to run.

  What the loader does before wWinMain

  Process-start side
  The loader-side helper for TLS data is:

    • ntdll!LdrpHandleTlsData

  From its disassembly, it:

    • reads image directory 9 (TLS directory)
    • allocates loader TLS bookkeeping
    • assigns/registers a TLS slot
    • links the module into the loader TLS list

  I also dumped the loader TLS bookkeeping list:

    • ntdll!LdrpTlsList
    • first entry at 0x00000000004250b0

  That entry mirrors the conhost TLS directory exactly:

    • raw data start = conhost!tls_start
    • raw data end = conhost!tls_end
    • index addr = conhost!tls_index
    • callback addr = 0x00007ff603aafac0
    • owning module entry = 0x0000000000424ce0 = main image _LDR_DATA_TABLE_ENTRY

  So conhost is definitely registered as a TLS-bearing image.

  Per-thread allocation path

  For thread startup, I disassembled:

    • ntdll!LdrpInitializeThread
    • ntdll!LdrpAllocateTls
    • ntdll!LdrpCallTlsInitializers

  That path is very clear:

    1. LdrpInitializeThread
    2. calls LdrpAllocateTls
    3. allocates/extends the thread TLS vector
    4. walks LdrpTlsList
    5. copies each module’s TLS template into that thread’s slot
    6. calls LdrpCallTlsInitializers(..., DLL_THREAD_ATTACH) for modules that have callbacks

  In LdrpInitializeThread, I saw:

    • call ntdll!LdrpAllocateTls
    • later call ntdll!LdrpCallTlsInitializers
    • with ecx = 2 → DLL_THREAD_ATTACH

  So for secondary threads, TLS is prepared before normal thread entry logic.

  Concrete proof from actual thread TLS vectors

  The working threads’ TEB.ThreadLocalStoragePointer values are non-null.

  Examples:

    • thread 0 TLS vector at 0x000000000044c6e0
    • thread 1 TLS vector at 0x000000000044c0a0

  Slot 0 of those vectors points to per-thread TLS blocks:

    • thread 0 slot 0 → 0x00000000004366a0
    • thread 1 slot 0 → 0x0000000002c3d720

  That matches conhost!tls_index = 0.

  So the loader really did allocate per-thread storage for this image.

  Important conclusion

  For this conhost.exe, the actual “TLS before main” story is:

    1. loader sees a TLS directory
    2. loader registers the image in LdrpTlsList
    3. loader assigns tls_index = 0
    4. loader creates/copies the 8-byte static TLS block for threads
    5. there are no PE TLS callbacks to invoke
    6. CRT startup checks for _dyn_tls_init_callback
    7. _dyn_tls_init_callback is null, so no CRT dynamic TLS init runs
    8. execution continues into:
       • wWinMainCRTStartup
       • __scrt_common_main_seh
       • wWinMain

  So in this binary, TLS is mostly a data allocation/copy mechanism, not a “run code before main” mechanism.

  Practical takeaway

  If your question is:

  “Does TLS execute code before main here?”
  Answer:

    • Not via PE TLS callbacks
    • Not via CRT dynamic TLS init callback
    • only static TLS allocation/copy is happening before wWinMain

  “Is TLS still active in this image?”
  Yes:

    • the image has a real TLS directory
    • the loader registers it
    • per-thread TLS blocks exist
    • slot index is assigned and used

  Best next step

  If you want, I can now investigate one of these:

    1. where that 8-byte TLS variable is used inside conhost
    2. how the loader sets up the main thread TLS vector during process init
    3. compare main-image TLS vs DLL TLS entries in `LdrpTlsList`



● What TLS is actually doing here

  I traced the concrete mechanism further.

  The key finding is:

    • conhost.exe uses TLS mainly for the CRT’s thread-safe static initialization epoch
    • not for PE TLS callbacks
    • not for a custom “run code before main” callback

  So TLS is still part of the pre-main setup, but as per-thread data provisioning, not as a TLS-callback code path.

  The TLS payload in this image

  The PE TLS template is only 8 bytes:

    • conhost!tls_start = 0x00007ff603abadb0
    • conhost!tls_end   = 0x00007ff603abadb8

  Raw template:

    • +0x0 = 0x00000000
    • +0x4 = 0x80000000

  The symbol at +0x4 is:

    • conhost!Init_thread_epoch

  So the image’s TLS block is basically:

    1. one dword at offset 0
    2. Init_thread_epoch at offset 4

  The loader part

  Earlier we confirmed:

    • conhost!tls_index = 0
    • the loader registered this image in ntdll!LdrpTlsList
    • each thread gets a per-thread TLS block for this image in TLS slot 0

  Live thread examples:

    • thread 0 slot 0 → 0x004366a0
    • thread 1 slot 0 → 0x02c3d720

  Their contents:

  Thread 0
    • [slot0+0] = 0x00000000
    • [slot0+4] = 0x80000007

  Thread 1
    • [slot0+0] = 0x00000000
    • [slot0+4] = 0x80000000

  So the loader copied the TLS template, and later the main thread’s epoch advanced.

  The global epoch

  There is also:

    • conhost!Init_global_epoch = 0x80000007

  That matches the main thread’s TLS epoch value exactly:

    • main thread Init_thread_epoch = 0x80000007

  while another thread still has:

    • Init_thread_epoch = 0x80000000

  That is strong evidence this TLS value is used as a per-thread cache of the global initialization epoch.

  The actual CRT logic

  I disassembled the helpers that use this TLS field.

  conhost!Init_thread_header

  This is the most important one.

  It does:

    1. enter CRT lock
    2. inspect the guarded static’s state
    3. if another thread already initialized it:
       • read the TLS vector from gs:[58h]
       • load tls_index
       • fetch current thread’s TLS block
       • write Init_global_epoch into TLS_block + 4

  The critical instructions are:

    • mov rax, qword ptr gs:[58h]
    • mov ecx, dword ptr [conhost!tls_index]
    • mov rdx, qword ptr [rax+rcx*8]
    • mov eax, dword ptr [conhost!Init_global_epoch]
    • mov dword ptr [rdx+4], eax

  So this directly proves:

    • the CRT uses loader-provided TLS slot lookup
    • then writes to the TLS field at offset +4
    • which is exactly Init_thread_epoch

  conhost!Init_thread_footer

  This completes successful initialization of a guarded static.

  It does:

    1. enter lock
    2. increment Init_global_epoch
    3. store the new epoch into the guard object
    4. fetch current thread TLS block via gs:[58h] + tls_index
    5. store the new epoch into TLS_block + 4

  Again, directly:

    • mov edx, dword ptr [conhost!tls_index]
    • mov rax, qword ptr gs:[58h]
    • mov r8, qword ptr [rax+rdx*8]
    • mov eax, dword ptr [conhost!Init_global_epoch]
    • mov dword ptr [r8+4], eax

  So the thread that performs the initialization updates its own TLS epoch cache.

  conhost!Init_thread_abort

  If initialization fails/aborts:

    • it resets the guard state to 0
    • then notifies waiters

  So the usual CRT guarded-static state machine is present.

  What this means semantically

  This is the classic MSVC CRT model for thread-safe local static initialization.

  Conceptually:

    • each guarded static has a global state / epoch
    • each thread has a TLS Init_thread_epoch
    • the CRT compares thread epoch vs global epoch to know whether the thread has observed the latest initialized state
    • this avoids unnecessary locking in some paths and supports safe lazy initialization

  Relationship to “before main”

  Here is the important distinction.

  What happens before main
  Before wWinMain runs, the loader:

    1. recognizes the TLS directory
    2. assigns tls_index
    3. allocates the main thread’s TLS block
    4. copies the TLS template into that block

  That means the per-thread Init_thread_epoch storage already exists before CRT/app startup.

  What does not happen before main
  For this image, I found no active:

    • PE TLS callbacks
    • CRT dynamic TLS init callback

  So no TLS callback code is executing before main.

  Why TLS still matters before main
  Because the CRT thread-safe-static machinery depends on that TLS block being ready from the start.

  Without loader TLS setup, these helpers:

    • Init_thread_header
    • Init_thread_footer

  would have nowhere to store/read the per-thread epoch.

  The clean execution picture

  So the actual mechanism is:

    1. loader sets up TLS slot and per-thread TLS block
    2. entry goes to wWinMainCRTStartup
    3. CRT startup runs
    4. later, whenever code hits a guarded local static, CRT helpers use:
       • gs:[58h] → thread TLS vector
       • tls_index
       • TLS_block + 4 → Init_thread_epoch

  That is the real TLS→main connection in this binary.

  Best conclusion

  For conhost.exe:

    • TLS is present and active
    • but it is used as runtime per-thread state storage
    • specifically for CRT thread-safe statics
    • not as a custom callback mechanism that executes code before main

  If you want, I can go one step deeper

  Next useful directions are:

    1. find a concrete guarded static site that calls Init_thread_header/footer
    2. inspect the main thread’s TLS slot 0 block symbolically
    3. trace exactly where in startup Init_global_epoch advanced from 0x80000000 to 0x80000007




























