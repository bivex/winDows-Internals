// ============================================================================
// Program.cs — NativeAOT Security Surface Analysis PoC
//
// Self-contained security auditor that runs INSIDE a NativeAOT-compiled
// binary and verifies the research claims from:
//   KERNEL/RoP_NativeAOT_Research.md
//
// Verifies:
//   1. PE DllCharacteristics — CFG flag presence/absence
//   2. Memory region protections — identify writable data sections
//   3. ARM64 gadget density — count ret instructions in .text
//   4. Process-wide RWX scan — confirm zero writable+executable regions
//
// Build: dotnet publish -r win-arm64 -c Release
// Run:   TestAotSec.exe
// ============================================================================

using System;
using System.Runtime.InteropServices;

namespace NativeAotSecurityPoc;

public class Program
{
    [StructLayout(LayoutKind.Sequential)]
    struct MEMORY_BASIC_INFORMATION
    {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandleW(string moduleName);

    [DllImport("kernel32")]
    static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
        byte[] lpBuffer, IntPtr nSize, out IntPtr lpNumberOfBytesRead);

    [DllImport("kernel32")]
    static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32")]
    static extern IntPtr VirtualQuery(IntPtr lpAddress,
        out MEMORY_BASIC_INFORMATION lpBuffer, IntPtr dwLength);

    const uint PAGE_EXECUTE_READWRITE = 0x40;
    const uint PAGE_READONLY = 0x02;
    const uint PAGE_READWRITE = 0x04;
    const uint PAGE_EXECUTE_READ = 0x20;
    const uint MEM_COMMIT = 0x1000;

    static byte[] ReadMem(IntPtr address, int size)
    {
        var buffer = new byte[size];
        IntPtr bytesRead;
        ReadProcessMemory(GetCurrentProcess(), address, buffer, (IntPtr)size, out bytesRead);
        return buffer;
    }

    public static void Main(string[] args)
    {
        Console.WriteLine("=== NativeAOT Security Surface Analysis PoC ===\n");

        var hModule = GetModuleHandleW(null);
        Console.WriteLine($"Module Base: 0x{hModule:X}");

        Console.WriteLine("\n--- CHECK 1: PE Security Flags ---");
        CheckPeFlags(hModule);

        Console.WriteLine("\n--- CHECK 2: Memory Region Protections ---");
        CheckMemoryProtections(hModule);

        Console.WriteLine("\n--- CHECK 3: ARM64 ret Gadget Scan ---");
        ScanForRetGadgets(hModule);

        Console.WriteLine("\n--- CHECK 4: Process-Wide RWX Regions ---");
        ScanForRwxRegions();

        Console.WriteLine("\n=== Analysis Complete ===");
    }

    // -------------------------------------------------------------------
    // CHECK 1: Parse own PE header to verify CFG flag (claim AOTSEC-01)
    // -------------------------------------------------------------------
    static void CheckPeFlags(IntPtr hModule)
    {
        var dosBytes = ReadMem(hModule, 64);
        if (dosBytes[0] != 0x4D || dosBytes[1] != 0x5A)
        {
            Console.WriteLine("  [!] Invalid DOS header");
            return;
        }

        int peOffset = BitConverter.ToInt32(dosBytes, 0x3C);
        var dllCharsOffset = hModule + peOffset + 4 + 20 + 70;
        var dllCharsBytes = ReadMem(dllCharsOffset, 2);
        ushort dllChars = (ushort)(dllCharsBytes[0] | (dllCharsBytes[1] << 8));

        Console.WriteLine($"  DllCharacteristics: 0x{dllChars:X4}");
        Console.WriteLine($"  ASLR (0x0040):          {((dllChars & 0x0040) != 0 ? "ENABLED" : "DISABLED")}");
        Console.WriteLine($"  DEP (0x0100):           {((dllChars & 0x0100) != 0 ? "ENABLED" : "DISABLED")}");
        Console.WriteLine($"  HighEntropyVA (0x0020): {((dllChars & 0x0020) != 0 ? "ENABLED" : "DISABLED")}");
        Console.WriteLine($"  CFG (0x4000):           {((dllChars & 0x4000) != 0 ? "ENABLED" : "!!! DISABLED !!!")}");

        if ((dllChars & 0x4000) == 0)
            Console.WriteLine("  [FINDING] No CFG = unvalidated indirect branches (AOTSEC-01)");
    }

