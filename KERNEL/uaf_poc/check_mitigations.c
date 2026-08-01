/*
 * check_mitigations.c — Mitigation & Hardware Protection Checker
 * 
 * Verifies system and process-level security mitigations:
 *  A. PAC (Pointer Authentication Code) / Hardware Security
 *  B. BTI (Branch Target Identification) / CET User Mode Policy
 *  C. CFG (Control Flow Guard) & ASLR / DEP Policies
 */

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

#ifndef ProcessUserCetPolicy
#define ProcessUserCetPolicy 21
typedef struct _PROCESS_MITIGATION_USER_CET_POLICY_FALLBACK {
    union {
        DWORD Flags;
        struct {
            DWORD EnableUserCetSetContextIpValidation : 1;
            DWORD AuditUserCetSetContextIpValidation : 1;
            DWORD EnableUserCetSetContextIpValidationRelaxedMode : 1;
            DWORD ReservedFlags : 29;
        } Bits;
    } U;
} PROCESS_MITIGATION_USER_CET_POLICY_FALLBACK;
#endif

void InspectMitigations(void)
{
    HANDLE hProcess = GetCurrentProcess();
    BOOL status;
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfgPolicy;
    PROCESS_MITIGATION_USER_CET_POLICY_FALLBACK cetPolicy;
    PROCESS_MITIGATION_DEP_POLICY depPolicy;
    PROCESS_MITIGATION_ASLR_POLICY aslrPolicy;

    memset(&cfgPolicy, 0, sizeof(cfgPolicy));
    memset(&cetPolicy, 0, sizeof(cetPolicy));
    memset(&depPolicy, 0, sizeof(depPolicy));
    memset(&aslrPolicy, 0, sizeof(aslrPolicy));

    printf("====================================================\n");
    printf("  Windows 11 ARM64 Mitigation Status Inspector\n");
    printf("====================================================\n\n");

    /* Architecture Inspection */
    #if defined(_M_ARM64)
        printf("[+] Binary Architecture : Native ARM64 (AArch64)\n");
    #elif defined(_M_X64)
        printf("[+] Binary Architecture : x64 (CHPE Emulation Layer)\n");
    #else
        printf("[+] Binary Architecture : x86 (WOW64 Emulation Layer)\n");
    #endif

    /* C. Control Flow Guard (CFG) */
    status = GetProcessMitigationPolicy(hProcess, ProcessControlFlowGuardPolicy, &cfgPolicy, sizeof(cfgPolicy));
    if (status) {
        printf("\n[C] Control Flow Guard (CFG):\n");
        printf("    - CFG Enabled            : %s\n", cfgPolicy.EnableControlFlowGuard ? "YES" : "NO");
        printf("    - Export Suppression     : %s\n", cfgPolicy.EnableExportSuppression ? "YES" : "NO");
        printf("    - Strict Mode            : %s\n", cfgPolicy.StrictMode ? "YES" : "NO");
    } else {
        printf("\n[C] Control Flow Guard (CFG) : Unable to query policy (Err: %lu)\n", GetLastError());
    }

    /* B. BTI / CET User Mode Policy (Branch Target Identification) */
    status = GetProcessMitigationPolicy(hProcess, (PROCESS_MITIGATION_POLICY)ProcessUserCetPolicy, &cetPolicy, sizeof(cetPolicy));
    if (status) {
        printf("\n[B] Branch Target Identification (BTI / CET):\n");
        printf("    - User CET / BTI Active  : %s\n", cetPolicy.U.Bits.EnableUserCetSetContextIpValidation ? "YES" : "NO");
    } else {
        printf("\n[B] Branch Target Identification (BTI / CET) : Not Active / Legacy Mode\n");
    }

    /* A. Pointer Authentication (PAC) / ARM Hardware Features */
    printf("\n[A] Pointer Authentication (PAC) & Hardware Security:\n");
    #if defined(_M_ARM64)
        printf("    - PAC Compiler Support   : %s\n", 
            #if defined(__ARM_FEATURE_PAC_DEFAULT)
                "ENABLED (__ARM_FEATURE_PAC_DEFAULT)"
            #else
                "Disabled in Compiler Flags"
            #endif
        );
        printf("    - ARM64 Atomic Ops (v8.1): %s\n", 
            IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE) ? "SUPPORTED" : "UNSUPPORTED");
    #else
        printf("    - PAC Status             : N/A (Binary running under x64 CHPE emulation)\n");
    #endif

    /* DEP / ASLR Policies */
    if (GetProcessMitigationPolicy(hProcess, ProcessDEPPolicy, &depPolicy, sizeof(depPolicy))) {
        printf("\n[+] DEP / NX Status:\n");
        printf("    - DEP Enabled            : %s\n", depPolicy.Enable ? "YES" : "NO");
        printf("    - Permanent              : %s\n", depPolicy.Permanent ? "YES" : "NO");
    }

    if (GetProcessMitigationPolicy(hProcess, ProcessASLRPolicy, &aslrPolicy, sizeof(aslrPolicy))) {
        printf("\n[+] ASLR Status:\n");
        printf("    - High Entropy ASLR      : %s\n", aslrPolicy.EnableHighEntropy ? "YES" : "NO");
        printf("    - Force Relocate Images  : %s\n", aslrPolicy.EnableForceRelocateImages ? "YES" : "NO");
    }

    printf("\n====================================================\n");
}

int main(void)
{
    InspectMitigations();
    printf("\nPress Enter to exit...\n");
    getchar();
    return 0;
}
