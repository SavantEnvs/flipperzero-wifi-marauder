/*
 * Fuzz target: the byte stream the ESP32 Marauder board sends over UART.
 * The app's own RX path runs unchanged: wifi_marauder_uart_on_irq_cb() (registered through
 * furi_hal_serial_async_rx_start) splits console text from PCAP data on the [BUF/BEGIN] /
 * [BUF/CLOSE] markers into two stream buffers; the RX worker thread body drains them into the
 * console-output scene's callbacks (wifi_marauder_console_output_handle_rx_data_cb /
 * _rx_packets_cb), which append to the 4 KiB text-box store (with its truncation) and write
 * log / pcap files.
 *
 * Input layout: byte 0 = flags (bit0 log file open, bit1 pcap file open), then a sequence of
 * chunks, each [len byte][len bytes of UART data]; after each chunk the RX worker runs, the way
 * the real thread wakes after a burst of interrupts. A chunk is delivered byte by byte through the
 * interrupt callback.
 * State kept across inputs: none — the WifiMarauderApp, the UART object (with its marker state)
 * and the text-box store are created and freed per input.
 */
#include "wifi_marauder_app_i.h"

void wifi_marauder_console_output_handle_rx_data_cb(uint8_t* buf, size_t len, void* context);
void wifi_marauder_console_output_handle_rx_packets_cb(uint8_t* buf, size_t len, void* context);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if(size < 1) return 0;
    shim_reset();
    WifiMarauderApp* app = calloc(1, sizeof(WifiMarauderApp));
    app->text_box_store = furi_string_alloc();
    app->log_file = storage_file_alloc(NULL);
    app->capture_file = storage_file_alloc(NULL);
    if(data[0] & 1) app->is_writing_log = storage_file_open(app->log_file, "log.txt", FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(data[0] & 2) app->is_writing_pcap = storage_file_open(app->capture_file, "cap.pcap", FSAM_WRITE, FSOM_CREATE_ALWAYS);
    app->uart = wifi_marauder_usart_init(app);
    wifi_marauder_uart_set_handle_rx_data_cb(app->uart, wifi_marauder_console_output_handle_rx_data_cb);
    wifi_marauder_uart_set_handle_rx_pcap_cb(app->uart, wifi_marauder_console_output_handle_rx_packets_cb);

    size_t i = 1;
    while(i < size) {
        size_t n = data[i++];
        if(n > size - i) n = size - i;
        shim_uart_rx(data + i, n);
        i += n;
        shim_uart_pump();
    }

    wifi_marauder_uart_free(app->uart);
    storage_file_free(app->log_file);
    storage_file_free(app->capture_file);
    furi_string_free(app->text_box_store);
    free(app);
    shim_release_app_allocs();
    return 0;
}
