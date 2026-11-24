/*
 * Force-included (-include) into the app's own translation units ONLY (not the shim, harnesses or
 * self-test). Routes the app's heap calls through the shim's tracking allocator so that:
 *  1. allocations are ZEROED, like the Flipper firmware allocator (furi memmgr over its FreeRTOS
 *     heap) that the app is written for — e.g. wifi_marauder_uart_init() mallocs its state and never
 *     clears mark_test_idx/pcap, which on the host would read uninitialised memory on every input;
 *  2. a harness can release whatever the app still holds at the end of an input
 *     (shim_release_app_allocs). The app leaks by design on every script load (the file buffer in
 *     wifi_marauder_script_parse_json, the cJSON tree in parse_raw, the cJSON_Print text in
 *     save_json), which would otherwise grow a persistent fuzzer to its RSS limit.
 * Blocks are really freed, so ASan still reports overflows, use-after-free and double free.
 */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
void* shim_app_malloc(size_t n);
void* shim_app_calloc(size_t n, size_t m);
void* shim_app_realloc(void* p, size_t n);
void shim_app_free(void* p);
char* shim_app_strdup(const char* s);
#define malloc shim_app_malloc
#define calloc shim_app_calloc
#define realloc shim_app_realloc
#define free shim_app_free
#define strdup shim_app_strdup
