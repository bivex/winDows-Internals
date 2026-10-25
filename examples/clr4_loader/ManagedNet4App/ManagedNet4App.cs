// ============================================================================
// ManagedNet4App.cs — Pure Managed .NET Framework 4.x Target Component (ARM64)
//
// Demonstrates execution within a native C++ CLR 4 hosted environment:
//   - Entry point invoked via ICLRRuntimeHost::ExecuteInDefaultAppDomain
//   - CLR environment and AppDomain reflection
//   - GC heap allocation, collection and compaction tracking
//   - Structured exception handling within managed frames
//   - P/Invoke to Win32 MessageBoxTimeoutW displaying interactive GUI dialog
//
// Compile with:
// C:\Windows\Microsoft.NET\FrameworkArm64\v4.0.30319\csc.exe /target:library /out:ManagedNet4App.dll ManagedNet4App.cs
// ============================================================================

using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

namespace ManagedNet4App
{
    public class Program
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "MessageBoxTimeoutW")]
        private static extern int MessageBoxTimeoutW(
            IntPtr hWnd,
            string lpText,
            string lpCaption,
            uint uType,
            ushort wLanguageId,
            uint dwMilliseconds
        );

        private const uint MB_OK = 0x00000000;
        private const uint MB_ICONINFORMATION = 0x00000040;
        private const uint MB_TOPMOST = 0x00040000;
        private const uint MB_SETFOREGROUND = 0x00010000;
        private const uint MB_SYSTEMMODAL = 0x00001000;

        public static int EntryPoint(string argument)
        {
            Console.WriteLine("\n[ManagedNet4App] >>> Executing in CLR 4.x Default AppDomain <<<");
            Console.WriteLine("[ManagedNet4App] Argument Received : \"" + argument + "\"");

            // 1. Environment & Architecture Diagnostics
            Console.WriteLine("[ManagedNet4App] CLR Version       : " + Environment.Version);
            Console.WriteLine("[ManagedNet4App] Is64BitProcess    : " + Environment.Is64BitProcess);
            Console.WriteLine("[ManagedNet4App] OS Version        : " + Environment.OSVersion);
            Console.WriteLine("[ManagedNet4App] Machine Name      : " + Environment.MachineName);
            Console.WriteLine("[ManagedNet4App] AppDomain Name    : " + AppDomain.CurrentDomain.FriendlyName);

            // 2. GC Heap Allocation & Compaction Experiment
            long memBefore = GC.GetTotalMemory(false);
            Console.WriteLine("[ManagedNet4App] GC Heap Initial   : " + memBefore + " bytes");

            List<byte[]> garbage = new List<byte[]>();
            for (int i = 0; i < 200; ++i)
            {
                garbage.Add(new byte[1024 * 16]); // Allocate ~3.2 MB of garbage
            }
            long memAllocated = GC.GetTotalMemory(false);
            Console.WriteLine("[ManagedNet4App] GC Heap Allocated : " + memAllocated + " bytes (Allocated ~3.2 MB)");

            garbage.Clear();
            garbage = null;
            GC.Collect(2, GCCollectionMode.Forced);
            GC.WaitForPendingFinalizers();
            long memAfter = GC.GetTotalMemory(true);
            Console.WriteLine("[ManagedNet4App] GC Heap Collected : " + memAfter + " bytes (Reclaimed: " + (memAllocated - memAfter) + " bytes)");

            // 3. Exception Handling Verification
            try
            {
                Console.WriteLine("[ManagedNet4App] Testing CLR 4 Exception Handling...");
                ThrowSampleException();
            }
            catch (InvalidOperationException ex)
            {
                Console.WriteLine("[ManagedNet4App] Caught Managed Exception: \"" + ex.Message + "\" (Clean Catch)");
            }

            // 4. Native Win32 GUI Dialog Display
            Console.WriteLine("[ManagedNet4App] Displaying Native Win32 GUI MessageBox from .NET Framework 4.8...");
            string title = ".NET Framework 4.x CLR Host (ARM64)";
            string message = "Hello from Classic .NET Framework 4.8 running under CLR 4 MetaHost!\n\n" +
                             "CLR Engine      : clr.dll (CLR v4.0.30319)\n" +
                             "Runtime Path    : C:\\Windows\\Microsoft.NET\\FrameworkArm64\\v4.0.30319\\\n" +
                             "Execution Model : Default AppDomain via ICLRRuntimeHost::ExecuteInDefaultAppDomain\n" +
                             "Host Interface  : ICLRMetaHost / ICLRRuntimeInfo / ICLRRuntimeHost / ICorRuntimeHost\n\n" +
                             "Click OK or wait 60 seconds for auto-close.";

            int dlgRes = MessageBoxTimeoutW(IntPtr.Zero, message, title,
                                            MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL,
                                            0, 60000);
            Console.WriteLine("[ManagedNet4App] DialogResult = " + dlgRes);

            // Return calculated magic exit code
            return 1337;
        }

        private static void ThrowSampleException()
        {
            throw new InvalidOperationException("CLR 4 Boundary Guard Invariant Verified");
        }
    }
}
