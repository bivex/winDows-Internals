# CDB.EXE User-Mode Debugging Guide: Windows 11 ARM64 & .NET Internals

**Date:** 2026-10-24  
**Environment:** Windows 11 Pro ARM64 (Build 26100+), Parallels Desktop on Apple Silicon  
**Tool Path:** `C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe`  
**Host Controller:** macOS host via `prlctl exec "Windows 11 Pro (Debugger)"`  

---

## 1. Executive Overview

`cdb.exe` (Microsoft Console Debugger) is the headless, scriptable command-line engine of **WinDbg**. It shares the identical DbgEng core (`dbgeng.dll`), symbol engine (`dbghelp.dll`), and SOS extension support as GUI WinDbg (`DbgX.Shell.exe`), but allows fully automated, reproducible user-mode debugging sessions without GUI dependencies or interactive terminal hangs.

### Key Use Cases:
1. **Automated CI/CD / Remote Testing:** Launching, breaking, dumping call stacks, and exiting automatically via one-line host commands.
2. **Reverse-Engineering Execution Lifecycles:** Catching DLL load events (`sxe ld`), setting native/managed breakpoints, and disassembling JIT code.
3. **Dual-Architecture Analysis:** Debugging mixed ARM64 / ARM64EC / AMD64 processes on Apple Silicon.

---

## 2. Launching & Connecting `cdb.exe`

### 2.1 From Host (macOS) via `prlctl exec`

All commands can be dispatched directly from the macOS terminal using `prlctl exec`.

#### Golden Rules for Scripting with `prlctl exec`:
- Always terminate command strings with `; q` so `cdb.exe` exits cleanly and does not leave a zombie process.
- Wrap internal debugger commands in `-c "..."`.
- Use `cmd /c` inside the VM to avoid PowerShell string-escaping traps when passing quotes.

```bash
# Basic template from macOS host:
prlctl exec "Windows 11 Pro (Debugger)" cmd /c \
  "\"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe\" -c \"<COMMANDS>; q\" <TARGET_EXE>"
```

---

### 2.2 Connection Modes

| Mode | Command Line | Description |
|---|---|---|
| **Spawn Target** | `cdb.exe -c "<cmds>; q" C:\path\app.exe` | Launches new process, breaks at initial loader breakpoint (`LdrpDoDebuggerBreak`). |
| **Attach by PID** | `cdb.exe -p <PID> -c "<cmds>; q"` | Attaches to an already running process by process ID. |
| **Attach by Name** | `cdb.exe -pn <app.exe> -c "<cmds>; q"` | Attaches to a running process by image name. |
| **Non-Invasive Attach** | `cdb.exe -pv -p <PID> -c "<cmds>; q"` | Suspends process and reads memory/registers without injecting a debug port. |
| **Start Remote Server** | `cdb.exe -server tcp:port=5005 C:\path\app.exe` | Exposes a DbgEng server listening on TCP port 5005. |
| **Connect to Remote** | `cdb.exe -remote tcp:server=10.211.55.5,port=5005` | Connects client debugger to the remote server session. |

---

## 3. Essential `cdb.exe` Configuration & Commands

### 3.1 Symbols Setup

```text
.symfix C:\Symbols
.sympath+ srv*C:\Symbols*https://msdl.microsoft.com/download/symbols
.reload /f
```
*Tip:* On initial runs, symbol downloading may take 10–30 seconds. Pass `-g -G` to allow execution through non-target breakpoints.

---

### 3.2 Event Filters (`sxe` / `sxd`)

Control when the debugger halts during process execution:

```text
sxe ld:hostfxr.dll       # Break immediately when hostfxr.dll is mapped into memory
sxe ld:coreclr.dll       # Break when CoreCLR execution engine loads
sxe ld:clrjit.dll        # Break when RyuJIT loads
sxe ld:mscoree.dll       # Break when .NET Framework shim loads
sxe ld:clr.dll           # Break when classic .NET Framework 4.8.1 loads
sxe -c "k 5; g" ld:*.dll # On any DLL load, print 5 stack frames and resume automatically
sxd av                   # Ignore first-chance Access Violations (0xC0000005)
sxe eh                   # Break on C++ and Win32 structured exceptions
```

---

### 3.3 Managed Debugging via SOS

The SOS debugging extension bridges native machine code and the managed CLR type system:

#### Loading SOS:
* **Modern .NET (.NET Core / 5 / 6 / 8 / 10):**
  ```text
  .loadby sos coreclr
  ```
* **Classic .NET Framework 4.8.1:**
  ```text
  .load C:\Windows\Microsoft.NET\Framework64\v4.0.30319\SOS.dll
  ```

#### Key SOS Inspection Commands:

| Command | Usage | Description |
|---|---|---|
| `!bpmd` | `!bpmd <Assembly.dll> <Namespace.Type.Method>` | Sets a pending breakpoint on a managed method (resolves before or during JIT compilation). |
| `!clrstack -a` | `!clrstack -a` | Prints managed call stack with arguments and local variables. |
| `!dumpmd` | `!dumpmd <MethodDesc>` | Dumps metadata, JIT status, and native code address for a method. |
| `!name2ee` | `!name2ee <Assembly.dll> <Type>` | Resolves class name to `MethodTable` address. |
| `!ip2md` | `!ip2md <Address>` | Maps a native machine code instruction pointer to its parent `MethodDesc`. |
| `!eeheap -gc` | `!eeheap -gc` | Dumps GC heap segments (Gen 0/1/2, LOH, POH) and total allocated memory. |
| `!clrthreads` | `!clrthreads` | Lists all threads registered in the CLR ThreadPool and GC runtime. |

---

## 4. Reproducible Recipe 1: Tracing Modern .NET 10 (ARM64 AppHost)

This test proves the complete execution pipeline:
$$\text{AppHost (wmain)} \longrightarrow \text{hostfxr} \longrightarrow \text{hostpolicy} \longrightarrow \text{coreclr} \longrightarrow \text{clrjit} \longrightarrow \text{Program.Main()}$$

### Step 1: Create and Build the Target Application (Inside VM)
```cmd
mkdir C:\test_net10
cd /d C:\test_net10
"C:\Program Files\dotnet\dotnet.exe" new console -n TestNet10 -o C:\test_net10 --force

echo using System; class Program { static int Main(string[] args) { Console.WriteLine("MAIN_REACHED_NET10"); return args.Length == 0 ? 7 : 9; } } > C:\test_net10\Program.cs

"C:\Program Files\dotnet\dotnet.exe" build C:\test_net10\TestNet10.csproj -c Release
```

### Step 2: Run Automated `cdb` Session from macOS Host
```bash
prlctl exec "Windows 11 Pro (Debugger)" cmd /c \
  "\"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe\" -c \"sxe ld:hostfxr.dll; g; bu hostfxr!hostfxr_main_startupinfo; sxe ld:coreclr.dll; g; bu coreclr!coreclr_execute_assembly; sxe ld:clrjit.dll; g; bu clrjit!getJit; g; k 10; g; k 10; g; q\" C:\test_net10\bin\Release\net10.0\TestNet10.exe"
```

### Step 3: Catch Managed `Main()` and Disassemble JIT Machine Code
```bash
prlctl exec "Windows 11 Pro (Debugger)" cmd /c \
  "\"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe\" -c \"sxe ld:coreclr.dll; g; bu coreclr!coreclr_execute_assembly; g; .loadby sos coreclr; !bpmd TestNet10.dll Program.Main; g; u @pc L15; r; q\" C:\test_net10\bin\Release\net10.0\TestNet10.exe"
```

