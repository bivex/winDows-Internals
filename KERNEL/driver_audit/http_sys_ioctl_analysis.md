# HTTP.sys IOCTL Attack Surface Analysis

**Target:** Windows 11 ARM64 (Build 26100) via WinDbg MCP
**Date:** 2026-05-29
**Driver:** `\Driver\HTTP` at `ffff8a88f4b4edf0`

## Driver Overview

- **3 device objects**, all `FILE_DEVICE_NETWORK` (0x12)
- **Characteristics:** `FILE_DEVICE_SECURE_OPEN` (0x20000)
- **Custom IOCTL handler:** `HTTP!UxDeviceControl` at `fffff802b017de30`
- **Fast I/O path:** `HTTP!UxFastIoDeviceControl` at `fffff802b017df80`
- **Security:** DACL grants wide access (Authenticated Users, Guests, AppContainers = `0x0012019f`)
- **No SE_DACL_PROTECTED** flag - DACL can be modified by inheritance

## IOCTL Dispatch Architecture

`UxDeviceControl` uses a table-based dispatch:
1. Extracts IOCTL code from `IRP->CurrentStackLocation->Parameters.DeviceIoControl.IoControlCode`
2. Extracts function code via `ubfx w2, w8, #2, #0xC` (bits 2-13, 12-bit value)
3. Bounds check: function code must be < 0x40 (64 max)
4. Validates against dispatch table: `HTTP!UxModuleEntries + 0x9F0`
5. Entry size: 0x18 bytes (24 bytes): `[IOCTL_code(4B), padding(4B), handler(8B), extra(8B)]`
6. CFG-protected indirect call via `nt!KscpCfgCheckUserCallTargetEs`

## Complete IOCTL Table

