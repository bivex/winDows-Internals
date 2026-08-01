/*
 * uaf_spray_test.c — UAF verification with Heap Spray (100 objects)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int   id;
    char  name[32];
    void (*action)(void);
} KernelObject;

static void legit_action(void) {
    printf("[LEGIT] Legitimate function called!\n");
}

static void sprayed_action(void) {
    printf("[EXPLOIT] SPRAYED FUNCTION EXECUTED VIA UAF!\n");
}

int main(void)
{
    KernelObject *obj = (KernelObject*)malloc(sizeof(KernelObject));
    obj->id = 100;
    strncpy(obj->name, "OriginalObject", sizeof(obj->name));
    obj->action = legit_action;
    
    printf("[1] Initial call: ");
    obj->action();
    printf("[+] Allocated obj at: %p\n", (void*)obj);

    /* Free object */
    free(obj);
    printf("[+] Freed obj at: %p\n", (void*)obj);

    /* Heap Spray: allocate 100 objects of identical size to fill freelist slot */
    KernelObject *spray_arr[100];
    int hit_index = -1;

    for (int i = 0; i < 100; i++) {
        spray_arr[i] = (KernelObject*)malloc(sizeof(KernelObject));
        spray_arr[i]->id = 200 + i;
        spray_arr[i]->action = sprayed_action;

        if (spray_arr[i] == obj) {
            hit_index = i;
        }
    }

    if (hit_index != -1) {
        printf("[+] Heap Spray Success! Chunk reused at index %d (%p)\n", hit_index, (void*)spray_arr[hit_index]);
    } else {
        printf("[-] Heap Chunk was not immediately reused by spray.\n");
    }

    printf("[2] Call via dangling pointer (obj->action): ");
    obj->action();

    for (int i = 0; i < 100; i++) {
        free(spray_arr[i]);
    }

    return 0;
}
