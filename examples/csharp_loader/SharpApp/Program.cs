// ============================================================================
// Program.cs — Pure C# Assembly for C++ CoreCLR Host & Loader Research
//
// Loaded into CoreCLR by CSharpLoader.exe via hostfxr runtime delegate:
//   load_assembly_and_get_function_pointer()
//
// Invariants verified:
//   1. CoreCLR 10.0 runtime initialization inside native C++ process
//   2. RyuJIT dynamic compilation of managed C# bytecode
//   3. CoreCLR Gen0/Gen1/Gen2 Garbage Collection
//   4. Managed exception handling & stack unwinding
//   5. BCL UTF-8 string infrastructure
//   6. Native Win32 GUI MessageBox from pure C#
// ============================================================================

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace SharpApp;

public unsafe class Program
{
    private class Node
    {
        public int Value { get; set; }
        public string? Tag { get; set; }
    }

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

    // ------------------------------------------------------------------------
    // Method 1: ABI & RyuJIT Calling Convention
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "Add")]
    public static int Add(int a, int b)
    {
        return a + b;
    }

    // ------------------------------------------------------------------------
    // Method 2: CoreCLR Heap Allocations & GC Collection
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AllocateAndCollect")]
    public static int AllocateAndCollect(int count, long* pBefore, long* pAfter)
    {
        var list = new List<Node>(count);
        for (int i = 0; i < count; i++)
        {
            list.Add(new Node
            {
                Value = i * 2,
                Tag = $"CoreCLR_Node_{i}"
            });
        }

        long before = GC.GetTotalMemory(false);
        if (pBefore != null)
        {
            *pBefore = before;
        }

        // Drop references and force collection on CoreCLR
        list = null!;
        GC.Collect(2, GCCollectionMode.Forced, true, true);
        GC.WaitForPendingFinalizers();

        long after = GC.GetTotalMemory(false);
        if (pAfter != null)
        {
            *pAfter = after;
        }

        return 1;
    }

    // ------------------------------------------------------------------------
    // Method 3: BCL String Formatting & Memory Handling
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "StringFormatting")]
    public static int StringFormatting(int value, byte* outBuffer, int bufferLen)
    {
        if (outBuffer == null || bufferLen <= 0)
            return 0;

        string formatted = $"[CoreCLR BCL] Value={value}, Framework={RuntimeInformation.FrameworkDescription}, Arch={RuntimeInformation.ProcessArchitecture}";
        byte[] utf8 = Encoding.UTF8.GetBytes(formatted);

        int toCopy = Math.Min(utf8.Length, bufferLen - 1);
        Marshal.Copy(utf8, 0, (IntPtr)outBuffer, toCopy);
        outBuffer[toCopy] = 0;

        return toCopy;
    }

    // ------------------------------------------------------------------------
    // Method 4: Managed Exception Handling inside CoreCLR
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "CatchManagedException")]
    public static int CatchManagedException(int code)
    {
        try
        {
            if (code > 0)
            {
                throw new InvalidOperationException("CORECLR_MANAGED_EXCEPTION_PAYLOAD");
            }
            return 100;
        }
        catch (InvalidOperationException)
        {
            return 999;
        }
    }

    // ------------------------------------------------------------------------
    // Method 5: GUI MessageBox via user32 P/Invoke
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "ShowGuiMessageBox")]
    public static int ShowGuiMessageBox()
    {
        var proc = Process.GetCurrentProcess();
        Console.WriteLine("[SharpApp!ShowGuiMessageBox] Displaying native Win32 GUI MessageBox from CoreCLR...");

        string title = "C# / CoreCLR (.NET 10) Loader (C++)";
        string message =
            "Hello from pure C# running inside a C++ Host & Loader!\n\n" +
            $"Host Process: {proc.ProcessName} (PID={proc.Id})\n" +
            $"Architecture: {RuntimeInformation.ProcessArchitecture}\n" +
            $"Runtime     : {RuntimeInformation.FrameworkDescription}\n" +
            $"OS          : {RuntimeInformation.OSDescription}\n" +
            $"JIT Mode    : RyuJIT Active (DynamicCode: {RuntimeFeature.IsDynamicCodeSupported})\n" +
            $"Hosting API : hostfxr_initialize_for_runtime_config\n\n" +
            "Click OK or wait 4 seconds for auto-close.";

        int result = MessageBoxTimeoutW(
            IntPtr.Zero,
            message,
            title,
            MB_OK | MB_ICONINFORMATION | MB_SERVICE_NOTIFICATION,
            0,
            4000);

        Console.WriteLine($"[SharpApp!ShowGuiMessageBox] DialogResult = {result} (1=IDOK, 32000=TIMEOUT)");
        return result;
    }
}