| # | IOCTL Code | Func | Method | Access | IOCTL Handler | FastIO Handler |
|---|-----------|------|--------|--------|---------------|----------------|
| 0 | 0x00128000 | 0 | BUFFERED | WRITE | UlCreateServerSessionIoctl | - |
| 1 | 0x00128004 | 1 | BUFFERED | WRITE | UlCloseServerSessionIoctl | - |
| 2 | 0x0012400a | 2 | OUT_DIRECT | WRITE | UlQueryServerSessionIoctl | - |
| 3 | 0x0012800d | 3 | IN_DIRECT | WRITE | UlSetServerSessionIoctl | - |
| 4 | 0x00128010 | 4 | BUFFERED | WRITE | UlCreateUrlGroupIoctl | - |
| 5 | 0x00128014 | 5 | BUFFERED | WRITE | UlDeleteUrlGroupIoctl | - |
| 6 | 0x0012401a | 6 | OUT_DIRECT | WRITE | UlQueryUrlGroupIoctl | - |
| 7 | 0x0012801d | 7 | IN_DIRECT | WRITE | UlSetUrlGroupIoctl | - |
| 8 | 0x00128020 | 8 | BUFFERED | WRITE | UlAddUrlToUrlGroupIoctl | - |
| 9 | 0x00128024 | 9 | BUFFERED | WRITE | UlRemoveUrlFromUrlGroupIoctl | - |
| 10 | 0x0012402a | 10 | OUT_DIRECT | WRITE | UlQueryRequestQueueIoctl | - |
| 11 | 0x0012802d | 11 | IN_DIRECT | WRITE | UlSetRequestQueueIoctl | - |
| 12 | 0x00124030 | 12 | BUFFERED | WRITE | UlShutdownRequestQueueIoctl | - |
| 13 | 0x00124036 | 13 | OUT_DIRECT | WRITE | **UlReceiveHttpRequestIoctl** | UlReceiveHttpRequestFastIo |
| 14 | 0x0012403b | 14 | **NEITHER** | WRITE | UlReceiveEntityBodyIoctl | UlReceiveEntityBodyFastIo |
| 15 | 0x0012403f | 15 | **NEITHER** | WRITE | UlSendHttpResponseIoctl | UlSendHttpResponseFastIo |
| 16 | 0x00124043 | 16 | **NEITHER** | WRITE | UlSendEntityBodyIoctl | - |
| 17 | 0x00124044 | 17 | BUFFERED | WRITE | UlFlushResponseCacheIoctl | - |
| 18 | 0x00128048 | 18 | BUFFERED | WRITE | UlWaitForDemandStartIoctl | - |
| 19 | 0x0012404c | 19 | BUFFERED | WRITE | UlWaitForDisconnectIoctl | - |
| 20 | 0x00124052 | 20 | OUT_DIRECT | WRITE | UlReceiveClientCertIoctl | - |
| 21 | 0x00124056 | 21 | OUT_DIRECT | WRITE | UlGetCountersIoctl | - |
| 22 | 0x00124058 | 22 | BUFFERED | WRITE | UlAddFragmentToCacheIoctl | - |
| 23 | 0x0012405f | 23 | **NEITHER** | WRITE | UlReadFragmentFromCacheIoctl | UlReadFragmentFromCacheFastIo |
| 24 | 0x00124063 | 24 | **NEITHER** | WRITE | UlCancelHttpRequestIoctl | UlCancelHttpRequestFastIo |
| 25 | 0x00124066 | 25 | OUT_DIRECT | WRITE | UlPrepareUrlIoctl | - |
| 26 | 0x00124068 | 26 | BUFFERED | WRITE | UlEvaluateRequestIoctl | UlEvaluateRequestFastIo |
| 27 | 0x0012406c | 27 | BUFFERED | WRITE | UlQueryRequestIoctl | UlQueryRequestFastIo |
| 28 | 0x00124070 | 28 | BUFFERED | WRITE | UlDeclarePushIoctl | - |
| 29 | 0x00124074 | 29 | BUFFERED | WRITE | UlDelegateRequestIoctl | UlDelegateRequestFastIo |
| 30 | 0x00124078 | 30 | BUFFERED | WRITE | UlFindUrlGroupIdIoctl | - |
| 31 | 0x0012407c | 31 | BUFFERED | WRITE | UlDelegateRequestExIoctl | UlDelegateRequestExFastIo |
| 32 | 0x00124080 | 32 | BUFFERED | WRITE | UlSetRequestIoctl | UlSetRequestFastIo |
| 33 | 0x00124086 | 33 | OUT_DIRECT | WRITE | UlQueryServiceConfigIoctl | - |
| 34 | 0x00128089 | 34 | BUFFERED | WRITE | UlSetServiceConfigIoctl | - |
| 35 | 0x0012808d | 35 | BUFFERED | WRITE | UlUpdateServiceConfigIoctl | - |
| 36 | 0x00128091 | 36 | BUFFERED | WRITE | UlDeleteServiceConfigIoctl | - |
| 37 | 0x00124096 | 37 | OUT_DIRECT | WRITE | UlControlServiceIoctl | - |
| 38 | 0x00124098 | 38 | BUFFERED | WRITE | UlFeatureSupportedIoctl | UlFeatureSupportedFastIo |
| 39-42 | 0x0012409c-0x001240a8 | 39-42 | BUFFERED | WRITE | UcUnusedIoctl (placeholder) | - |
| 43 | 0x001240b4 | 43 | OUT_DIRECT | WRITE | UcCreateClientConnectionIoctl | - |
| 44 | 0x001240b8 | 44 | BUFFERED | WRITE | UcStartClientConnectionIoctl | - |
| 45 | 0x001240bc | 45 | BUFFERED | WRITE | UcCloseClientConnectionIoctl | - |
| 46 | 0x001240c0 | 46 | BUFFERED | WRITE | UcCreateClientStreamIoctl | - |
| 47 | 0x001240c7 | 47 | NEITHER | WRITE | UcSendHeadersClientStreamIoctl | UcSendHeadersClientStreamFastIo |
| 48 | 0x001240cb | 48 | NEITHER | WRITE | UcReceiveHeadersClientStreamIoctl | UcReceiveHeadersClientStreamFastIo |
| 49 | 0x001240cf | 49 | NEITHER | WRITE | UcReceiveEntityBodyClientStreamIoctl | UcReceiveEntityBodyClientStreamFastIo |
| 50 | 0x001240d0 | 50 | BUFFERED | WRITE | UcCloseClientStreamIoctl | - |
| 51 | 0x001240d7 | 51 | NEITHER | WRITE | UcSendEntityBodyClientStreamIoctl | UcSendEntityBodyClientStreamFastIo |
| 52 | 0x001240d8 | 52 | NEITHER | WRITE | UcQueryClientConnectionIoctl | UcQueryClientConnectionFastIo |
| 53 | 0x001240dc | 53 | NEITHER | WRITE | UcQueryClientStreamIoctl | UcQueryClientStreamFastIo |
| 54 | 0x001240e3 | 54 | NEITHER | WRITE | UcAbortClientConnectionIoctl | UcAbortClientConnectionFastIo |
| 55 | 0x001240e7 | 55 | NEITHER | WRITE | UcAbortClientStreamIoctl | UcAbortClientStreamFastIo |
| 56 | 0x001240e8 | 56 | BUFFERED | WRITE | UcSetClientConnectionIoctl | UcSetClientConnectionFastIo |
| 57 | 0x001240ec | 57 | BUFFERED | WRITE | UcWaitForClientConnectionDisconnectIoctl | - |
| 58 | 0x001240f0 | 58 | BUFFERED | WRITE | UcCreateClientCredentialIoctl | - |