#### Expected Output on Success:
```text
JITTED TestNet10!Program.Main(System.String[])
Setting breakpoint: bp 00007FFF5AD34E30 [Program.Main(System.String[])]
Breakpoint 1 hit
00007fff`5ad34e30 a9be7bfd stp   fp, lr, [sp, #-0x20]!
00007fff`5ad34e34 910003fd mov   fp, sp
00007fff`5ad34e38 f9000fa0 str   x0, [fp, #0x18]
00007fff`5ad34e58 d63f0020 blr   x1                    ; Call System.Console.WriteLine
00007fff`5ad34e60 b9400800 ldr   w0, [x0, #8]          ; Array Length check
00007fff`5ad34e64 34000080 cbz   w0, 00007fff`5ad34e74 ; Branch to return 7
00007fff`5ad34e68 52800120 mov   w0, #9                ; return 9
00007fff`5ad34e70 d65f03c0 ret
00007fff`5ad34e74 528000e0 mov   w0, #7                ; return 7
00007fff`5ad34e7c d65f03c0 ret
```

---

## 5. Reproducible Recipe 2: Tracing Classic .NET Framework 4.8.1

This test proves the multi-architecture transition:
$$\text{MSCOREE.DLL (ARM64EC)} \longrightarrow \text{\$iexit\_thunk} \longrightarrow \text{mscoreei (AMD64)} \longrightarrow \text{clr (AMD64)} \longrightarrow \text{\$ientry\_thunk} \longrightarrow \text{KERNEL32 (ARM64EC)}$$

### Step 1: Compile Test Binary (Inside VM)
```cmd
echo using System; class Program { static int Main(string[] args) { Console.WriteLine("MAIN_REACHED_NETFX"); return args.Length == 0 ? 7 : 9; } } > C:\test_netfx.cs

C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /out:C:\test_netfx.exe C:\test_netfx.cs
```

### Step 2: Run Automated `cdb` Session from macOS Host
```bash
prlctl exec "Windows 11 Pro (Debugger)" cmd /c \
  "\"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe\" -c \"sxe ld:mscoree.dll; sxe ld:clr.dll; g; bu mscoree!_CorExeMain; g; k 15; g; q\" C:\test_netfx.exe"
```

#### Expected Output on Success:
```text
  Arch   Child-SP          RetAddr               Call Site
 ARM64EC 00000000`00d7f1e0 00007ff8`23d608e0     ntdll!#RtlExitUserProcess+0x90
 ARM64EC 00000000`00d7f200 00007ff8`23de0f34     KERNEL32!#ExitProcessImplementation+0x10
 ARM64EC 00000000`00d7f210 00007ff8`0eb7882e     KERNEL32!$ientry_thunk$cdecl$d$i8+0x24   <-- Thunk back to ARM64EC
   AMD64 00000000`00d7f2c0 00007ff8`0eb7f344     mscoreei!RuntimeDesc::ShutdownAllActiveRuntimes+0x2fa
   AMD64 00000000`00d7f5a0 00007fff`e48f462b     mscoreei!CLRRuntimeHostInternalImpl::ShutdownAllRuntimesThenExit+0x14
   AMD64 00000000`00d7f5d0 00007fff`e48f44a9     clr!EEPolicy::ExitProcessViaShim+0x8b
   AMD64 00000000`00d7f630 00007fff`e48f43fe     clr!SafeExitProcess+0x9d
   AMD64 00000000`00d7f8b0 00007fff`e48f3f24     clr!HandleExitProcessHelper+0x3e
   AMD64 00000000`00d7f8e0 00007fff`e48f3e04     clr!_CorExeMainInternal+0xf8
   AMD64 00000000`00d7f970 00007ff8`0eb6d6da     clr!CorExeMain+0x14
   AMD64 00000000`00d7f9b0 00007ff8`0581f2cc     mscoreei!CorExeMain+0xfa
 ARM64EC 00000000`00d7fa10 00007ff8`058051a4     MSCOREE!$iexit_thunk$cdecl$i8$i8+0x1c    <-- Thunk to AMD64
 ARM64EC 00000000`00d7fa40 00007ff8`05805404     MSCOREE!#ShellShim__CorExeMain+0xe4
 ARM64EC 00000000`00d7fa70 00007ff8`23d615f8     MSCOREE!#_CorExeMain_Exported+0x14
```

---

## 6. Common Gotchas & Troubleshooting

| Symptom | Root Cause | Fix |
|---|---|---|
| `No runnable debuggees error` | Target process completed its execution and exited before the next command in `-c` executed. | Normal when target exits naturally; keep commands sequenced or terminate chain with `; q`. |
| `cdb hangs indefinitely` | Debugger hit an unhandled exception or breakpoint without a following `g` or `q`. | Ensure every command chain finishes with `; q`. If hung, kill via `prlctl exec "<VM>" taskkill /F /IM cdb.exe`. |
| `Failed to find a supported runtime within ...` | `!bpmd` invoked before `coreclr.dll` finished binding the target managed assembly. | Break on `coreclr!coreclr_execute_assembly` *before* issuing `.loadby sos coreclr` and `!bpmd`. |
| `Command aborted` from host | Target VM is currently paused at a kernel breakpoint in kernel WinDbg. | Resume the kernel debugger (`g` in WinDbg) so the Target OS scheduler can process the user-mode command. |
| `Illegal instruction - code c000001d` | Mismatched instruction set decoding when invoking non-native extensions under emulation. | Ensure using the matching architecture of SOS (`arm64` vs `amd64`). |
