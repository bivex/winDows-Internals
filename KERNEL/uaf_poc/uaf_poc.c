/*
 * UAF Proof-of-Concept — Research & Educational Purposes
 *
 * Demonstrates a classic Use-After-Free in user-space C.
 * Run with AddressSanitizer to observe the detection:
 *
 *   macOS/Linux:
 *     clang -fsanitize=address -g -o uaf_poc uaf_poc.c && ./uaf_poc
 *
 *   Windows (MSVC + ASan):
 *     cl /fsanitize=address /Zi /Fe:uaf_poc.exe uaf_poc.c && uaf_poc.exe
 *
 * Expected ASan output:
 *   ERROR: AddressSanitizer: heap-use-after-free on address 0x...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int   id;
    char  name[32];
    void (*action)(void);      /* Function pointer — the UAF target */
} KernelObject;

static void legit_action(void) {
    printf("[INFO] Legitimate action executed.\n");
}

static void sprayed_action(void) {
    printf("[EXPLOIT] Heap spray landed! Attacker-controlled function pointer called.\n");
}

int main(void)
{
    /* 1. Allocate */
    KernelObject *obj = malloc(sizeof(KernelObject));
    obj->id = 42;
    strncpy(obj->name, "KernelJob", sizeof(obj->name));
    obj->action = legit_action;

    printf("[+] Allocated KernelObject at %p, id=%d\n", (void*)obj, obj->id);
    printf("[+] action ptr = %p\n", (void*)obj->action);
    obj->action();

    /* 2. Free — dangling pointer remains */
    free(obj);
    printf("[+] Freed KernelObject. Dangling pointer still at %p\n", (void*)obj);

    /* 3. Simulate heap spray — allocate same-sized block */
    /* In a real scenario this would be attacker-controlled data */
    KernelObject *spray = malloc(sizeof(KernelObject));
    spray->id = 0xDEADBEEF;
    spray->action = sprayed_action;  /* Overwrite vtable/function pointer */
    printf("[+] Spray allocation at %p\n", (void*)spray);

    /* 4. USE-AFTER-FREE: call via stale dangling pointer */
    printf("[!] Calling obj->action() via dangling pointer...\n");
    obj->action();  /* <-- UAF: obj was freed, spray now occupies same address */

    free(spray);
    return 0;
}
