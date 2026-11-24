/* Implementations of the host-side SDK stand-ins declared in furi.h. Single-threaded: the UART RX
 * worker thread body is run synchronously by shim_uart_pump(), and its thread-flag wait returns the
 * flags signalled since the last wait (or a Stop when nothing is pending, which ends the pump). */
#include "furi.h"

struct FuriString { char* s; size_t len, cap; };
struct File { FILE* f; bool writing; bool open; };
struct FuriStreamBuffer { uint8_t* buf; size_t size, head, len; bool final_free; };
struct FuriThread { FuriThreadCallback cb; void* ctx; bool started; uint32_t flags; bool stopping; };
struct FuriHalSerialHandle { FuriHalSerialAsyncRxCallback cb; void* ctx; uint8_t rx; };

FuriString* shim_tx_log;
FuriString* shim_written;
struct Icon { int unused; };
const Icon I_Text_10x10 = {0};

/* Tracking allocator for the app's translation units (alloc_track.h). Each block carries a header
 * linking it into a list of live app blocks. */
typedef struct ShimBlock { struct ShimBlock *prev, *next; size_t size; size_t pad; } ShimBlock;
static ShimBlock shim_live = {&shim_live, &shim_live, 0, 0};
void* shim_app_malloc(size_t n) {
    ShimBlock* b = calloc(1, sizeof(ShimBlock) + n);
    if(!b) return NULL;
    b->size = n;
    b->next = shim_live.next; b->prev = &shim_live;
    shim_live.next->prev = b; shim_live.next = b;
    return b + 1;
}
void* shim_app_calloc(size_t n, size_t m) {
    if(m && n > (size_t)-1 / m) return NULL;
    return shim_app_malloc(n * m);
}
void shim_app_free(void* p) {
    if(!p) return;
    ShimBlock* b = (ShimBlock*)p - 1;
    b->prev->next = b->next; b->next->prev = b->prev;
    free(b);
}
void* shim_app_realloc(void* p, size_t n) {
    if(!p) return shim_app_malloc(n);
    ShimBlock* old = (ShimBlock*)p - 1;
    void* q = shim_app_malloc(n);
    if(!q) return NULL;
    memcpy(q, p, old->size < n ? old->size : n);
    shim_app_free(p);
    return q;
}
char* shim_app_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = shim_app_malloc(n);
    if(d) memcpy(d, s, n);
    return d;
}
void shim_release_app_allocs(void) {
    while(shim_live.next != &shim_live) shim_app_free(shim_live.next + 1);
}

