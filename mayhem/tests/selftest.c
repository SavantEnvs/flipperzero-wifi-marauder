/*
 * Functional self-test for the app code the fuzz targets reach (mayhem/test.sh runs it).
 * Known inputs -> known outputs:
 *  - a script file parses into the expected meta + stages, re-saves to the expected JSON, and the
 *    executor emits exactly the expected ESP32 command stream;
 *  - malformed script JSON yields an empty, named script;
 *  - the UART RX path splits console text from PCAP data on the [BUF/BEGIN]/[BUF/CLOSE] markers,
 *    feeds the console text box / log file / pcap file, and truncates the 4 KiB text-box store.
 * Prints one "PASS name" / "FAIL name" line per check and a final "SUMMARY passed=P failed=F".
 */
#include <unistd.h>
#include "wifi_marauder_app_i.h"

void wifi_marauder_console_output_handle_rx_data_cb(uint8_t* buf, size_t len, void* context);
void wifi_marauder_console_output_handle_rx_packets_cb(uint8_t* buf, size_t len, void* context);

static int passed, failed;
static void check(const char* name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if(ok) passed++; else failed++;
}
static void check_str(const char* name, const char* got, const char* want) {
    int ok = got && strcmp(got, want) == 0;
    if(!ok) printf("  got:  [%s]\n  want: [%s]\n", got ? got : "(null)", want);
    check(name, ok);
}

static char* write_tmp(const char* content) {
    const char* tmpdir = getenv("TMPDIR");
    static char path[4096];
    snprintf(path, sizeof(path), "%s/selftest_script_XXXXXX", (tmpdir && *tmpdir) ? tmpdir : "/tmp");
    int fd = mkstemp(path);
    if(fd < 0) return NULL;
    if(write(fd, content, strlen(content)) < 0) { close(fd); return NULL; }
    close(fd);
    return path;
}

static void test_script(void) {
    const char* json =
        "{\"meta\":{\"description\":\"demo script\",\"enableLed\":false,\"savePcap\":true,\"repeat\":3},"
        "\"stages\":{\"scan\":{\"type\":\"ap\",\"channel\":6,\"timeout\":1},"
        "\"select\":{\"type\":\"ap\",\"filter\":\"all\"},"
        "\"deauth\":{\"timeout\":2},"
        "\"beaconList\":{\"ssids\":[\"one\",\"two\"],\"timeout\":1},"
        "\"exec\":{\"command\":\"clearlist -a\"}}}";
    shim_reset();
    WifiMarauderApp app;
    memset(&app, 0, sizeof(app));
    WifiMarauderUart* uart = wifi_marauder_usart_init(&app);
    char* path = write_tmp(json);
    WifiMarauderScript* s = wifi_marauder_script_parse_json(NULL, path);
    unlink(path);
    check("script_parsed", s != NULL);
    if(!s) { wifi_marauder_uart_free(uart); return; }
    check_str("script_description", s->description, "demo script");
    check("script_repeat", s->repeat == 3);
    check("script_enable_led_false", s->enable_led == WifiMarauderScriptBooleanFalse);
    check("script_save_pcap_true", s->save_pcap == WifiMarauderScriptBooleanTrue);
    int n = 0;
    for(WifiMarauderScriptStage* st = s->first_stage; st; st = st->next_stage) n++;
    check("script_stage_count", n == 5);
    WifiMarauderScriptStageScan* scan = s->first_stage ? s->first_stage->stage : NULL;
    check("script_scan_stage", s->first_stage && s->first_stage->type == WifiMarauderScriptStageTypeScan &&
                                   scan->channel == 6 && scan->timeout == 1);
    check("script_has_beacon_list", wifi_marauder_script_has_stage(s, WifiMarauderScriptStageTypeBeaconList));
    check("script_has_no_sniff_raw", !wifi_marauder_script_has_stage(s, WifiMarauderScriptStageTypeSniffRaw));

    wifi_marauder_script_save_json(NULL, "script.json", s);
    const char* saved = furi_string_get_cstr(shim_written);
    check("save_json_description", strstr(saved, "\"description\":\t\"demo script\"") != NULL);
    check("save_json_beacon_ssids", strstr(saved, "\"one\"") && strstr(saved, "\"two\""));
    check("save_json_exec_command", strstr(saved, "\"clearlist -a\"") != NULL);
    WifiMarauderScript* again = wifi_marauder_script_parse_raw(saved);
    int n2 = 0;
    for(WifiMarauderScriptStage* st = again ? again->first_stage : NULL; st; st = st->next_stage) n2++;
    check("save_json_roundtrip_stages", again && n2 == 5 && again->repeat == 3);
    if(again) wifi_marauder_script_free(again);

    furi_string_reset(shim_tx_log);
    WifiMarauderScriptWorker worker;
    memset(&worker, 0, sizeof(worker));
    worker.script = s;
    worker.uart = uart;
    worker.context = &worker;
    worker.is_running = true;
    wifi_marauder_script_execute_start(&worker);
    for(WifiMarauderScriptStage* st = s->first_stage; st; st = st->next_stage)
        wifi_marauder_script_execute_stage(st, &worker);
    check_str(
        "executor_command_stream",
        furi_string_get_cstr(shim_tx_log),
        "settings -s EnableLED disable\n"
        "settings -s SavePCAP enable\n"
        "\n"
        "channel -s 6\n"
        "scanall\n"
        "stopscan\n"
        "select -a all\n"
        "attack -t deauth\n"
        "stopscan\n"
        "clearlist -s\n"
        "ssid -a -n \"one\"\n"
        "ssid -a -n \"two\"\n"
        "attack -t beacon -l\n"
        "stopscan\n"
        "clearlist -a\n");
    wifi_marauder_script_free(s);
    wifi_marauder_uart_free(uart);
}

