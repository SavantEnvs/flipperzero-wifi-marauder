/*
 * Host-side stand-in for the Flipper Zero SDK headers (furi, furi_hal, gui, storage, dialogs, toolbox)
 * that this external app (.fap) includes. The app's own sources (script parser/executor, UART
 * demux, console-output RX callbacks) are compiled UNCHANGED against these declarations; the
 * implementations live in mayhem/shim/furi_shim.c. Only what the compiled translation units
 * reference is declared here.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>


typedef struct FuriString FuriString;
typedef struct FuriThread FuriThread;
typedef void* FuriThreadId;
typedef struct FuriStreamBuffer FuriStreamBuffer;
typedef struct Gui Gui; typedef struct ViewDispatcher ViewDispatcher; typedef struct SceneManager SceneManager;
typedef struct TextBox TextBox; typedef struct Storage Storage; typedef struct File File;
typedef struct DialogsApp DialogsApp; typedef struct VariableItemList VariableItemList;
typedef struct Widget Widget; typedef struct Submenu Submenu; typedef struct View View;
typedef struct FuriHalSerialHandle FuriHalSerialHandle;
typedef struct Icon Icon;
typedef struct { int type; uint32_t event; } SceneManagerEvent;
typedef struct { void* a; void* b; void* c; uint32_t n; } SceneManagerHandlers;
typedef enum { SceneManagerEventTypeCustom, SceneManagerEventTypeBack, SceneManagerEventTypeTick } SceneManagerEventType;
typedef enum { FSAM_READ = 1, FSAM_WRITE = 2 } FS_AccessMode;
typedef enum { FSOM_OPEN_EXISTING = 1, FSOM_OPEN_ALWAYS = 2, FSOM_OPEN_APPEND = 4, FSOM_CREATE_NEW = 8, FSOM_CREATE_ALWAYS = 16 } FS_OpenMode;
#define EXT_PATH(p) "/ext/" p
#define furi_assert(x) assert(x)
#define furi_check(x) assert(x)
#define UNUSED(x) (void)(x)

/* FuriString */
FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_set_str(const char* s);
void furi_string_free(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
void furi_string_set(FuriString* s, const char* c);
void furi_string_reset(FuriString* s);
size_t furi_string_size(const FuriString* s);
void furi_string_right(FuriString* s, size_t start);
void furi_string_cat_str(FuriString* s, const char* c);
int furi_string_cat_printf(FuriString* s, const char* fmt, ...);

/* storage */
File* storage_file_alloc(Storage* storage);
void storage_file_free(File* f);
bool storage_file_open(File* f, const char* path, FS_AccessMode access, FS_OpenMode mode);
bool storage_file_close(File* f);
bool storage_file_is_open(File* f);
uint64_t storage_file_size(File* f);
size_t storage_file_read(File* f, void* buf, size_t n);
size_t storage_file_write(File* f, const void* buf, size_t n);
bool storage_file_exists(Storage* storage, const char* path);
void path_extract_filename_no_ext(const char* path, FuriString* out);

/* dialogs */
typedef struct { const char* extension; const Icon* icon; } DialogsFileBrowserOptions;
void dialog_file_browser_set_basic_options(DialogsFileBrowserOptions* o, const char* ext, const Icon* icon);
bool dialog_file_browser_show(DialogsApp* d, FuriString* out, FuriString* start, const DialogsFileBrowserOptions* o);
void dialog_message_show_storage_error(DialogsApp* d, const char* msg);
extern const Icon I_Text_10x10;

/* kernel / threads / flags / stream buffers */
void furi_delay_ms(uint32_t ms);
typedef int32_t (*FuriThreadCallback)(void* context);
FuriThread* furi_thread_alloc(void);
FuriThread* furi_thread_alloc_ex(const char* name, uint32_t stack, FuriThreadCallback cb, void* ctx);
void furi_thread_set_name(FuriThread* t, const char* name);
void furi_thread_set_stack_size(FuriThread* t, size_t size);
void furi_thread_set_context(FuriThread* t, void* ctx);
void furi_thread_set_callback(FuriThread* t, FuriThreadCallback cb);
void furi_thread_start(FuriThread* t);
bool furi_thread_join(FuriThread* t);
void furi_thread_free(FuriThread* t);
FuriThreadId furi_thread_get_id(FuriThread* t);
#define FuriFlagWaitAny 0u
#define FuriFlagError 0x80000000u
#define FuriWaitForever 0xFFFFFFFFu
uint32_t furi_thread_flags_set(FuriThreadId id, uint32_t flags);
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout);
FuriStreamBuffer* furi_stream_buffer_alloc(size_t size, size_t trigger);
void furi_stream_buffer_free(FuriStreamBuffer* sb);
size_t furi_stream_buffer_send(FuriStreamBuffer* sb, const void* data, size_t len, uint32_t timeout);
size_t furi_stream_buffer_receive(FuriStreamBuffer* sb, void* data, size_t len, uint32_t timeout);

/* furi_hal serial */
typedef enum { FuriHalSerialIdUsart, FuriHalSerialIdLpuart } FuriHalSerialId;
typedef enum { FuriHalSerialRxEventData = 1, FuriHalSerialRxEventIdle = 2 } FuriHalSerialRxEvent;
typedef void (*FuriHalSerialAsyncRxCallback)(FuriHalSerialHandle* h, FuriHalSerialRxEvent e, void* ctx);
FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId id);
void furi_hal_serial_control_release(FuriHalSerialHandle* h);
void furi_hal_serial_init(FuriHalSerialHandle* h, uint32_t baud);
void furi_hal_serial_deinit(FuriHalSerialHandle* h);
void furi_hal_serial_tx(FuriHalSerialHandle* h, const uint8_t* data, size_t len);
void furi_hal_serial_async_rx_start(FuriHalSerialHandle* h, FuriHalSerialAsyncRxCallback cb, void* ctx, bool report_errors);
uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* h);

/* gui (only what the compiled scene references; all no-ops) */
typedef enum { TextBoxFontText, TextBoxFontHex } TextBoxFont;
typedef enum { TextBoxFocusStart, TextBoxFocusEnd } TextBoxFocus;
void text_box_reset(TextBox* t);
void text_box_set_font(TextBox* t, TextBoxFont f);
void text_box_set_focus(TextBox* t, TextBoxFocus f);
void text_box_set_text(TextBox* t, const char* text);
void view_dispatcher_send_custom_event(ViewDispatcher* v, uint32_t event);
void view_dispatcher_switch_to_view(ViewDispatcher* v, uint32_t view);
void scene_manager_set_scene_state(SceneManager* s, uint32_t scene, uint32_t state);
typedef struct VariableItem VariableItem;
typedef void (*VariableItemSetupCallback)(VariableItem*);
typedef void (*VariableItemChangeCallback)(VariableItem*);

/* ---- harness-side hooks into the shim (not part of the SDK) ---- */
/* Bytes the app wrote to the ESP32 (wifi_marauder_uart_tx -> furi_hal_serial_tx). */
extern FuriString* shim_tx_log;
/* Bytes the app wrote to files opened FSAM_WRITE (kept in memory, never on disk). */
extern FuriString* shim_written;
/* Feed bytes to the registered UART RX interrupt callback; shim_uart_pump runs the RX worker
 * thread body synchronously until it has drained everything that was signalled. */
void shim_uart_rx(const uint8_t* data, size_t len);
void shim_uart_pump(void);
void shim_reset(void);
/* Free every heap block the app allocated and still holds (see alloc_track.h). */
void shim_release_app_allocs(void);