    // -------------------------------------------------------------------
    // CHECK 2: Enumerate memory regions and their protections
    // -------------------------------------------------------------------
    static void CheckMemoryProtections(IntPtr hModule)
    {
        var mbi = new MEMORY_BASIC_INFORMATION();
        IntPtr addr = hModule;
        int mbiSize = Marshal.SizeOf<MEMORY_BASIC_INFORMATION>();
        int regionCount = 0;

        Console.WriteLine($"  Scanning module regions at 0x{hModule:X}...");

        for (int i = 0; i < 10; i++)
        {
            var result = VirtualQuery(addr, out mbi, (IntPtr)mbiSize);
            if (result == IntPtr.Zero) break;

            ulong baseAddr = (ulong)mbi.BaseAddress;
            ulong moduleBase = (ulong)hModule;
            if (baseAddr < moduleBase || baseAddr > moduleBase + 0x200000) break;

            string protStr = mbi.Protect switch
            {
                PAGE_READONLY => "PAGE_READONLY",
                PAGE_READWRITE => "PAGE_READWRITE",
                PAGE_EXECUTE_READ => "PAGE_EXECUTE_READ",
                PAGE_EXECUTE_READWRITE => "PAGE_EXECUTE_READWRITE (RWX!)",
                _ => $"0x{mbi.Protect:X}"
            };
            Console.WriteLine($"  Region {i}: 0x{mbi.BaseAddress:X} size=0x{(ulong)mbi.RegionSize:X} prot={protStr}");
            regionCount++;

            addr = mbi.BaseAddress + mbi.RegionSize;
        }
        Console.WriteLine($"  Scanned {regionCount} regions");
    }

    // -------------------------------------------------------------------
    // CHECK 3: Scan .text section for ARM64 ret gadgets (claim AOTSEC-03)
    // -------------------------------------------------------------------
    static void ScanForRetGadgets(IntPtr hModule)
    {
        var textStart = hModule + 0x1000;
        int scanSize = 0x10000; // 64KB sample

        Console.WriteLine($"  Scanning {(scanSize/1024)}KB at module+0x1000 for ARM64 ret (C0 03 5F D6)...");

        var bytes = ReadMem(textStart, scanSize);
        if (bytes.Length < scanSize)
        {
            Console.WriteLine($"  [!] Read only {bytes.Length} bytes, adjusting");
            scanSize = bytes.Length;
        }

        int retCount = 0;
        for (int i = 0; i < scanSize - 3; i += 4)
        {
            // ARM64 ret instruction: C0 03 5F D6 (little-endian D65F03C0)
            if (bytes[i] == 0xC0 && bytes[i+1] == 0x03 &&
                bytes[i+2] == 0x5F && bytes[i+3] == 0xD6)
                retCount++;
        }

        double density = (double)retCount / (scanSize / 1024.0);
        Console.WriteLine($"  Found: {retCount} ret instructions in {(scanSize/1024)}KB");
        Console.WriteLine($"  Density: {density:F2} ret/KB");
        Console.WriteLine($"  Estimated total for 553KB .text: ~{(int)(density * 553)} gadgets");
    }

    // -------------------------------------------------------------------
    // CHECK 4: Scan entire address space for RWX regions (claim AOTSEC-03)
    // -------------------------------------------------------------------
    static void ScanForRwxRegions()
    {
        var mbi = new MEMORY_BASIC_INFORMATION();
        IntPtr addr = IntPtr.Zero;
        int mbiSize = Marshal.SizeOf<MEMORY_BASIC_INFORMATION>();
        int rwxCount = 0;

        Console.WriteLine("  Scanning address space for PAGE_EXECUTE_READWRITE regions...");

        while (true)
        {
            var result = VirtualQuery(addr, out mbi, (IntPtr)mbiSize);
            if (result == IntPtr.Zero) break;

            if (mbi.Protect == PAGE_EXECUTE_READWRITE && mbi.State == MEM_COMMIT)
            {
                rwxCount++;
                if (rwxCount <= 5)
                    Console.WriteLine($"  RWX: 0x{mbi.BaseAddress:X} size=0x{(ulong)mbi.RegionSize:X}");
            }

            addr = mbi.BaseAddress + mbi.RegionSize;
            if ((ulong)addr > 0x7FFFFFFFFFFF) break;
        }

        Console.WriteLine($"  Total RWX regions: {rwxCount}");
        if (rwxCount == 0)
            Console.WriteLine("  [SECURE] Zero RWX = all code AOT compiled (confirms AOTSEC-03)");
    }
}