static void fs_reserve(FuriString* s, size_t need) {
    if(need + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 32;
    while(cap < need + 1) cap *= 2;
    s->s = realloc(s->s, cap);
    furi_check(s->s);
    s->cap = cap;
}
FuriString* furi_string_alloc(void) {
    FuriString* s = calloc(1, sizeof(*s));
    fs_reserve(s, 0);
    s->s[0] = '\0';
    return s;
}
FuriString* furi_string_alloc_set_str(const char* c) { FuriString* s = furi_string_alloc(); furi_string_set(s, c); return s; }
void furi_string_free(FuriString* s) { if(s) { free(s->s); free(s); } }
const char* furi_string_get_cstr(const FuriString* s) { return s->s; }
void furi_string_set(FuriString* s, const char* c) { size_t n = strlen(c); fs_reserve(s, n); memmove(s->s, c, n + 1); s->len = n; }
void furi_string_reset(FuriString* s) { s->len = 0; s->s[0] = '\0'; }
size_t furi_string_size(const FuriString* s) { return s->len; }
void furi_string_right(FuriString* s, size_t start) {
    if(start >= s->len) { furi_string_reset(s); return; }
    memmove(s->s, s->s + start, s->len - start + 1);
    s->len -= start;
}
static void fs_cat_n(FuriString* s, const char* c, size_t n) { fs_reserve(s, s->len + n); memcpy(s->s + s->len, c, n); s->len += n; s->s[s->len] = '\0'; }
void furi_string_cat_str(FuriString* s, const char* c) { fs_cat_n(s, c, strlen(c)); }
int furi_string_cat_printf(FuriString* s, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if(n > 0) { fs_reserve(s, s->len + (size_t)n); vsnprintf(s->s + s->len, (size_t)n + 1, fmt, ap2); s->len += (size_t)n; }
    va_end(ap2);
    return n;
}

/* storage: reads go to the host file system (the fuzz input file); writes are captured in memory */
File* storage_file_alloc(Storage* storage) { (void)storage; return calloc(1, sizeof(File)); }
void storage_file_free(File* f) { if(f && f->f) fclose(f->f); free(f); }
bool storage_file_open(File* f, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    (void)mode;
    if(access == FSAM_WRITE) { f->writing = true; f->open = true; if(!shim_written) shim_written = furi_string_alloc(); furi_string_reset(shim_written); return true; }
    f->f = fopen(path, "rb");
    f->open = f->f != NULL;
    return f->open;
}
bool storage_file_close(File* f) { if(f->f) { fclose(f->f); f->f = NULL; } f->open = false; f->writing = false; return true; }
bool storage_file_is_open(File* f) { return f->open; }
uint64_t storage_file_size(File* f) {
    if(!f->f) return 0;
    long cur = ftell(f->f); fseek(f->f, 0, SEEK_END); long n = ftell(f->f); fseek(f->f, cur, SEEK_SET);
    return n < 0 ? 0 : (uint64_t)n;
}
size_t storage_file_read(File* f, void* buf, size_t n) { return f->f ? fread(buf, 1, n, f->f) : 0; }
size_t storage_file_write(File* f, const void* buf, size_t n) {
    if(!f || !f->open) return 0;
    if(!shim_written) shim_written = furi_string_alloc();
    fs_cat_n(shim_written, buf, n);
    return n;
}
bool storage_file_exists(Storage* storage, const char* path) { (void)storage; (void)path; return false; }
void path_extract_filename_no_ext(const char* path, FuriString* out) {
    const char* b = strrchr(path, '/'); b = b ? b + 1 : path;
    const char* dot = strrchr(b, '.');
    size_t n = dot ? (size_t)(dot - b) : strlen(b);
    furi_string_reset(out); fs_cat_n(out, b, n);
}

void dialog_file_browser_set_basic_options(DialogsFileBrowserOptions* o, const char* ext, const Icon* icon) { o->extension = ext; o->icon = icon; }
bool dialog_file_browser_show(DialogsApp* d, FuriString* out, FuriString* start, const DialogsFileBrowserOptions* o) { (void)d; (void)out; (void)start; (void)o; return false; }
void dialog_message_show_storage_error(DialogsApp* d, const char* msg) { (void)d; (void)msg; }

void furi_delay_ms(uint32_t ms) { (void)ms; }

/* threads: only one thread object matters (the UART RX worker); run synchronously on demand */
static FuriThread* rx_thread;
static FuriThread* current;
FuriThread* furi_thread_alloc(void) { return calloc(1, sizeof(FuriThread)); }
FuriThread* furi_thread_alloc_ex(const char* name, uint32_t stack, FuriThreadCallback cb, void* ctx) {
    (void)name; (void)stack; FuriThread* t = furi_thread_alloc(); t->cb = cb; t->ctx = ctx; return t;
}
void furi_thread_set_name(FuriThread* t, const char* name) { (void)t; (void)name; }
void furi_thread_set_stack_size(FuriThread* t, size_t size) { (void)t; (void)size; }
void furi_thread_set_context(FuriThread* t, void* ctx) { t->ctx = ctx; }
void furi_thread_set_callback(FuriThread* t, FuriThreadCallback cb) { t->cb = cb; }
void furi_thread_start(FuriThread* t) { t->started = true; rx_thread = t; }
bool furi_thread_join(FuriThread* t) {
    if(t->started && t->cb) { FuriThread* prev = current; current = t; t->cb(t->ctx); current = prev; }
    t->started = false;
    return true;
}
void furi_thread_free(FuriThread* t) { if(rx_thread == t) rx_thread = NULL; free(t); }
FuriThreadId furi_thread_get_id(FuriThread* t) { return t; }
#define SHIM_STOP_FLAG 1u /* WorkerEvtStop in wifi_marauder_uart.c */
uint32_t furi_thread_flags_set(FuriThreadId id, uint32_t flags) {
    FuriThread* t = id; t->flags |= flags;
    if(flags & SHIM_STOP_FLAG) t->stopping = true;
    return t->flags;
}
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    (void)options; (void)timeout;
    FuriThread* t = current;
    furi_check(t);
    uint32_t got = t->flags & flags;
    t->flags &= ~got;
    /* Nothing pending: end this synchronous pump (the worker sees Stop and returns). */
    return got ? got : SHIM_STOP_FLAG;
}
FuriStreamBuffer* furi_stream_buffer_alloc(size_t size, size_t trigger) {
    (void)trigger; FuriStreamBuffer* sb = calloc(1, sizeof(*sb)); sb->buf = malloc(size); sb->size = size; return sb;
}
/* The worker frees its stream buffers whenever it returns; only the final return (after a real
 * WorkerEvtStop from wifi_marauder_uart_free) actually releases them. */
