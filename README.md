# Windows Internals

Deep dive into Windows x64 internals, including TLS/FLS, dynamic function tables, and kernel debugging.

## Topics

| File | Topic |
|------|-------|
| `TLS.md` | Thread Local Storage internals |
| `FLS.md` | Fiber Local Storage internals |
| `growable_ft_repro_x64.cpp` | Dynamic function table reproduction (v1) |
| `growable_ft_repro_x64 – v2.cpp` | Dynamic function table reproduction (v2) |
| `Reverse Engineering Windows x64 Unwind Infrastructure...md` | Analysis of `LdrpInvertedFunctionTable` |

### KERNEL

Kernel-level debugging and analysis:

| File | Description |
|------|-------------|
| `KERNEL/ColdStart.md` | Cold start analysis |
| `KERNEL/windbg_agent.md` | WinDbg agent setup |
| `KERNEL/START_DEBUG_SESSION.sh` | Debug session launcher |
| `KERNEL/readme.md` | Kernel debugging notes |

## Usage

For kernel debugging, start a session with:

```bash
cd KERNEL
./START_DEBUG_SESSION.sh
```

## License

See [LICENSE](LICENSE) for details.
