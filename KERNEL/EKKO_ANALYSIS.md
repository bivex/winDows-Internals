# Ekko Sleep Obfuscation — Debugger Analysis (ARM64)

## Overview

Ekko is a sleep obfuscation technique that encrypts a process's memory while it sleeps, then decrypts it before resuming execution. It uses a **ROP chain via `NtContinue`** and **timer queues** to achieve this without blocking the calling thread.

## The ROP Chain Mechanism

Ekko creates 6 copies of the current thread's CONTEXT structure, each modified to point to the next "gadget" in the chain. When the timer fires, `NtContinue` is called as the callback — the kernel restores the crafted CONTEXT and execution continues at the gadget address.

**Chain flow:**
```
Timer fires → NtContinue(ctx1) → VirtualProtect(RW)
            → NtContinue(ctx2) → SystemFunction032(encrypt)
            → NtContinue(ctx3) → WaitForSingleObject(sleep)
            → NtContinue(ctx4) → SystemFunction032(decrypt)
            → NtContinue(ctx5) → VirtualProtect(RWX)
            → NtContinue(ctx6) → SetEvent(done)
```

## Key Kernel Functions

### ntdll!NtContinue

Address: `00007ff8'3b111470`

```asm
svc  #0x43      ; syscall 0x43 (67) to kernel
ret
```

Just a syscall stub — transitions to kernel mode.

### nt!NtContinue → nt!NtContinueEx → nt!KxContinue

- `KiContinue()` validates and prepares the CONTEXT
- Restores all registers from the CONTEXT structure
- Performs kernel→user transition at the new **Pc** (ARM64) / **Rip** (x64)

**nt!NtContinue** (`fffff801'e08264f4`):
```asm
nt!NtContinue:
    uxtb  w1,w1              ; zero-extend byte
    b     nt!NtContinueEx    ; branch to main implementation
```

**nt!NtContinueEx** (`fffff801'e0826480`):
```asm
stp   x19,x20,[sp,#-0x60]!  ; save callee-saved regs
stp   x21,x22,[sp,#0x10]
stp   x23,x24,[sp,#0x20]
stp   x25,x26,[sp,#0x30]
stp   x27,x28,[sp,#0x40]
stp   fp,lr,[sp,#0x50]
mov   x19,x0                 ; x19 = CONTEXT ptr
mov   x20,x1                 ; x20 = ContinueStatus
bl    nt!KiSaveVfpState      ; save NEON/VFP state
; ... checks for SVE ...
mov   x0,x19                 ; arg0 = CONTEXT
mov   x1,x20                 ; arg1 = ContinueStatus
mov   x2,sp                  ; arg2 = trap frame
bl    nt!KxContinue           ; do the actual CONTEXT restore
; ... restore regs and ret ...
```

**nt!KxContinue** (`fffff801'e04791c0`):
```asm
pacibsp                       ; pointer authentication
stp   fp,lr,[sp,#-0x60]!     ; save frame
; ... save callee-saved regs ...
bl    nt!_security_push_cookie
mov   x26,sp                 ; stack frame
mov   x24,x2                 ; save trap frame ptr
bl    nt!KiContinue           ; validate + prepare CONTEXT
; ... IRQL handling, processor selection ...
; Final: restores X0-X28, Fp, Lr, Sp, Pc from CONTEXT
; Then returns to userspace at CONTEXT.Pc
```

## Timer Queue Dispatch

### Timer Creation

```
CreateTimerQueueTimer (kernel32)
  → ntdll!RtlCreateTimer       (allocates 0x60-byte timer struct)
    → ntdll!TppInitializeTimer  (links into timer queue, sets callback)
    → ntdll!TpSetTimerEx        (arms the timer with due time)
```

**ntdll!RtlCreateTimer** (`00007ff8'3b1c0480`):
- Allocates timer object (0x60 bytes)
- Stores callback pointer and due time
- Links into timer queue linked list
- Calls `TpSetTimerEx` to arm

### Timer Firing

```
ntdll!TppTimerQueueExpiration   (dequeues expired timers)
  → dispatches callback         (NtContinue with crafted CONTEXT)
```

### Threadpool Worker Dispatch

**ntdll!TppWorkerThread** (`00007ff8'3b1bf390`):
```asm
; Dispatch based on function pointer comparison:
;   TppWorkpExecuteCallback     — work items
;   TppTimerQueueExpiration     — timer callbacks
;   generic indirect call       — other types
```

`WT_EXECUTEINTIMERTHREAD` (0x20) routes the callback to run on the timer thread itself, not a worker thread.

## ARM64 CONTEXT Structure

From `dt nt!_CONTEXT` on the target:

```
+0x000 ContextFlags  : Uint4B
+0x004 Cpsr          : Uint4B
+0x008 X0            : Uint8B    ← first arg (like Rcx on x64)
+0x010 X1            : Uint8B    ← second arg (like Rdx on x64)
+0x018 X2            : Uint8B    ← third arg (like R8 on x64)
+0x020 X3            : Uint8B    ← fourth arg (like R9 on x64)
+0x028 X4            : Uint8B
+0x030 X5            : Uint8B
+0x038 X6            : Uint8B
+0x040 X7            : Uint8B    ← eighth arg
+0x048 X8            : Uint8B
+0x050 X9            : Uint8B
+0x058 X10           : Uint8B
+0x060 X11           : Uint8B
+0x068 X12           : Uint8B
+0x070 X13           : Uint8B
+0x078 X14           : Uint8B
+0x080 X15           : Uint8B
+0x088 X16           : Uint8B
+0x090 X17           : Uint8B
+0x098 X18           : Uint8B
+0x0a0 X19           : Uint8B
+0x0a8 X20           : Uint8B
+0x0b0 X21           : Uint8B
+0x0b8 X22           : Uint8B
+0x0c0 X23           : Uint8B
+0x0c8 X24           : Uint8B
+0x0d0 X25           : Uint8B
+0x0d8 X26           : Uint8B
+0x0e0 X27           : Uint8B
+0x0e8 X28           : Uint8B
+0x0f0 Fp            : Uint8B    ← frame pointer (X29)
+0x0f8 Lr            : Uint8B    ← link register (X30)
+0x008 X             : [31] Uint8B  (alias for X0-X28, Fp)
+0x100 Sp            : Uint8B    ← stack pointer
+0x108 Pc            : Uint8B    ← program counter (like Rip on x64)
+0x110 V             : [32] _ARM64_NT_NEON128  ← SIMD registers
+0x310 Fpcr          : Uint4B
+0x314 Fpsr          : Uint4B
+0x318 Bcr           : [8] Uint4B    ← breakpoint control
+0x338 Bvr           : [8] Uint8B    ← breakpoint value
+0x378 Wcr           : [2] Uint4B    ← watchpoint control
+0x380 Wvr           : [2] Uint8B    ← watchpoint value
```

## Critical Finding: Ekko is x64-only

The Ekko source code uses x64 CONTEXT fields:

```c
// Ekko source — x64 CONTEXT layout
ctx1->Rcx = (DWORD64)ImageBase;        // x64: first arg
ctx1->Rdx = ImageSize;                  // x64: second arg
ctx1->R8  = PAGE_READWRITE;            // x64: third arg
ctx1->R9  = 0;                          // x64: fourth arg
ctx1->Rsp = Stack;                      // x64: stack pointer
ctx1->Rip = (DWORD64)VirtualProtect;   // x64: instruction pointer
```

These fields **do not exist** in the ARM64 CONTEXT structure. An ARM64 port would need:

```c
// ARM64 CONTEXT layout — hypothetical port
ctx1->X0  = (ULONG64)ImageBase;        // X0 = first arg
ctx1->X1  = ImageSize;                  // X1 = second arg
ctx1->X2  = PAGE_READWRITE;            // X2 = third arg
ctx1->X3  = 0;                          // X3 = fourth arg
ctx1->Sp  = Stack;                      // Sp = stack pointer
ctx1->Pc  = (ULONG64)VirtualProtect;   // Pc = execution address
```

### ARM64 vs x64 Calling Convention Differences

| Aspect | x64 | ARM64 |
|--------|-----|-------|
| First arg | Rcx | X0 |
| Second arg | Rdx | X1 |
| Third arg | R8 | X2 |
| Fourth arg | R9 | X3 |
| Args 5-8 | stack | X4-X7 |
| Stack ptr | Rsp | Sp |
| Instr ptr | Rip | Pc |
| Return addr | pushed to stack | Lr (X30) |
| Frame ptr | Rbp | Fp (X29) |
| Callee-saved | Rbx,Rbp,R12-R15 | X19-X28 |

### ROP Gadgets

On x64, Ekko uses `ret`-based gadgets (each `ret` pops the next address from the stack). On ARM64:
- `ret` branches to Lr (X30), not stack
- Need `br xN` or `blr xN` gadgets instead
- Gadgets must end with a way to load the next gadget address into a register and branch to it
- Entire gadget chain must be rebuilt for ARM64 instruction encoding

## Summary

| Component | x64 | ARM64 |
|-----------|-----|-------|
| NtContinue syscall | `syscall` | `svc #0x43` |
| CONTEXT size | ~1232 bytes | ~0x388 bytes |
| Instruction pointer | Rip (+0x0F8) | Pc (+0x108) |
| Stack pointer | Rsp (+0x098) | Sp (+0x100) |
| First function arg | Rcx (+0x078) | X0 (+0x008) |
| Gadget style | `ret` (stack pop) | `br xN` (register branch) |
| Ekko compatible? | Yes | **No — needs rewrite** |

**Bottom line**: Ekko as published targets x64 exclusively. The CONTEXT field names, calling convention, and ROP gadget strategy all need rewriting for ARM64.

---

*Analysis performed via WinDbg kernel debugging on Windows 11 ARM64 Build 26100 running in Parallels.*
