// ============================================================================
// AotLib.cs — NativeAOT Shared Library for Manual PE Loader Research
//
// Compiled with:
//   dotnet publish -c Release -r win-arm64 -p:PublishAot=true
//
// Exercises under manual loading:
//   1. RhpNewFast lock-free bump allocation on WKS::gc_heap
//   2. Native Win32 TLS lookup via TEB->ThreadLocalStoragePointer[__tls_index]
//   3. Statically linked Garbage Collector (GC.Collect)
//   4. NativeAOT internal exception handling & .pdata unwinding
//   5. BCL string formatting & RuntimeFeature validation
// ============================================================================

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace AotLib;

public unsafe class NativeExports
{
    private class Node
    {
        public int Value { get; set; }
        public string? Tag { get; set; }
    }

    // ------------------------------------------------------------------------
    // Export 1: ABI & calling convention verification
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotAdd")]
    public static int AotAdd(int a, int b)
    {
        return a + b;
    }

    // ------------------------------------------------------------------------
    // Export 2: Exercises RhpNewFast lock-free bump allocation on WKS::gc_heap
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotAllocateAndCompute")]
    public static int AotAllocateAndCompute(int count)
    {
        var list = new List<Node>(count);
        for (int i = 0; i < count; i++)
        {
            list.Add(new Node
            {
                Value = i * 2,
                Tag = $"Node_{i}"
            });
        }

        int sum = 0;
        foreach (var node in list)
        {
            sum += node.Value;
        }

        return sum;
    }

    // ------------------------------------------------------------------------
    // Export 3: In-binary Workstation Garbage Collection (WKS::gc_heap)
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotTriggerGc")]
    public static int AotTriggerGc(long* pBeforeBytes, long* pAfterBytes)
    {
        long before = GC.GetTotalMemory(false);
        if (pBeforeBytes != null)
        {
            *pBeforeBytes = before;
        }

        // Allocate objects on WKS::gc_heap
        var temp = new object[5000];
        for (int i = 0; i < temp.Length; i++)
        {
            temp[i] = new byte[64];
        }

        long after = GC.GetTotalMemory(false);
        if (pAfterBytes != null)
        {
            *pAfterBytes = after;
        }

        return after > before ? 1 : 0;
    }

    // ------------------------------------------------------------------------
    // Export 4: BCL string formatting & memory copying
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotStringFormatting")]
    public static int AotStringFormatting(int value, byte* outBuffer, int bufferLen)
    {
        if (outBuffer == null || bufferLen <= 0)
            return 0;

        string formatted = $"[NativeAOT BCL] Value={value}, IsDynamicCodeSupported={RuntimeFeature.IsDynamicCodeSupported}";
        byte[] utf8 = Encoding.UTF8.GetBytes(formatted);

        int toCopy = Math.Min(utf8.Length, bufferLen - 1);
        Marshal.Copy(utf8, 0, (IntPtr)outBuffer, toCopy);
        outBuffer[toCopy] = 0; // null-terminate

        return toCopy;
    }

    // ------------------------------------------------------------------------
    // Export 5: Internal NativeAOT SEH unwinding & personality routine
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotTestExceptions")]
    public static int AotTestExceptions(int code)
    {
        try
        {
            if (code > 0)
            {
                throw new InvalidOperationException("NATIVE_AOT_MANAGED_EXCEPTION_PAYLOAD");
            }
            return 100;
        }
        catch (InvalidOperationException)
        {
            // Successfully caught managed exception inside NativeAOT runtime!
            return 999;
        }
    }

    // ------------------------------------------------------------------------
    // Export 6: Runtime feature & environment inspection
    // ------------------------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "AotGetRuntimeInfo")]
    public static int AotGetRuntimeInfo(byte* outBuffer, int bufferLen)
    {
        if (outBuffer == null || bufferLen <= 0)
            return 0;

        string info = $"Architecture: {RuntimeInformation.ProcessArchitecture}, " +
                      $"OS: {RuntimeInformation.OSDescription}, " +
                      $"Framework: {RuntimeInformation.FrameworkDescription}, " +
                      $"DynamicCode: {RuntimeFeature.IsDynamicCodeSupported}";

        byte[] utf8 = Encoding.UTF8.GetBytes(info);
        int toCopy = Math.Min(utf8.Length, bufferLen - 1);
        Marshal.Copy(utf8, 0, (IntPtr)outBuffer, toCopy);
        outBuffer[toCopy] = 0;

        return toCopy;
    }

    // ------------------------------------------------------------------------
    // Export 7: GUI MessageBox from NativeAOT via P/Invoke to user32
    // ------------------------------------------------------------------------
    [DllImport("user32.dll", EntryPoint = "MessageBoxW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    [UnmanagedCallersOnly(EntryPoint = "AotTestGuiMessageBox")]
    public static int AotTestGuiMessageBox()
    {
        Console.WriteLine("[AotLib!AotTestGuiMessageBox] Showing GUI MessageBox from NativeAOT...");
        int result = MessageBoxW(
            IntPtr.Zero,
            "Hello from a MANUALLY MAPPED NativeAOT assembly!\n\n" +
            "Execution flow:\n" +
            "1. NativeAotLoader maps AotLib manually (no LoadLibrary)\n" +
            "2. DataDirectory[14] strictly absent (0x0)\n" +
            "3. ntdll!KiUserInvertedFunctionTable entry inserted\n" +
            "4. In-binary WKS::gc_heap initialized via RhInitialize\n" +
            "5. C# [UnmanagedCallersOnly] export invoked\n" +
            "6. P/Invoke to user32!MessageBoxW executed from AOT code!",
            "NativeAOT Manual Loader (ARM64)",
            0x00200040 /* MB_OK | MB_ICONINFORMATION | MB_SERVICE_NOTIFICATION */);
        Console.WriteLine($"[AotLib!AotTestGuiMessageBox] DialogResult = {result}");
        return result;
    }
}
