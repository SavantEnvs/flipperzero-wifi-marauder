/*
 * Fuzz target: automation script files (apps_data/marauder/scripts/*.json on the SD card).
 * Same path the app takes when the user picks a script: wifi_marauder_script_parse_json() reads the
 * file through the storage API (cJSON + the meta/stage loaders), the script editor re-saves it with
 * wifi_marauder_script_save_json(), and the script worker turns every stage into ESP32 commands with
 * wifi_marauder_script_execute_start()/wifi_marauder_script_execute_stage(), then frees it.
 * Stages are executed once each (the JSON `repeat` count only re-runs the same stages; furi_delay_ms
 * is a no-op in the shim, so stage timeouts cost nothing).
 * State kept across inputs: none — the shim's tx/write capture buffers are reset per input, and
 * whatever the app leaked (it leaks its file buffer, cJSON tree and printed JSON by design) is
 * released by shim_release_app_allocs() at the end of each input.
 */
#include <unistd.h>
#include "wifi_marauder_app_i.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    /* storage_file_read's result lands in a uint16_t in wifi_marauder_script_parse_json: the app
     * only handles script files below 64 KiB. */
    if(size >= 65535) return -1;
    const char* tmpdir = getenv("TMPDIR");
    char path[4096];
    snprintf(path, sizeof(path), "%s/marauder_script_XXXXXX", (tmpdir && *tmpdir) ? tmpdir : "/tmp");
    int fd = mkstemp(path);
    if(fd < 0) return 0;
    size_t off = 0;
    while(off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if(w <= 0) break;
        off += (size_t)w;
    }
    close(fd);

    shim_reset();
    WifiMarauderApp app;
    memset(&app, 0, sizeof(app));
    WifiMarauderUart* uart = wifi_marauder_usart_init(&app);

    WifiMarauderScript* script = wifi_marauder_script_parse_json(NULL, path);
    unlink(path);
    if(script) {
        wifi_marauder_script_save_json(NULL, "script.json", script);

        WifiMarauderScriptWorker worker;
        memset(&worker, 0, sizeof(worker));
        worker.script = script;
        worker.uart = uart;
        worker.context = &worker;
        worker.is_running = true;
        wifi_marauder_script_execute_start(&worker);
        for(WifiMarauderScriptStage* st = script->first_stage; st; st = st->next_stage) {
            wifi_marauder_script_execute_stage(st, &worker);
        }
        wifi_marauder_script_free(script);
    }
    wifi_marauder_uart_free(uart);
    shim_release_app_allocs();
    return 0;
}
