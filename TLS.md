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
