static void test_bad_script(void) {
    char* path = write_tmp("{\"stages\": [ {\"scan\": ");
    WifiMarauderScript* s = wifi_marauder_script_parse_json(NULL, path);
    check("bad_json_still_returns_script", s != NULL);
    if(s) {
        check("bad_json_no_stages", s->first_stage == NULL);
        check("bad_json_named_after_file", s->name && strncmp(s->name, "selftest_script_", 16) == 0);
        wifi_marauder_script_free(s);
    }
    unlink(path);
}

static void test_uart(void) {
    shim_reset();
    WifiMarauderApp* app = calloc(1, sizeof(WifiMarauderApp));
    app->text_box_store = furi_string_alloc();
    app->capture_file = storage_file_alloc(NULL);
    app->is_writing_pcap = storage_file_open(app->capture_file, "cap.pcap", FSAM_WRITE, FSOM_CREATE_ALWAYS);
    app->uart = wifi_marauder_usart_init(app);
    wifi_marauder_uart_set_handle_rx_data_cb(app->uart, wifi_marauder_console_output_handle_rx_data_cb);
    wifi_marauder_uart_set_handle_rx_pcap_cb(app->uart, wifi_marauder_console_output_handle_rx_packets_cb);

    const char* stream = "scan done[BUF/BEGIN]\xd4\xc3\xb2\xa1PKT[BUF/CLOSE]\n> [BUF/";
    shim_uart_rx((const uint8_t*)stream, strlen(stream));
    shim_uart_pump();
    check_str("uart_console_text", furi_string_get_cstr(app->text_box_store), "scan done\n> ");
    check_str("uart_pcap_bytes", furi_string_get_cstr(shim_written), "\xd4\xc3\xb2\xa1PKT");
    check("uart_text_len_tracked", app->text_box_store_strlen == strlen("scan done\n> "));
    /* A pending marker prefix ("[BUF/") is held back, then flushed as data on the next
     * non-matching byte. */
    shim_uart_rx((const uint8_t*)"!", 1);
    shim_uart_pump();
    check_str("uart_partial_marker_flushed", furi_string_get_cstr(app->text_box_store), "scan done\n> [BUF/!");

    /* Overflowing the 4 KiB store keeps only its tail. */
    char big[1001];
    memset(big, 'A', 1000);
    big[1000] = '\0';
    for(int i = 0; i < 5; i++) { shim_uart_rx((const uint8_t*)big, 1000); shim_uart_pump(); }
    size_t len = furi_string_size(app->text_box_store);
    check("uart_store_truncated", len < WIFI_MARAUDER_TEXT_BOX_STORE_SIZE && len > 1000);
    const char* t = furi_string_get_cstr(app->text_box_store);
    check("uart_store_keeps_tail", t[len - 1] == 'A' && strstr(t, "scan done") == NULL);

    wifi_marauder_uart_free(app->uart);
    storage_file_free(app->capture_file);
    furi_string_free(app->text_box_store);
    free(app);
}

int main(void) {
    test_script();
    test_bad_script();
    test_uart();
    printf("SUMMARY passed=%d failed=%d\n", passed, failed);
    return failed ? 1 : 0;
}