void furi_stream_buffer_free(FuriStreamBuffer* sb) {
    if(current && !current->stopping) return;
    free(sb->buf); free(sb);
}
size_t furi_stream_buffer_send(FuriStreamBuffer* sb, const void* data, size_t len, uint32_t timeout) {
    (void)timeout; size_t n = 0; const uint8_t* d = data;
    while(n < len && sb->len < sb->size) { sb->buf[(sb->head + sb->len) % sb->size] = d[n++]; sb->len++; }
    return n;
}
size_t furi_stream_buffer_receive(FuriStreamBuffer* sb, void* data, size_t len, uint32_t timeout) {
    (void)timeout; size_t n = 0; uint8_t* d = data;
    while(n < len && sb->len) { d[n++] = sb->buf[sb->head]; sb->head = (sb->head + 1) % sb->size; sb->len--; }
    return n;
}

static FuriHalSerialHandle serial;
FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId id) { (void)id; memset(&serial, 0, sizeof(serial)); return &serial; }
void furi_hal_serial_control_release(FuriHalSerialHandle* h) { memset(h, 0, sizeof(*h)); }
void furi_hal_serial_init(FuriHalSerialHandle* h, uint32_t baud) { (void)h; (void)baud; }
void furi_hal_serial_deinit(FuriHalSerialHandle* h) { (void)h; }
void furi_hal_serial_tx(FuriHalSerialHandle* h, const uint8_t* data, size_t len) {
    (void)h;
    if(!shim_tx_log) shim_tx_log = furi_string_alloc();
    fs_cat_n(shim_tx_log, (const char*)data, len);
}
void furi_hal_serial_async_rx_start(FuriHalSerialHandle* h, FuriHalSerialAsyncRxCallback cb, void* ctx, bool report_errors) {
    (void)report_errors; h->cb = cb; h->ctx = ctx;
}
uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* h) { return h->rx; }

void shim_uart_rx(const uint8_t* data, size_t len) {
    furi_check(serial.cb);
    for(size_t i = 0; i < len; i++) { serial.rx = data[i]; serial.cb(&serial, FuriHalSerialRxEventData, serial.ctx); }
}
void shim_uart_pump(void) {
    if(!rx_thread || !rx_thread->cb) return;
    FuriThread* prev = current; current = rx_thread;
    rx_thread->cb(rx_thread->ctx);
    current = prev;
}
void shim_reset(void) {
    if(!shim_tx_log) shim_tx_log = furi_string_alloc();
    if(!shim_written) shim_written = furi_string_alloc();
    furi_string_reset(shim_tx_log); furi_string_reset(shim_written);
}

void text_box_reset(TextBox* t) { (void)t; }
void text_box_set_font(TextBox* t, TextBoxFont f) { (void)t; (void)f; }
void text_box_set_focus(TextBox* t, TextBoxFocus f) { (void)t; (void)f; }
void text_box_set_text(TextBox* t, const char* text) { (void)t; (void)text; }
void view_dispatcher_send_custom_event(ViewDispatcher* v, uint32_t event) { (void)v; (void)event; }
void view_dispatcher_switch_to_view(ViewDispatcher* v, uint32_t view) { (void)v; (void)view; }
void scene_manager_set_scene_state(SceneManager* s, uint32_t scene, uint32_t state) { (void)s; (void)scene; (void)state; }