## METHOD_NEITHER IOCTLs (HIGH RISK)

These IOCTLs pass raw user-mode pointers to the driver without I/O manager buffering:
- Input buffer = `Parameters.DeviceIoControl.Type3InputBuffer` (user VA)
- Output buffer = `Irp->UserBuffer` (user VA)

### Server-side (Ul* functions):
| IOCTL | Function | Handler |
|-------|----------|---------|
| 0x0012403b | UlReceiveEntityBodyIoctl | Reads entity body from HTTP request |
| 0x0012403f | UlSendHttpResponseIoctl | Sends HTTP response |
| 0x00124043 | UlSendEntityBodyIoctl | Sends entity body in response |
| 0x0012405f | UlReadFragmentFromCacheIoctl | Reads from fragment cache |
| 0x00124063 | UlCancelHttpRequestIoctl | Cancels pending HTTP request |

### Client-side (Uc* functions):
| IOCTL | Function | Handler |
|-------|----------|---------|
| 0x001240c7 | UcSendHeadersClientStreamIoctl | Sends headers on client stream |
| 0x001240cb | UcReceiveHeadersClientStreamIoctl | Receives headers on client stream |
| 0x001240cf | UcReceiveEntityBodyClientStreamIoctl | Receives entity body on stream |
| 0x001240d7 | UcSendEntityBodyClientStreamIoctl | Sends entity body on stream |
| 0x001240d8 | UcQueryClientConnectionIoctl | Queries client connection info |
| 0x001240dc | UcQueryClientStreamIoctl | Queries client stream info |
| 0x001240e3 | UcAbortClientConnectionIoctl | Aborts client connection |
| 0x001240e7 | UcAbortClientStreamIoctl | Aborts client stream |

## Security Assessment

### Protections Present
1. **CFG (Control Flow Guard):** All indirect calls go through `nt!KscpCfgCheckUserCallTargetEs`
2. **PAC (Pointer Authentication):** `pacibsp`/`autibsp` on function entry/exit
3. **IOCTL validation:** Function code bounds-checked (< 0x40) and full IOCTL code validated
4. **FILE_DEVICE_SECURE_OPEN:** Security checks on open

### Risks
1. **Wide DACL:** Authenticated Users, Guests, and AppContainers all get `0x0012019f` access to the device
2. **No SE_DACL_PROTECTED:** DACL can be weakened via inheritance
3. **14 METHOD_NEITHER IOCTLs:** Direct user-buffer pointer access - classic TOCTOU/double-fetch attack surface
4. **60 IOCTL handlers:** Large attack surface with many code paths
5. **Kernel-mode HTTP parsing:** Complex protocol parsing (HTTP/1.1, chunked encoding, etc.) in kernel - history of vulns (MS15-034, CVE-2015-1635)
6. **FastIO path:** Additional attack surface via `UxFastIoDeviceControl` that bypasses IRP machinery

### Historical Vulnerabilities in HTTP.sys
- **MS15-034 (CVE-2015-1635):** Range header integer overflow → RCE + DoS
- **CVE-2021-31166:** HTTP Protocol Stack RCE
- Multiple denial-of-service vulnerabilities in request/response handling

### BYOVD Assessment
HTTP.sys is NOT a good BYOVD target:
- It's a Microsoft inbox driver, well-audited
- CFG + PAC make exploitation difficult
- But it IS a valuable fuzzing/auditing target due to:
  - Wide DACL (any user can send IOCTLs)
  - METHOD_NEITHER handlers (complex buffer validation)
  - Complex kernel-mode HTTP protocol parsing
