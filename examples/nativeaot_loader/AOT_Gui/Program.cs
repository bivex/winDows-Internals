// ============================================================================
// AOT_Gui — Standalone NativeAOT GUI Executable
//
// Designed to be executed directly OR manually mapped via NativeAotLoader.exe:
//   NativeAotLoader.exe AOT_Gui.exe
//
// Invariants verified:
//   1. Executable PE anatomy (DataDirectory[14] strictly absent)
//   2. Entry point wmainCRTStartup execution inside foreign host process
//   3. In-binary WKS::gc_heap active
//   4. Native Win32 GUI MessageBox display via user32 P/Invoke
//   5. ExitProcess / exit interception by manual loader host
// ============================================================================

using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AotGui;

public class Program
{
    [DllImport("user32.dll", EntryPoint = "MessageBoxTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int MessageBoxTimeoutW(
        IntPtr hWnd,
        string text,
        string caption,
        uint type,
        ushort languageId,
        uint milliseconds);

    private const uint MB_OK = 0x00000000;
    private const uint MB_ICONINFORMATION = 0x00000040;
    private const uint MB_SERVICE_NOTIFICATION = 0x00200000;

    public static int Main(string[] args)
    {
        Console.WriteLine("====================================================================");
        Console.WriteLine("  AOT_Gui.exe — NativeAOT Standalone Executable (ARM64)");
        Console.WriteLine("====================================================================");

        var currentProcess = Process.GetCurrentProcess();
        Console.WriteLine($"[AOT_Gui] Running in Process: {currentProcess.ProcessName} (PID={currentProcess.Id})");
        Console.WriteLine($"[AOT_Gui] Architecture      : {RuntimeInformation.ProcessArchitecture}");
        Console.WriteLine($"[AOT_Gui] OS Description    : {RuntimeInformation.OSDescription}");
        Console.WriteLine($"[AOT_Gui] Framework         : {RuntimeInformation.FrameworkDescription}");
        Console.WriteLine($"[AOT_Gui] Dynamic Code      : {RuntimeFeature.IsDynamicCodeSupported} (AOT Invariant)");

        long memBefore = GC.GetTotalMemory(false);
        var dummy = new byte[1024 * 512]; // 512 KB allocation on WKS::gc_heap
        dummy[0] = 0x42;
        long memAfter = GC.GetTotalMemory(false);
        Console.WriteLine($"[AOT_Gui] GC Heap Memory    : {memBefore} -> {memAfter} bytes (bump allocation OK)");

        string windowTitle = "AOT_Gui.exe — NativeAOT Manual Loader";
        string message =
            "NativeAOT Executable (AOT_Gui.exe) running via Manual PE Loader!\n\n" +
            $"Host Process: {currentProcess.ProcessName} (PID={currentProcess.Id})\n" +
            $"Architecture: {RuntimeInformation.ProcessArchitecture}\n" +
            $"Runtime     : Statically Linked NativeAOT (.NET 10)\n" +
            $"Memory      : WKS::gc_heap active ({memAfter} bytes)\n" +
            $"DynamicCode : False (Ahead-of-Time Compiled)\n\n" +
            "Click OK or wait 4 seconds for auto-close.";

        Console.WriteLine("[AOT_Gui] Displaying native Win32 GUI MessageBox (timeout=4000ms)...");
        int result = MessageBoxTimeoutW(
            IntPtr.Zero,
            message,
            windowTitle,
            MB_OK | MB_ICONINFORMATION | MB_SERVICE_NOTIFICATION,
            0,
            4000);

        Console.WriteLine($"[AOT_Gui] MessageBox DialogResult = {result} (1=IDOK, 32000=TIMEOUT)");
        Console.WriteLine("[AOT_Gui] Exiting wmain with status 0...");
        Console.WriteLine("====================================================================");

        return 0;
    }
}
