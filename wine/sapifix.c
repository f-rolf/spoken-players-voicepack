/*
 * sapifix.dll: what World of Warcraft's text to speech needs of SAPI that Wine's sapi.dll lacks.
 *
 * WoW speaks every line as SAPI XML, <bookmark mark='Start'/>...<bookmark mark='End'/> around the
 * text (and the Spoken Players addon puts <pitch middle=".."><rate speed=".."> inside), into a WAV
 * file it opens with ISpStream::BindToFile, and listens to the voice's events. Wine's SpVoice parses
 * SSML only and has no events, and its SpStream has no BindToFile, so under Wine WoW says nothing.
 *
 * Registered as the TreatAs class of SpVoice and of SpStream (DllRegisterServer), this DLL gives:
 *  - a voice that wraps Wine's own SpVoice. Speak turns SAPI XML into SSML that Wine parses into the
 *    same rate, pitch and volume, split at the bookmarks; the events (stream start, bookmarks,
 *    stream end) and every way SAPI notifies of them are its own;
 *  - a whole SpStream, whose BindToFile writes (or reads) a WAV file.
 *
 * Wine's registration of sapi.dll (on every Proton update) keeps a TreatAs key, so this stays.
 * Nothing here is for Windows, whose SAPI does all of it; setup.c installs it, in a Wine prefix only.
 * Log: %LOCALAPPDATA%\SapiFix\sapifix.log.
 */

#define COBJMACROS
#define CONST_VTABLE
#define INITGUID
#include <windows.h>
#include <mmreg.h>
#include <shlwapi.h>
#include <sapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

DEFINE_GUID(CLSID_SapiFixVoice,  0xfe430f78, 0xed03, 0x4645, 0x8f, 0xf9, 0x11, 0xe5, 0x60, 0xbc, 0x9d, 0x08);
DEFINE_GUID(CLSID_SapiFixStream, 0xf8bb12f6, 0x862e, 0x4f04, 0xab, 0x0c, 0xcb, 0xf7, 0x9a, 0xe5, 0x22, 0xe2);
DEFINE_GUID(FORMAT_WaveFormatEx, 0xc31adbae, 0x527f, 0x4ff5, 0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c);

#define SPERR_UNINITIALIZED       0x80045001
#define SPERR_ALREADY_INITIALIZED 0x80045002
#define SPERR_UNSUPPORTED_FORMAT  0x80045003
#define SPERR_STREAM_CLOSED       0x80045038

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const GUID no_format;

/* ISpNotifyCallback is a C++ class, not a COM interface: one virtual method, no reference count. */
struct notify_callback
{
    const struct
    {
        HRESULT (STDMETHODCALLTYPE *NotifyCallback)(struct notify_callback *This, WPARAM wparam, LPARAM lparam);
    } *vtbl;
};

static HINSTANCE instance;

/* ---- log ---------------------------------------------------------------------------------- */

static CRITICAL_SECTION log_cs;
static WCHAR log_path[MAX_PATH];

static void log_init(void)
{
    WCHAR dir[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);

    if (!len || len >= MAX_PATH - 32) len = GetTempPathW(MAX_PATH, dir);
    if (!len || len >= MAX_PATH - 32) return;
    if (dir[len - 1] == '\\') dir[--len] = 0;
    wcscat(dir, L"\\SapiFix");
    CreateDirectoryW(dir, NULL);
    swprintf(log_path, MAX_PATH, L"%ls\\sapifix.log", dir);
}

static void log_line(const char *format, ...)
{
    char line[1200];
    SYSTEMTIME now;
    va_list args;
    HANDLE file;
    int len;

    if (!log_path[0]) return;
    GetLocalTime(&now);
    len = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu %lu] ", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                   GetCurrentProcessId(), GetCurrentThreadId());
    va_start(args, format);
    len += vsnprintf(line + len, sizeof(line) - len - 2, format, args);
    va_end(args);
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';

    EnterCriticalSection(&log_cs);
    file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        DWORD written;

        if (GetFileSizeEx(file, &size) && size.QuadPart > (2 << 20))
        {
            WCHAR old[MAX_PATH];

            CloseHandle(file);
            swprintf(old, MAX_PATH, L"%ls.old", log_path);
            MoveFileExW(log_path, old, MOVEFILE_REPLACE_EXISTING);
            file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        }
        if (file != INVALID_HANDLE_VALUE)
        {
            WriteFile(file, line, len, &written, NULL);
            CloseHandle(file);
        }
    }
    LeaveCriticalSection(&log_cs);
}

/* At most `max` characters of `text` in UTF-8, for the log. */
static const char *utf8(const WCHAR *text, char *buffer, int size, int max)
{
    int len;

    if (!text) return "(null)";
    len = (int)wcslen(text);
    if (len > max) len = max;
    len = WideCharToMultiByte(CP_UTF8, 0, text, len, buffer, size - 4, NULL, NULL);
    if (len < 0) len = 0;
    if ((int)wcslen(text) > max) strcpy(buffer + len, "...");
    else buffer[len] = 0;
    return buffer;
}

static const char *guid_str(const GUID *guid, char *buffer)
{
    if (!guid) return "(null)";
    sprintf(buffer, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid->Data1, guid->Data2, guid->Data3,
            guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5],
            guid->Data4[6], guid->Data4[7]);
    return buffer;
}

static WCHAR *strdupW(const WCHAR *text)
{
    size_t size = (wcslen(text) + 1) * sizeof(WCHAR);
    WCHAR *copy = malloc(size);

    if (copy) memcpy(copy, text, size);
    return copy;
}

static WCHAR *co_strdupW(const WCHAR *text)
{
    size_t size = (wcslen(text) + 1) * sizeof(WCHAR);
    WCHAR *copy = CoTaskMemAlloc(size);

    if (copy) memcpy(copy, text, size);
    return copy;
}

/* ---- SAPI XML ------------------------------------------------------------------------------- */

/* Growing string. */
struct buf
{
    WCHAR *text;
    size_t len, cap;
};

static BOOL buf_add(struct buf *buf, const WCHAR *text, size_t len)
{
    if (buf->len + len + 1 > buf->cap)
    {
        size_t cap = max(buf->cap * 2, buf->len + len + 64);
        WCHAR *grown = realloc(buf->text, cap * sizeof(WCHAR));

        if (!grown) return FALSE;
        buf->text = grown;
        buf->cap = cap;
    }
    memcpy(buf->text + buf->len, text, len * sizeof(WCHAR));
    buf->len += len;
    buf->text[buf->len] = 0;
    return TRUE;
}

static BOOL buf_str(struct buf *buf, const WCHAR *text)
{
    return buf_add(buf, text, wcslen(text));
}

/* `text` inside Wine's SSML. Its parser takes text as it stands up to the next '<' and decodes no
 * entities, so nothing is escaped (an apostrophe would be said as "&apos;"); a '<' of the text,
 * which it would take for a tag, becomes a space. */
static BOOL buf_ssml_text(struct buf *buf, const WCHAR *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        if (!buf_add(buf, text[i] == '<' ? L" " : text + i, 1)) return FALSE;
    return TRUE;
}

enum segment_kind { SEGMENT_TEXT, SEGMENT_BOOKMARK };

/* What a request says, in order: stretches of text (SSML for Wine's parser when any of it has
 * its own rate, pitch or volume, else plain), and the bookmarks between them. */
struct segment
{
    struct segment *next;
    enum segment_kind kind;
    WCHAR *text;    /* the bookmark's name, or the text as given to Wine */
    WCHAR *plain;   /* the text without markup, should Wine refuse the SSML */
    BOOL ssml;
};

static void free_segments(struct segment *segment)
{
    while (segment)
    {
        struct segment *next = segment->next;

        free(segment->text);
        free(segment->plain);
        free(segment);
        segment = next;
    }
}

struct prosody
{
    LONG rate;      /* -10 to 10, SAPI's RateAdj */
    LONG pitch;     /* -10 to 10, SAPI's PitchAdj.MiddleAdj */
    LONG volume;    /* 0 to 100 */
};

static const struct prosody default_prosody = { 0, 0, 100 };

/* Builds the segments of a request. */
struct builder
{
    struct segment *head, **tail;
    struct buf ssml, plain;
    BOOL ssml_needed, has_text;
    struct prosody stack[64];
    int depth;
};

static BOOL add_segment(struct builder *b, enum segment_kind kind, WCHAR *text, WCHAR *plain, BOOL ssml)
{
    struct segment *segment = calloc(1, sizeof(*segment));

    if (!segment)
    {
        free(text);
        free(plain);
        return FALSE;
    }
    segment->kind = kind;
    segment->text = text;
    segment->plain = plain;
    segment->ssml = ssml;
    *b->tail = segment;
    b->tail = &segment->next;
    return TRUE;
}

/* The text gathered since the last bookmark becomes a segment (none when it is only spaces). */
static BOOL flush_text(struct builder *b)
{
    BOOL ok = TRUE;

    if (b->has_text)
    {
        if (b->ssml_needed)
        {
            buf_str(&b->ssml, L"</speak>");
            ok = add_segment(b, SEGMENT_TEXT, b->ssml.text, b->plain.text, TRUE);
            b->ssml.text = NULL;
        }
        else
        {
            ok = add_segment(b, SEGMENT_TEXT, b->plain.text, NULL, FALSE);
        }
        b->plain.text = NULL;
    }
    free(b->ssml.text);
    free(b->plain.text);
    memset(&b->ssml, 0, sizeof(b->ssml));
    memset(&b->plain, 0, sizeof(b->plain));
    b->ssml_needed = b->has_text = FALSE;
    return ok;
}

/* A run of decoded text, said with the prosody on top of the stack. Wine's SSML parser sets
 * RateAdj from the rate multiplier as round(ln(rate) * 10 / ln 3), and moves MiddleAdj from the
 * <speak> element's by round(log2(1 + pitch / 100) * 24): so 3^(r/10) and (2^(p/24) - 1) * 100%
 * come out as exactly r and p. A volume without a sign is taken as it is. */
static BOOL add_text(struct builder *b, const WCHAR *text, size_t len)
{
    const struct prosody *p = &b->stack[b->depth];
    BOOL shaped = memcmp(p, &default_prosody, sizeof(*p)) != 0;
    WCHAR attrs[160];
    size_t i;

    if (!len) return TRUE;
    for (i = 0; i < len; i++) if (!iswspace(text[i])) b->has_text = TRUE;

    if (!b->ssml.len)
        buf_str(&b->ssml, L"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\">");
    if (shaped)
    {
        b->ssml_needed = TRUE;
        swprintf(attrs, ARRAY_SIZE(attrs), L"<prosody rate=\"%.6f\" pitch=\"%.6f%%\" volume=\"%ld\">",
                 pow(3.0, p->rate / 10.0), (pow(2.0, p->pitch / 24.0) - 1.0) * 100.0, p->volume);
        buf_str(&b->ssml, attrs);
    }
    buf_ssml_text(&b->ssml, text, len);
    if (shaped) buf_str(&b->ssml, L"</prosody>");
    return buf_add(&b->plain, text, len);
}

static LONG clamp(LONG value, LONG low, LONG high)
{
    return value < low ? low : value > high ? high : value;
}

/* The value of attribute `name` among `attrs` (the inside of a tag, after its name), decoded
 * into `out`; FALSE when it has none. */
static BOOL get_attr(const WCHAR *attrs, size_t len, const WCHAR *name, WCHAR *out, size_t size)
{
    size_t name_len = wcslen(name), i = 0;

    while (i < len)
    {
        size_t start, value_start;
        WCHAR quote;

        while (i < len && (iswspace(attrs[i]) || attrs[i] == '/')) i++;
        start = i;
        while (i < len && !iswspace(attrs[i]) && attrs[i] != '=' && attrs[i] != '/') i++;
        if (i == start) break;
        BOOL match = i - start == name_len && !_wcsnicmp(attrs + start, name, name_len);
        while (i < len && iswspace(attrs[i])) i++;
        if (i >= len || attrs[i] != '=') continue;
        i++;
        while (i < len && iswspace(attrs[i])) i++;
        if (i >= len) break;
        quote = attrs[i];
        if (quote == '"' || quote == '\'') i++;
        else quote = 0;
        value_start = i;
        while (i < len && (quote ? attrs[i] != quote : !iswspace(attrs[i]))) i++;
        if (match)
        {
            size_t value_len = min(i - value_start, size - 1);

            memcpy(out, attrs + value_start, value_len * sizeof(WCHAR));
            out[value_len] = 0;
            return TRUE;
        }
        if (i < len && quote) i++;
    }
    return FALSE;
}

static LONG attr_number(const WCHAR *attrs, size_t len, const WCHAR *name, LONG fallback, BOOL *found)
{
    WCHAR value[32];

    *found = get_attr(attrs, len, name, value, ARRAY_SIZE(value));
    return *found ? wcstol(value, NULL, 10) : fallback;
}

/* Decodes the entities in a run of text: &lt; &gt; &amp; &quot; &apos; &#N; &#xN;. */
static BOOL add_decoded(struct builder *b, const WCHAR *text, size_t len)
{
    struct buf out = {0};
    size_t i = 0;
    BOOL ok;

    while (i < len)
    {
        if (text[i] == '&')
        {
            const WCHAR *semi = wmemchr(text + i, ';', min(len - i, 12));
            WCHAR c = 0;

            if (semi)
            {
                size_t n = semi - (text + i);

                if (n == 3 && !wcsncmp(text + i, L"&lt", 3)) c = '<';
                else if (n == 3 && !wcsncmp(text + i, L"&gt", 3)) c = '>';
                else if (n == 4 && !wcsncmp(text + i, L"&amp", 4)) c = '&';
                else if (n == 5 && !wcsncmp(text + i, L"&quot", 5)) c = '"';
                else if (n == 5 && !wcsncmp(text + i, L"&apos", 5)) c = '\'';
                else if (n > 2 && text[i + 1] == '#')
                    c = (WCHAR)(text[i + 2] == 'x' || text[i + 2] == 'X' ? wcstol(text + i + 3, NULL, 16)
                                                                          : wcstol(text + i + 2, NULL, 10));
                if (c)
                {
                    buf_add(&out, &c, 1);
                    i += n + 1;
                    continue;
                }
            }
        }
        buf_add(&out, text + i, 1);
        i++;
    }
    ok = add_text(b, out.text ? out.text : L"", out.len);
    free(out.text);
    return ok;
}

/* Whether a tag's name (len characters at `name`) is `expected`. */
static BOOL tag_is(const WCHAR *name, size_t len, const WCHAR *expected)
{
    return len == wcslen(expected) && !_wcsnicmp(name, expected, len);
}

/* SAPI's XML: <bookmark mark/>, <rate speed|absspeed>, <pitch middle|absmiddle>,
 * <volume level>, and the rest (<emph>, <spell>, <silence>, <voice>, <lang>, <context>, <pron>,
 * <partofsp>, <sapi>) as plain containers. */
static HRESULT parse_sapi(const WCHAR *text, struct segment **out)
{
    struct builder b;
    const WCHAR *p = text;

    memset(&b, 0, sizeof(b));
    b.tail = &b.head;
    b.stack[0] = default_prosody;

    while (*p)
    {
        const WCHAR *lt = wcschr(p, '<');

        if (!lt)
        {
            add_decoded(&b, p, wcslen(p));
            break;
        }
        add_decoded(&b, p, lt - p);

        if (!wcsncmp(lt, L"<!--", 4))
        {
            const WCHAR *end = wcsstr(lt + 4, L"-->");
            p = end ? end + 3 : lt + wcslen(lt);
            continue;
        }
        if (lt[1] == '?' || lt[1] == '!')
        {
            const WCHAR *end = wcschr(lt, '>');
            p = end ? end + 1 : lt + wcslen(lt);
            continue;
        }

        {
            const WCHAR *gt = wcschr(lt, '>'), *name = lt + 1, *attrs;
            BOOL closing = FALSE, empty, found, found2;
            size_t name_len, attrs_len;

            if (!gt)
            {
                /* A lone '<': text. */
                add_decoded(&b, lt, 1);
                p = lt + 1;
                continue;
            }
            if (*name == '/')
            {
                closing = TRUE;
                name++;
            }
            for (name_len = 0; name + name_len < gt && !iswspace(name[name_len]) && name[name_len] != '/'; name_len++) ;
            attrs = name + name_len;
            attrs_len = gt - attrs;
            empty = gt > lt && gt[-1] == '/';
            p = gt + 1;

            if (closing)
            {
                if (b.depth > 0) b.depth--;
                continue;
            }

            if (tag_is(name, name_len, L"bookmark"))
            {
                WCHAR mark[256];

                if (!get_attr(attrs, attrs_len, L"mark", mark, ARRAY_SIZE(mark))) mark[0] = 0;
                flush_text(&b);
                add_segment(&b, SEGMENT_BOOKMARK, strdupW(mark), NULL, FALSE);
                /* Written <bookmark ...></bookmark>, its closing tag must not end the element around it. */
                if (!empty && b.depth + 1 < (int)ARRAY_SIZE(b.stack))
                {
                    b.stack[b.depth + 1] = b.stack[b.depth];
                    b.depth++;
                }
                continue;
            }
            if (empty) continue;  /* <silence/>, <pron sym/> and the like: nothing to say */

            if (b.depth + 1 < (int)ARRAY_SIZE(b.stack))
            {
                struct prosody next = b.stack[b.depth];
                LONG value;

                if (tag_is(name, name_len, L"rate"))
                {
                    value = attr_number(attrs, attrs_len, L"absspeed", 0, &found);
                    if (found) next.rate = value;
                    value = attr_number(attrs, attrs_len, L"speed", 0, &found2);
                    if (found2) next.rate += value;
                    next.rate = clamp(next.rate, -10, 10);
                }
                else if (tag_is(name, name_len, L"pitch"))
                {
                    value = attr_number(attrs, attrs_len, L"absmiddle", 0, &found);
                    if (found) next.pitch = value;
                    value = attr_number(attrs, attrs_len, L"middle", 0, &found2);
                    if (found2) next.pitch += value;
                    next.pitch = clamp(next.pitch, -10, 10);
                }
                else if (tag_is(name, name_len, L"volume"))
                {
                    value = attr_number(attrs, attrs_len, L"level", 100, &found);
                    if (found) next.volume = clamp(value, 0, 100);
                }
                b.stack[++b.depth] = next;
            }
        }
    }
    flush_text(&b);
    *out = b.head;
    return S_OK;
}

/* Whether the XML's first element is SSML's <speak>. */
static BOOL is_ssml(const WCHAR *text)
{
    const WCHAR *p = text;

    for (;;)
    {
        while (iswspace(*p)) p++;
        if (!wcsncmp(p, L"<?", 2) || !wcsncmp(p, L"<!", 2))
        {
            const WCHAR *end = wcschr(p, '>');
            if (!end) return FALSE;
            p = end + 1;
            continue;
        }
        return !_wcsnicmp(p, L"<speak", 6) && (iswspace(p[6]) || p[6] == '>' || p[6] == '/');
    }
}

/* ---- the voice ---------------------------------------------------------------------------- */

enum notify_kind { NOTIFY_NONE, NOTIFY_SINK, NOTIFY_WINDOW, NOTIFY_RELAY, NOTIFY_EVENT };

/* A callback (function or ISpNotifyCallback) is called on the thread that asked for it, as SAPI
 * does, through a message-only window made on that thread. The window owns this. */
struct relay
{
    SPNOTIFYCALLBACK *function;
    struct notify_callback *callback;
    WPARAM wparam;
    LPARAM lparam;
};

#define WM_RELAY_NOTIFY (WM_APP + 1)

struct request
{
    struct request *next;
    ULONG stream;
    LONG generation;
    DWORD nlp_flags;
    struct segment *segments;
    HANDLE done;        /* a synchronous Speak's, which frees the request */
    HRESULT hr;
};

struct event_entry
{
    SPEVENT event;
    WCHAR *text;        /* a bookmark's name */
};

struct voice
{
    ISpVoice ISpVoice_iface;
    LONG ref;
    ISpVoice *inner;            /* Wine's SpVoice */
    IUnknown *output;           /* what SetOutput was given, for the events' audio offsets */
    CRITICAL_SECTION cs;

    ULONGLONG interest, queued_interest;
    struct event_entry *events;
    ULONG event_count, event_cap;

    enum notify_kind notify;
    ISpNotifySink *sink;
    HWND window, relay_window;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    HANDLE notify_event;

    struct request *head, *tail;
    HANDLE thread, wake, idle;  /* idle: manual reset, set while nothing is queued or said */
    DWORD thread_id;
    BOOL quit, free_on_exit;
    LONG generation;            /* moves on at every purge */

    ULONG stream_count, current_stream, last_stream;
    HRESULT last_hr;
    BOOL speaking;
    WCHAR *last_bookmark;
    LONG last_bookmark_id;

    ULONG sync_timeout;
    SPVPRIORITY priority;
    SPEVENTENUM alert_boundary;
};

static inline struct voice *impl_from_ISpVoice(ISpVoice *iface)
{
    return CONTAINING_RECORD(iface, struct voice, ISpVoice_iface);
}

static LRESULT CALLBACK relay_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct relay *relay = (struct relay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_RELAY_NOTIFY:
        if (relay && relay->function) relay->function(relay->wparam, relay->lparam);
        else if (relay && relay->callback) relay->callback->vtbl->NotifyCallback(relay->callback, relay->wparam, relay->lparam);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (relay)
        {
            free(relay);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND create_relay(struct relay *relay)
{
    static LONG registered;
    HWND hwnd;

    if (!InterlockedExchange(&registered, 1))
    {
        WNDCLASSW cls = {0};

        cls.lpfnWndProc = relay_proc;
        cls.hInstance = instance;
        cls.lpszClassName = L"SapiFixRelay";
        RegisterClassW(&cls);
    }
    hwnd = CreateWindowExW(0, L"SapiFixRelay", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, instance, NULL);
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)relay);
    return hwnd;
}

/* Drops the notification set up before (call with cs held). */
static void clear_notify(struct voice *This)
{
    if (This->sink) ISpNotifySink_Release(This->sink);
    if (This->relay_window) PostMessageW(This->relay_window, WM_CLOSE, 0, 0);
    if (This->notify_event) CloseHandle(This->notify_event);
    This->sink = NULL;
    This->relay_window = NULL;
    This->notify_event = NULL;
    This->window = NULL;
    This->notify = NOTIFY_NONE;
}

static void send_notify(struct voice *This)
{
    ISpNotifySink *sink = NULL;

    EnterCriticalSection(&This->cs);
    switch (This->notify)
    {
    case NOTIFY_SINK:
        sink = This->sink;
        ISpNotifySink_AddRef(sink);
        break;
    case NOTIFY_WINDOW:
        PostMessageW(This->window, This->message, This->wparam, This->lparam);
        break;
    case NOTIFY_RELAY:
        PostMessageW(This->relay_window, WM_RELAY_NOTIFY, 0, 0);
        break;
    case NOTIFY_EVENT:
        SetEvent(This->notify_event);
        break;
    case NOTIFY_NONE:
        break;
    }
    LeaveCriticalSection(&This->cs);

    if (sink)
    {
        ISpNotifySink_Notify(sink);
        ISpNotifySink_Release(sink);
    }
}

/* Where the output stream is now, for an event's ullAudioStreamOffset (0 for the speakers). */
static ULONGLONG audio_offset(struct voice *This)
{
    ULARGE_INTEGER pos = {{0}};
    LARGE_INTEGER zero = {{0}};
    IStream *stream;

    EnterCriticalSection(&This->cs);
    if (This->output && SUCCEEDED(IUnknown_QueryInterface(This->output, &IID_IStream, (void **)&stream)))
    {
        if (FAILED(IStream_Seek(stream, zero, STREAM_SEEK_CUR, &pos))) pos.QuadPart = 0;
        IStream_Release(stream);
    }
    LeaveCriticalSection(&This->cs);
    return pos.QuadPart;
}

static void add_event(struct voice *This, SPEVENTENUM id, ULONG stream, WPARAM wparam, const WCHAR *text)
{
    ULONGLONG bit = 1ull << id;
    BOOL notify;

    EnterCriticalSection(&This->cs);
    notify = (This->interest & bit) != 0;
    if (This->queued_interest & bit)
    {
        struct event_entry *entry;

        if (This->event_count >= 1024)
        {
            free(This->events[0].text);
            memmove(This->events, This->events + 1, --This->event_count * sizeof(*This->events));
        }
        if (This->event_count == This->event_cap)
        {
            ULONG cap = This->event_cap ? This->event_cap * 2 : 16;
            struct event_entry *grown = realloc(This->events, cap * sizeof(*grown));

            if (!grown)
            {
                LeaveCriticalSection(&This->cs);
                return;
            }
            This->events = grown;
            This->event_cap = cap;
        }
        entry = &This->events[This->event_count++];
        memset(entry, 0, sizeof(*entry));
        entry->event.eEventId = id;
        entry->event.elParamType = text ? SPET_LPARAM_IS_STRING : SPET_LPARAM_IS_UNDEFINED;
        entry->event.ulStreamNum = stream;
        entry->event.wParam = wparam;
        entry->text = text ? strdupW(text) : NULL;
    }
    LeaveCriticalSection(&This->cs);

    if (notify)
    {
        /* The offset is looked up outside the lock: the output may be in the middle of a write. */
        ULONGLONG offset = audio_offset(This);

        EnterCriticalSection(&This->cs);
        if (This->event_count && This->events[This->event_count - 1].event.ulStreamNum == stream &&
            This->events[This->event_count - 1].event.eEventId == id)
            This->events[This->event_count - 1].event.ullAudioStreamOffset = offset;
        LeaveCriticalSection(&This->cs);
        send_notify(This);
    }
}

static void finish_request(struct request *request, HRESULT hr)
{
    request->hr = hr;
    if (request->done) SetEvent(request->done);
    else
    {
        free_segments(request->segments);
        free(request);
    }
}

/* Says one request on the worker thread: its text through Wine's voice, its events itself. */
static void run_request(struct voice *This, struct request *request)
{
    struct segment *segment;
    char text[256];
    HRESULT hr = S_OK;

    add_event(This, SPEI_START_INPUT_STREAM, request->stream, 0, NULL);

    for (segment = request->segments; segment; segment = segment->next)
    {
        if (This->generation != request->generation) break;

        if (segment->kind == SEGMENT_BOOKMARK)
        {
            LONG id = wcstol(segment->text, NULL, 10);

            EnterCriticalSection(&This->cs);
            free(This->last_bookmark);
            This->last_bookmark = strdupW(segment->text);
            This->last_bookmark_id = id;
            LeaveCriticalSection(&This->cs);
            add_event(This, SPEI_TTS_BOOKMARK, request->stream, id, segment->text);
            continue;
        }

        hr = ISpVoice_Speak(This->inner, segment->text,
                            SPF_ASYNC | (segment->ssml ? SPF_IS_XML : SPF_IS_NOT_XML) | request->nlp_flags, NULL);
        if (FAILED(hr) && segment->ssml)
        {
            log_line("Wine refused the SSML (%#lx), saying it plain: %s", hr,
                     utf8(segment->text, text, sizeof(text), 160));
            hr = ISpVoice_Speak(This->inner, segment->plain, SPF_ASYNC | SPF_IS_NOT_XML | request->nlp_flags, NULL);
        }
        if (SUCCEEDED(hr))
            ISpVoice_WaitUntilDone(This->inner, INFINITE);
        else
            log_line("stream %lu: Wine's Speak failed: %#lx", request->stream, hr);
    }

    add_event(This, SPEI_END_INPUT_STREAM, request->stream, 0, NULL);
    request->hr = hr;
}

static void free_voice(struct voice *This);

static DWORD WINAPI worker_proc(void *arg)
{
    struct voice *This = arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;)
    {
        struct request *request;

        EnterCriticalSection(&This->cs);
        if (This->quit)
        {
            LeaveCriticalSection(&This->cs);
            break;
        }
        if (!(request = This->head))
        {
            SetEvent(This->idle);
            LeaveCriticalSection(&This->cs);
            WaitForSingleObject(This->wake, INFINITE);
            continue;
        }
        if (!(This->head = request->next)) This->tail = NULL;
        if (request->generation != This->generation)
        {
            LeaveCriticalSection(&This->cs);
            finish_request(request, S_OK);
            continue;
        }
        This->speaking = TRUE;
        This->current_stream = request->stream;
        LeaveCriticalSection(&This->cs);

        run_request(This, request);

        EnterCriticalSection(&This->cs);
        This->speaking = FALSE;
        This->last_hr = request->hr;
        LeaveCriticalSection(&This->cs);
        finish_request(request, request->hr);
    }
    CoUninitialize();
    if (This->free_on_exit) free_voice(This);
    return 0;
}

/* Drops what is queued and stops what Wine is saying. */
static void purge(struct voice *This)
{
    struct request *request;

    EnterCriticalSection(&This->cs);
    This->generation++;
    request = This->head;
    This->head = This->tail = NULL;
    LeaveCriticalSection(&This->cs);

    while (request)
    {
        struct request *next = request->next;
        finish_request(request, S_OK);
        request = next;
    }
    ISpVoice_Speak(This->inner, NULL, SPF_PURGEBEFORESPEAK, NULL);
}

static HRESULT WINAPI voice_QueryInterface(ISpVoice *iface, REFIID iid, void **out)
{
    struct voice *This = impl_from_ISpVoice(iface);
    char guid[40];

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ISpVoice) ||
        IsEqualIID(iid, &IID_ISpEventSource) || IsEqualIID(iid, &IID_ISpNotifySource))
    {
        *out = iface;
        ISpVoice_AddRef(iface);
        return S_OK;
    }
    /* Automation (SAPI.SpVoice from scripts) is Wine's, without this DLL's additions. */
    if (IsEqualIID(iid, &IID_ISpeechVoice) || IsEqualIID(iid, &IID_IDispatch))
        return ISpVoice_QueryInterface(This->inner, iid, out);

    log_line("voice: no interface %s", guid_str(iid, guid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI voice_AddRef(ISpVoice *iface)
{
    return InterlockedIncrement(&impl_from_ISpVoice(iface)->ref);
}

static void free_voice(struct voice *This)
{
    ULONG i;

    if (This->thread) CloseHandle(This->thread);
    if (This->wake) CloseHandle(This->wake);
    if (This->idle) CloseHandle(This->idle);
    clear_notify(This);
    for (i = 0; i < This->event_count; i++) free(This->events[i].text);
    free(This->events);
    free(This->last_bookmark);
    if (This->output) IUnknown_Release(This->output);
    if (This->inner) ISpVoice_Release(This->inner);
    DeleteCriticalSection(&This->cs);
    free(This);
}

static ULONG WINAPI voice_Release(ISpVoice *iface)
{
    struct voice *This = impl_from_ISpVoice(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    if (ref) return ref;

    if (This->thread)
    {
        purge(This);
        EnterCriticalSection(&This->cs);
        This->quit = TRUE;
        LeaveCriticalSection(&This->cs);
        SetEvent(This->wake);
        if (GetCurrentThreadId() == This->thread_id)
        {
            /* Released by a notification on the worker thread: it frees the voice as it ends. */
            This->free_on_exit = TRUE;
            return 0;
        }
        WaitForSingleObject(This->thread, INFINITE);
    }
    free_voice(This);
    return 0;
}

static HRESULT WINAPI voice_SetNotifySink(ISpVoice *iface, ISpNotifySink *sink)
{
    struct voice *This = impl_from_ISpVoice(iface);

    EnterCriticalSection(&This->cs);
    clear_notify(This);
    if (sink)
    {
        ISpNotifySink_AddRef(sink);
        This->sink = sink;
        This->notify = NOTIFY_SINK;
    }
    LeaveCriticalSection(&This->cs);
    log_line("voice %p: notify by sink %p", This, sink);
    return S_OK;
}

static HRESULT WINAPI voice_SetNotifyWindowMessage(ISpVoice *iface, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct voice *This = impl_from_ISpVoice(iface);

    if (!IsWindow(hwnd)) return E_INVALIDARG;
    EnterCriticalSection(&This->cs);
    clear_notify(This);
    This->window = hwnd;
    This->message = msg;
    This->wparam = wparam;
    This->lparam = lparam;
    This->notify = NOTIFY_WINDOW;
    LeaveCriticalSection(&This->cs);
    log_line("voice %p: notify by window message %#x", This, msg);
    return S_OK;
}

static HRESULT set_relay(struct voice *This, SPNOTIFYCALLBACK *function, struct notify_callback *callback,
                         WPARAM wparam, LPARAM lparam)
{
    struct relay *relay = calloc(1, sizeof(*relay));
    HWND hwnd;

    if (!relay) return E_OUTOFMEMORY;
    relay->function = function;
    relay->callback = callback;
    relay->wparam = wparam;
    relay->lparam = lparam;
    if (!(hwnd = create_relay(relay)))
    {
        free(relay);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    EnterCriticalSection(&This->cs);
    clear_notify(This);
    This->relay_window = hwnd;
    This->notify = NOTIFY_RELAY;
    LeaveCriticalSection(&This->cs);
    return S_OK;
}

static HRESULT WINAPI voice_SetNotifyCallbackFunction(ISpVoice *iface, SPNOTIFYCALLBACK *function, WPARAM wparam, LPARAM lparam)
{
    struct voice *This = impl_from_ISpVoice(iface);

    log_line("voice %p: notify by callback function", This);
    if (!function) return E_INVALIDARG;
    return set_relay(This, function, NULL, wparam, lparam);
}

static HRESULT WINAPI voice_SetNotifyCallbackInterface(ISpVoice *iface, ISpNotifyCallback *callback, WPARAM wparam, LPARAM lparam)
{
    struct voice *This = impl_from_ISpVoice(iface);

    log_line("voice %p: notify by callback interface", This);
    if (!callback) return E_INVALIDARG;
    return set_relay(This, NULL, (struct notify_callback *)callback, wparam, lparam);
}

static HRESULT WINAPI voice_SetNotifyWin32Event(ISpVoice *iface)
{
    struct voice *This = impl_from_ISpVoice(iface);
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (!event) return HRESULT_FROM_WIN32(GetLastError());
    EnterCriticalSection(&This->cs);
    clear_notify(This);
    This->notify_event = event;
    This->notify = NOTIFY_EVENT;
    LeaveCriticalSection(&This->cs);
    log_line("voice %p: notify by Win32 event", This);
    return S_OK;
}

static HRESULT WINAPI voice_WaitForNotifyEvent(ISpVoice *iface, DWORD ms)
{
    struct voice *This = impl_from_ISpVoice(iface);
    HANDLE event;

    EnterCriticalSection(&This->cs);
    event = This->notify == NOTIFY_EVENT ? This->notify_event : NULL;
    LeaveCriticalSection(&This->cs);
    if (!event) return SPERR_UNINITIALIZED;
    return WaitForSingleObject(event, ms) == WAIT_OBJECT_0 ? S_OK : S_FALSE;
}

static HANDLE WINAPI voice_GetNotifyEventHandle(ISpVoice *iface)
{
    struct voice *This = impl_from_ISpVoice(iface);
    HANDLE event;

    EnterCriticalSection(&This->cs);
    event = This->notify == NOTIFY_EVENT ? This->notify_event : NULL;
    LeaveCriticalSection(&This->cs);
    return event;
}

static HRESULT WINAPI voice_SetInterest(ISpVoice *iface, ULONGLONG interest, ULONGLONG queued)
{
    struct voice *This = impl_from_ISpVoice(iface);

    if (queued & ~interest) return E_INVALIDARG;
    EnterCriticalSection(&This->cs);
    This->interest = interest;
    This->queued_interest = queued;
    LeaveCriticalSection(&This->cs);
    log_line("voice %p: interest %#llx, queued %#llx", This, interest, queued);
    return S_OK;
}

static HRESULT WINAPI voice_GetEvents(ISpVoice *iface, ULONG count, SPEVENT *events, ULONG *fetched)
{
    struct voice *This = impl_from_ISpVoice(iface);
    ULONG i, n;

    if (!events || (count > 1 && !fetched)) return E_POINTER;

    EnterCriticalSection(&This->cs);
    n = min(count, This->event_count);
    for (i = 0; i < n; i++)
    {
        events[i] = This->events[i].event;
        if (This->events[i].text)
        {
            events[i].lParam = (LPARAM)co_strdupW(This->events[i].text);
            free(This->events[i].text);
        }
    }
    This->event_count -= n;
    memmove(This->events, This->events + n, This->event_count * sizeof(*This->events));
    LeaveCriticalSection(&This->cs);

    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI voice_GetInfo(ISpVoice *iface, SPEVENTSOURCEINFO *info)
{
    struct voice *This = impl_from_ISpVoice(iface);

    if (!info) return E_POINTER;
    EnterCriticalSection(&This->cs);
    info->ullEventInterest = This->interest;
    info->ullQueuedInterest = This->queued_interest;
    info->ulCount = This->event_count;
    LeaveCriticalSection(&This->cs);
    return S_OK;
}

static BOOL is_own_stream(IUnknown *unk);

static HRESULT WINAPI voice_SetOutput(ISpVoice *iface, IUnknown *unk, WINBOOL allow_format_changes)
{
    struct voice *This = impl_from_ISpVoice(iface);
    BOOL own = is_own_stream(unk);
    HRESULT hr;

    /* This DLL's stream keeps its file's format whatever it is told: it converts (see struct stream). */
    hr = ISpVoice_SetOutput(This->inner, unk, own || allow_format_changes);
    log_line("voice %p: SetOutput(%p%s, %d) = %#lx", This, unk, own ? ", a SapiFix stream" : "", allow_format_changes, hr);
    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&This->cs);
        if (This->output) IUnknown_Release(This->output);
        if ((This->output = unk)) IUnknown_AddRef(unk);
        LeaveCriticalSection(&This->cs);
    }
    return hr;
}

static HRESULT WINAPI voice_GetOutputObjectToken(ISpVoice *iface, ISpObjectToken **token)
{
    return ISpVoice_GetOutputObjectToken(impl_from_ISpVoice(iface)->inner, token);
}

static HRESULT WINAPI voice_GetOutputStream(ISpVoice *iface, ISpStreamFormat **stream)
{
    struct voice *This = impl_from_ISpVoice(iface);
    HRESULT hr;

    if (!stream) return E_POINTER;
    EnterCriticalSection(&This->cs);
    hr = This->output ? IUnknown_QueryInterface(This->output, &IID_ISpStreamFormat, (void **)stream) : E_NOTIMPL;
    LeaveCriticalSection(&This->cs);
    if (hr == E_NOTIMPL) hr = ISpVoice_GetOutputStream(This->inner, stream);
    return hr;
}

static HRESULT WINAPI voice_Pause(ISpVoice *iface)
{
    return ISpVoice_Pause(impl_from_ISpVoice(iface)->inner);
}

static HRESULT WINAPI voice_Resume(ISpVoice *iface)
{
    return ISpVoice_Resume(impl_from_ISpVoice(iface)->inner);
}

static HRESULT WINAPI voice_SetVoice(ISpVoice *iface, ISpObjectToken *token)
{
    struct voice *This = impl_from_ISpVoice(iface);
    HRESULT hr = ISpVoice_SetVoice(This->inner, token);
    WCHAR *id = NULL;
    char text[200];

    if (token) ISpObjectToken_GetId(token, &id);
    log_line("voice %p: SetVoice(%s) = %#lx", This, id ? utf8(id, text, sizeof(text), 150) : "default", hr);
    CoTaskMemFree(id);
    return hr;
}

static HRESULT WINAPI voice_GetVoice(ISpVoice *iface, ISpObjectToken **token)
{
    return ISpVoice_GetVoice(impl_from_ISpVoice(iface)->inner, token);
}

static HRESULT WINAPI voice_Speak(ISpVoice *iface, const WCHAR *contents, DWORD flags, ULONG *stream_out)
{
    static const DWORD known = SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_XML | SPF_IS_NOT_XML |
                               SPF_PERSIST_XML | SPF_PARSE_MASK | SPF_NLP_MASK;
    struct voice *This = impl_from_ISpVoice(iface);
    struct request *request;
    struct segment *segments = NULL;
    char text[400];
    BOOL xml;
    HRESULT hr;

    log_line("voice %p: Speak(flags %#lx) %s", This, flags, utf8(contents, text, sizeof(text), 300));

    if (flags & ~known)
    {
        hr = ISpVoice_Speak(This->inner, contents, flags, stream_out);
        log_line("voice %p: flags this DLL does not know, left to Wine: %#lx", This, hr);
        return hr;
    }
    if ((flags & SPF_IS_XML) && (flags & SPF_IS_NOT_XML)) return E_INVALIDARG;
    if ((flags & SPF_PARSE_MASK) == SPF_PARSE_MASK) return E_INVALIDARG;

    if (flags & SPF_PURGEBEFORESPEAK)
    {
        if (This->thread) purge(This);
        if (!contents || !*contents) return S_OK;
    }
    else if (!contents) return E_POINTER;

    if (flags & SPF_IS_XML) xml = TRUE;
    else if (flags & SPF_IS_NOT_XML) xml = FALSE;
    else
    {
        const WCHAR *p = contents;
        while (iswspace(*p)) p++;
        xml = *p == '<';
    }

    if (!xml)
    {
        if (*contents && !(segments = calloc(1, sizeof(*segments)))) return E_OUTOFMEMORY;
        if (segments)
        {
            segments->kind = SEGMENT_TEXT;
            segments->text = strdupW(contents);
        }
    }
    else if ((flags & SPF_PARSE_MASK) == SPF_PARSE_SSML || ((flags & SPF_PARSE_MASK) != SPF_PARSE_SAPI && is_ssml(contents)))
    {
        /* SSML is Wine's own to parse. */
        if (!(segments = calloc(1, sizeof(*segments)))) return E_OUTOFMEMORY;
        segments->kind = SEGMENT_TEXT;
        segments->text = strdupW(contents);
        segments->ssml = TRUE;
    }
    else if (FAILED(hr = parse_sapi(contents, &segments)))
        return hr;

    if (!(request = calloc(1, sizeof(*request))))
    {
        free_segments(segments);
        return E_OUTOFMEMORY;
    }
    request->segments = segments;
    request->nlp_flags = flags & SPF_NLP_MASK;
    if (!(flags & SPF_ASYNC)) request->done = CreateEventW(NULL, TRUE, FALSE, NULL);

    EnterCriticalSection(&This->cs);
    if (!This->thread)
    {
        This->thread = CreateThread(NULL, 0, worker_proc, This, 0, &This->thread_id);
        if (!This->thread)
        {
            LeaveCriticalSection(&This->cs);
            if (request->done) CloseHandle(request->done);
            free_segments(segments);
            free(request);
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    request->stream = ++This->stream_count;
    request->generation = This->generation;
    This->last_stream = request->stream;
    if (This->tail) This->tail->next = request;
    else This->head = request;
    This->tail = request;
    ResetEvent(This->idle);
    LeaveCriticalSection(&This->cs);
    SetEvent(This->wake);

    if (stream_out) *stream_out = request->stream;
    if (flags & SPF_ASYNC) return S_OK;

    WaitForSingleObject(request->done, INFINITE);
    hr = request->hr;
    CloseHandle(request->done);
    free_segments(request->segments);
    free(request);
    return hr;
}

static HRESULT WINAPI voice_SpeakStream(ISpVoice *iface, IStream *stream, DWORD flags, ULONG *number)
{
    log_line("voice: SpeakStream is not done, left to Wine");
    return ISpVoice_SpeakStream(impl_from_ISpVoice(iface)->inner, stream, flags, number);
}

static HRESULT WINAPI voice_GetStatus(ISpVoice *iface, SPVOICESTATUS *status, LPWSTR *bookmark)
{
    struct voice *This = impl_from_ISpVoice(iface);

    EnterCriticalSection(&This->cs);
    if (status)
    {
        memset(status, 0, sizeof(*status));
        status->ulCurrentStream = This->current_stream;
        status->ulLastStreamQueued = This->last_stream;
        status->hrLastResult = This->last_hr;
        status->dwRunningState = This->speaking ? SPRS_IS_SPEAKING : This->head ? 0 : SPRS_DONE;
        status->lBookmarkId = This->last_bookmark_id;
    }
    if (bookmark) *bookmark = This->last_bookmark ? co_strdupW(This->last_bookmark) : NULL;
    LeaveCriticalSection(&This->cs);
    return S_OK;
}

static HRESULT WINAPI voice_Skip(ISpVoice *iface, LPCWSTR type, LONG items, ULONG *skipped)
{
    return ISpVoice_Skip(impl_from_ISpVoice(iface)->inner, type, items, skipped);
}

static HRESULT WINAPI voice_SetPriority(ISpVoice *iface, SPVPRIORITY priority)
{
    impl_from_ISpVoice(iface)->priority = priority;
    return S_OK;
}

static HRESULT WINAPI voice_GetPriority(ISpVoice *iface, SPVPRIORITY *priority)
{
    if (!priority) return E_POINTER;
    *priority = impl_from_ISpVoice(iface)->priority;
    return S_OK;
}

static HRESULT WINAPI voice_SetAlertBoundary(ISpVoice *iface, SPEVENTENUM boundary)
{
    impl_from_ISpVoice(iface)->alert_boundary = boundary;
    return S_OK;
}

static HRESULT WINAPI voice_GetAlertBoundary(ISpVoice *iface, SPEVENTENUM *boundary)
{
    if (!boundary) return E_POINTER;
    *boundary = impl_from_ISpVoice(iface)->alert_boundary;
    return S_OK;
}

static HRESULT WINAPI voice_SetRate(ISpVoice *iface, LONG rate)
{
    return ISpVoice_SetRate(impl_from_ISpVoice(iface)->inner, rate);
}

static HRESULT WINAPI voice_GetRate(ISpVoice *iface, LONG *rate)
{
    return ISpVoice_GetRate(impl_from_ISpVoice(iface)->inner, rate);
}

static HRESULT WINAPI voice_SetVolume(ISpVoice *iface, USHORT volume)
{
    return ISpVoice_SetVolume(impl_from_ISpVoice(iface)->inner, volume);
}

static HRESULT WINAPI voice_GetVolume(ISpVoice *iface, USHORT *volume)
{
    return ISpVoice_GetVolume(impl_from_ISpVoice(iface)->inner, volume);
}

static HRESULT WINAPI voice_WaitUntilDone(ISpVoice *iface, ULONG ms)
{
    struct voice *This = impl_from_ISpVoice(iface);

    return WaitForSingleObject(This->idle, ms) == WAIT_OBJECT_0 ? S_OK : S_FALSE;
}

static HRESULT WINAPI voice_SetSyncSpeakTimeout(ISpVoice *iface, ULONG ms)
{
    impl_from_ISpVoice(iface)->sync_timeout = ms;
    return S_OK;
}

static HRESULT WINAPI voice_GetSyncSpeakTimeout(ISpVoice *iface, ULONG *ms)
{
    if (!ms) return E_POINTER;
    *ms = impl_from_ISpVoice(iface)->sync_timeout;
    return S_OK;
}

static HANDLE WINAPI voice_SpeakCompleteEvent(ISpVoice *iface)
{
    return impl_from_ISpVoice(iface)->idle;
}

static HRESULT WINAPI voice_IsUISupported(ISpVoice *iface, LPCWSTR type, void *data, ULONG size, WINBOOL *supported)
{
    return ISpVoice_IsUISupported(impl_from_ISpVoice(iface)->inner, type, data, size, supported);
}

static HRESULT WINAPI voice_DisplayUI(ISpVoice *iface, HWND parent, LPCWSTR title, LPCWSTR type, void *data, ULONG size)
{
    return ISpVoice_DisplayUI(impl_from_ISpVoice(iface)->inner, parent, title, type, data, size);
}

static const ISpVoiceVtbl voice_vtbl =
{
    voice_QueryInterface,
    voice_AddRef,
    voice_Release,
    voice_SetNotifySink,
    voice_SetNotifyWindowMessage,
    voice_SetNotifyCallbackFunction,
    voice_SetNotifyCallbackInterface,
    voice_SetNotifyWin32Event,
    voice_WaitForNotifyEvent,
    voice_GetNotifyEventHandle,
    voice_SetInterest,
    voice_GetEvents,
    voice_GetInfo,
    voice_SetOutput,
    voice_GetOutputObjectToken,
    voice_GetOutputStream,
    voice_Pause,
    voice_Resume,
    voice_SetVoice,
    voice_GetVoice,
    voice_Speak,
    voice_SpeakStream,
    voice_GetStatus,
    voice_Skip,
    voice_SetPriority,
    voice_GetPriority,
    voice_SetAlertBoundary,
    voice_GetAlertBoundary,
    voice_SetRate,
    voice_GetRate,
    voice_SetVolume,
    voice_GetVolume,
    voice_WaitUntilDone,
    voice_SetSyncSpeakTimeout,
    voice_GetSyncSpeakTimeout,
    voice_SpeakCompleteEvent,
    voice_IsUISupported,
    voice_DisplayUI,
};

/* Wine's sapi.dll, from where SpVoice's class says it is (Wine puts it in system32\Speech\Common,
 * as Windows does): not on the DLL search path, and WoW's process does not find it by its name. */
static HMODULE load_wine_sapi(void)
{
    WCHAR path[MAX_PATH];
    DWORD size = sizeof(path);
    HMODULE sapi = NULL;
    char text[MAX_PATH * 3];

    if (!RegGetValueW(HKEY_CLASSES_ROOT, L"CLSID\\{96749377-3391-11D2-9EE3-00C04F797396}\\InprocServer32", NULL,
                      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, path, &size) && !wcsstr(path, L"sapifix"))
        sapi = LoadLibraryExW(path, NULL, 0);
    if (!sapi && GetSystemDirectoryW(path, MAX_PATH - 24))
    {
        wcscat(path, L"\\Speech\\Common\\sapi.dll");
        sapi = LoadLibraryExW(path, NULL, 0);
    }
    if (!sapi) sapi = LoadLibraryW(L"sapi.dll");
    if (!sapi)
    {
        DWORD error = GetLastError();
        log_line("could not load Wine's sapi.dll (%s): error %lu", utf8(path, text, sizeof(text), MAX_PATH), error);
    }
    return sapi;
}

/* Wine's own SpVoice, from sapi.dll itself: CoCreateInstance would come back here (TreatAs). */
static HRESULT create_wine_voice(ISpVoice **out)
{
    static HRESULT (WINAPI *get_class_object)(REFCLSID, REFIID, void **);
    IClassFactory *factory;
    HRESULT hr;

    if (!get_class_object)
    {
        HMODULE sapi = load_wine_sapi();

        if (sapi) get_class_object = (void *)GetProcAddress(sapi, "DllGetClassObject");
        if (!get_class_object) return REGDB_E_CLASSNOTREG;
    }
    if (FAILED(hr = get_class_object(&CLSID_SpVoice, &IID_IClassFactory, (void **)&factory)))
        return hr;
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_ISpVoice, (void **)out);
    IClassFactory_Release(factory);
    return hr;
}

static HRESULT create_voice(REFIID iid, void **out)
{
    struct voice *This = calloc(1, sizeof(*This));
    HRESULT hr;

    *out = NULL;
    if (!This) return E_OUTOFMEMORY;
    This->ISpVoice_iface.lpVtbl = &voice_vtbl;
    This->ref = 1;
    InitializeCriticalSection(&This->cs);
    This->interest = This->queued_interest = SPFEI(SPEI_END_INPUT_STREAM);
    This->last_hr = S_OK;
    This->priority = SPVPRI_NORMAL;
    This->alert_boundary = SPEI_WORD_BOUNDARY;
    This->sync_timeout = 10000;
    This->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    This->idle = CreateEventW(NULL, TRUE, TRUE, NULL);

    if (!This->wake || !This->idle) hr = HRESULT_FROM_WIN32(GetLastError());
    else hr = create_wine_voice(&This->inner);
    if (FAILED(hr))
    {
        log_line("could not make a voice: %#lx", hr);
        free_voice(This);
        return hr;
    }
    hr = ISpVoice_QueryInterface(&This->ISpVoice_iface, iid, out);
    ISpVoice_Release(&This->ISpVoice_iface);
    log_line("voice %p made", This);
    return hr;
}

/* ---- the stream --------------------------------------------------------------------------- */

/* The stream is also an ISpAudio, for one thing: when the engine speaks in another format than
 * the file's, Wine's voice tells an ISpAudio output the engine's format (SetFormat) and writes
 * that, where for a plain stream it would convert with Media Foundation's resampler, which Proton
 * does not have (it writes nothing). The stream converts instead: PCM of 8 or 16 bits or float,
 * mono or stereo, any rate, by linear interpolation. */
struct stream
{
    ISpStream ISpStream_iface;
    ISpAudio ISpAudio_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IStream *base;
    GUID format;
    WAVEFORMATEX *wfx;
    BOOL closed;
    BOOL wav_writer;            /* a WAV file written: its header is kept up to date */
    ULONGLONG data_start;       /* where a WAV file's samples start: positions count from there */

    WAVEFORMATEX in;            /* what is written to it, when SetFormat said other than wfx */
    BOOL convert;
    double step, pos;           /* input frames per output frame; where the next output frame is */
    float prev[2];              /* the last input frame of the write before */
    BOOL have_prev;
    HANDLE drained;             /* ISpAudio::EventHandle: always set, a file has nothing to play out */
};

static inline struct stream *impl_from_ISpStream(ISpStream *iface)
{
    return CONTAINING_RECORD(iface, struct stream, ISpStream_iface);
}

static inline struct stream *impl_from_ISpAudio(ISpAudio *iface)
{
    return CONTAINING_RECORD(iface, struct stream, ISpAudio_iface);
}

static BOOL convertible(const WAVEFORMATEX *wfx)
{
    if (wfx->nChannels < 1 || wfx->nChannels > 2 || !wfx->nSamplesPerSec) return FALSE;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) return wfx->wBitsPerSample == 8 || wfx->wBitsPerSample == 16;
    return wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32;
}

static float get_sample(const BYTE *p, const WAVEFORMATEX *wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return *(const float *)p;
    if (wfx->wBitsPerSample == 8) return (p[0] - 128) / 128.0f;
    return *(const SHORT *)p / 32768.0f;
}

static void put_sample(BYTE *p, float v, const WAVEFORMATEX *wfx)
{
    if (v > 1.0f) v = 1.0f;
    else if (v < -1.0f) v = -1.0f;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) *(float *)p = v;
    else if (wfx->wBitsPerSample == 8) p[0] = (BYTE)lrintf(v * 127.0f + 128.0f);
    else *(SHORT *)p = (SHORT)lrintf(v * 32767.0f);
}

/* Writes `size` bytes in the format SetFormat gave as the file's own (call with cs held). */
static HRESULT write_converted(struct stream *This, const BYTE *data, ULONG size)
{
    const WAVEFORMATEX *in = &This->in, *out = This->wfx;
    ULONG in_frames = size / in->nBlockAlign, total = in_frames + (This->have_prev ? 1 : 0), i, n = 0, written;
    WORD out_channels = out->nChannels, in_bytes = in->wBitsPerSample / 8, out_bytes = out->wBitsPerSample / 8;
    float *frames;
    BYTE *buffer;
    HRESULT hr = S_OK;

    if (!in_frames) return S_OK;
    if (!(frames = malloc(total * 2 * sizeof(float)))) return E_OUTOFMEMORY;
    if (!(buffer = malloc(((size_t)(total / This->step) + 2) * out->nBlockAlign)))
    {
        free(frames);
        return E_OUTOFMEMORY;
    }

    /* The input frames, after the last one of the write before, in the file's channels. */
    if (This->have_prev) memcpy(frames, This->prev, sizeof(This->prev));
    for (i = 0; i < in_frames; i++)
    {
        const BYTE *frame = data + i * in->nBlockAlign;
        float left = get_sample(frame, in), right = in->nChannels == 2 ? get_sample(frame + in_bytes, in) : left;
        float *to = frames + (i + (This->have_prev ? 1 : 0)) * 2;

        to[0] = out_channels == 1 ? (left + right) / 2 : left;
        to[1] = right;
    }

    while (This->pos + 1 < total)
    {
        ULONG at = (ULONG)This->pos;
        float frac = (float)(This->pos - at);
        BYTE *frame = buffer + n * out->nBlockAlign;
        WORD ch;

        for (ch = 0; ch < out_channels; ch++)
            put_sample(frame + ch * out_bytes, frames[at * 2 + ch] * (1 - frac) + frames[(at + 1) * 2 + ch] * frac, out);
        n++;
        This->pos += This->step;
    }
    This->pos -= total - 1;
    memcpy(This->prev, frames + (total - 1) * 2, sizeof(This->prev));
    This->have_prev = TRUE;

    if (n) hr = IStream_Write(This->base, buffer, n * out->nBlockAlign, &written);
    free(frames);
    free(buffer);
    return hr;
}

/* The base stream, or why there is none. */
static HRESULT ready(struct stream *This)
{
    if (This->closed) return SPERR_STREAM_CLOSED;
    if (!This->base) return SPERR_UNINITIALIZED;
    return S_OK;
}

static HRESULT write_at(IStream *stream, ULONGLONG offset, const void *data, ULONG size)
{
    LARGE_INTEGER pos;
    ULONG written;
    HRESULT hr;

    pos.QuadPart = offset;
    if (FAILED(hr = IStream_Seek(stream, pos, STREAM_SEEK_SET, NULL))) return hr;
    return IStream_Write(stream, data, size, &written);
}

/* The RIFF and data sizes of a WAV file being written, as it stands. */
static void update_header(struct stream *This)
{
    LARGE_INTEGER zero = {{0}};
    ULARGE_INTEGER here, end;
    DWORD riff, data;

    if (!This->wav_writer || !This->base) return;
    if (FAILED(IStream_Seek(This->base, zero, STREAM_SEEK_CUR, &here))) return;
    if (FAILED(IStream_Seek(This->base, zero, STREAM_SEEK_END, &end))) return;
    riff = (DWORD)(end.QuadPart - 8);
    data = (DWORD)(end.QuadPart - This->data_start);
    write_at(This->base, 4, &riff, 4);
    write_at(This->base, This->data_start - 4, &data, 4);
    IStream_Seek(This->base, *(LARGE_INTEGER *)&here, STREAM_SEEK_SET, NULL);
}

static HRESULT WINAPI stream_QueryInterface(ISpStream *iface, REFIID iid, void **out)
{
    char guid[40];

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ISequentialStream) || IsEqualIID(iid, &IID_IStream) ||
        IsEqualIID(iid, &IID_ISpStreamFormat) || IsEqualIID(iid, &IID_ISpStream))
    {
        *out = iface;
        ISpStream_AddRef(iface);
        return S_OK;
    }
    if (IsEqualIID(iid, &IID_ISpAudio))
    {
        *out = &impl_from_ISpStream(iface)->ISpAudio_iface;
        ISpStream_AddRef(iface);
        return S_OK;
    }
    log_line("stream: no interface %s", guid_str(iid, guid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(ISpStream *iface)
{
    return InterlockedIncrement(&impl_from_ISpStream(iface)->ref);
}

static ULONG WINAPI stream_Release(ISpStream *iface)
{
    struct stream *This = impl_from_ISpStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        if (This->base)
        {
            update_header(This);
            IStream_Release(This->base);
        }
        CoTaskMemFree(This->wfx);
        if (This->drained) CloseHandle(This->drained);
        DeleteCriticalSection(&This->cs);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI stream_Read(ISpStream *iface, void *data, ULONG size, ULONG *read)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This))) hr = IStream_Read(This->base, data, size, read);
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Write(ISpStream *iface, const void *data, ULONG size, ULONG *written)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        if (!This->convert) hr = IStream_Write(This->base, data, size, written);
        else if (SUCCEEDED(hr = write_converted(This, data, size)) && written) *written = size;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Seek(ISpStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    struct stream *This = impl_from_ISpStream(iface);
    ULARGE_INTEGER base_pos;
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        if (origin == STREAM_SEEK_SET) move.QuadPart += This->data_start;
        hr = IStream_Seek(This->base, move, origin, &base_pos);
        if (SUCCEEDED(hr) && pos)
            pos->QuadPart = base_pos.QuadPart > This->data_start ? base_pos.QuadPart - This->data_start : 0;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_SetSize(ISpStream *iface, ULARGE_INTEGER size)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        size.QuadPart += This->data_start;
        hr = IStream_SetSize(This->base, size);
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_CopyTo(ISpStream *iface, IStream *dest, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This))) hr = IStream_CopyTo(This->base, dest, size, read, written);
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Commit(ISpStream *iface, DWORD flags)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        update_header(This);
        hr = IStream_Commit(This->base, flags);
        /* A memory stream may have nothing to commit. */
        if (hr == E_NOTIMPL) hr = S_OK;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Revert(ISpStream *iface)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This))) hr = IStream_Revert(This->base);
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_LockRegion(ISpStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        offset.QuadPart += This->data_start;
        hr = IStream_LockRegion(This->base, offset, size, type);
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_UnlockRegion(ISpStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        offset.QuadPart += This->data_start;
        hr = IStream_UnlockRegion(This->base, offset, size, type);
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Stat(ISpStream *iface, STATSTG *stat, DWORD flags)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)) && SUCCEEDED(hr = IStream_Stat(This->base, stat, flags)))
        stat->cbSize.QuadPart = stat->cbSize.QuadPart > This->data_start ? stat->cbSize.QuadPart - This->data_start : 0;
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_Clone(ISpStream *iface, IStream **out)
{
    *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_GetFormat(ISpStream *iface, GUID *format, WAVEFORMATEX **wfx)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    if (!format) return E_POINTER;
    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        if (This->wfx)
        {
            size_t size = sizeof(WAVEFORMATEX) + This->wfx->cbSize;

            if (!wfx) hr = E_POINTER;
            else if (!(*wfx = CoTaskMemAlloc(size))) hr = E_OUTOFMEMORY;
            else memcpy(*wfx, This->wfx, size);
        }
        else if (wfx) *wfx = NULL;
        if (SUCCEEDED(hr)) *format = This->format;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static WAVEFORMATEX *copy_wfx(const WAVEFORMATEX *wfx)
{
    size_t size = sizeof(WAVEFORMATEX) + (wfx->wFormatTag == WAVE_FORMAT_PCM ? 0 : wfx->cbSize);
    WAVEFORMATEX *copy = CoTaskMemAlloc(size);

    if (!copy) return NULL;
    memcpy(copy, wfx, size);
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) copy->cbSize = 0;
    return copy;
}

static HRESULT WINAPI stream_SetBaseStream(ISpStream *iface, IStream *base, REFGUID format, const WAVEFORMATEX *wfx)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr = S_OK;

    if (!base || !format) return E_INVALIDARG;
    if (IsEqualGUID(format, &FORMAT_WaveFormatEx) && !wfx) return E_INVALIDARG;

    EnterCriticalSection(&This->cs);
    if (This->base || This->closed) hr = SPERR_ALREADY_INITIALIZED;
    else if (wfx && !(This->wfx = copy_wfx(wfx))) hr = E_OUTOFMEMORY;
    else
    {
        IStream_AddRef(base);
        This->base = base;
        This->format = *format;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI stream_GetBaseStream(ISpStream *iface, IStream **base)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    if (!base) return E_POINTER;
    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        IStream_AddRef(This->base);
        *base = This->base;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

/* Reads a WAV file's header: its format, and where its samples start. */
static HRESULT read_wav_header(struct stream *This)
{
    BYTE riff[12], chunk[8];
    LARGE_INTEGER pos;
    ULONGLONG at = 12;
    ULONG read;

    if (FAILED(IStream_Read(This->base, riff, 12, &read)) || read != 12 ||
        memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4))
        return SPERR_UNSUPPORTED_FORMAT;

    for (;;)
    {
        DWORD size;

        if (FAILED(IStream_Read(This->base, chunk, 8, &read)) || read != 8) return SPERR_UNSUPPORTED_FORMAT;
        memcpy(&size, chunk + 4, 4);
        at += 8;
        if (!memcmp(chunk, "fmt ", 4))
        {
            ULONG want = max(size, sizeof(WAVEFORMATEX));

            if (size < 16 || size > 4096 || !(This->wfx = CoTaskMemAlloc(want))) return SPERR_UNSUPPORTED_FORMAT;
            memset(This->wfx, 0, want);
            if (FAILED(IStream_Read(This->base, This->wfx, size, &read)) || read != size) return SPERR_UNSUPPORTED_FORMAT;
            if (size < sizeof(WAVEFORMATEX)) This->wfx->cbSize = 0;
        }
        else if (!memcmp(chunk, "data", 4))
        {
            if (!This->wfx) return SPERR_UNSUPPORTED_FORMAT;
            This->data_start = at;
            return S_OK;
        }
        at += size + (size & 1);
        pos.QuadPart = at;
        if (FAILED(IStream_Seek(This->base, pos, STREAM_SEEK_SET, NULL))) return SPERR_UNSUPPORTED_FORMAT;
    }
}

/* Starts a WAV file: its header, with sizes of 0 until there are samples. */
static HRESULT write_wav_header(struct stream *This)
{
    DWORD fmt_size = This->wfx->wFormatTag == WAVE_FORMAT_PCM ? 16 : sizeof(WAVEFORMATEX) + This->wfx->cbSize;
    BYTE header[12 + 8 + 8];
    DWORD zero = 0, riff = 0;
    ULONG written;
    HRESULT hr;

    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &riff, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    memcpy(header + 16, &fmt_size, 4);
    if (FAILED(hr = IStream_Write(This->base, header, 20, &written))) return hr;
    if (FAILED(hr = IStream_Write(This->base, This->wfx, fmt_size, &written))) return hr;
    memcpy(header, "data", 4);
    memcpy(header + 4, &zero, 4);
    if (FAILED(hr = IStream_Write(This->base, header, 8, &written))) return hr;
    This->data_start = 20 + fmt_size + 8;
    This->wav_writer = TRUE;
    return S_OK;
}

static HRESULT WINAPI stream_BindToFile(ISpStream *iface, LPCWSTR name, SPFILEMODE mode, const GUID *format,
                                        const WAVEFORMATEX *wfx, ULONGLONG interest)
{
    struct stream *This = impl_from_ISpStream(iface);
    BOOL reading = mode == SPFM_OPEN_READONLY || mode == SPFM_OPEN_READWRITE, wav;
    char path[400], guid[40];
    DWORD stgm;
    HRESULT hr;

    log_line("stream %p: BindToFile(%s, mode %d, format %s, %lu Hz %u-bit %u ch)", This,
             utf8(name, path, sizeof(path), 300), mode, guid_str(format, guid), wfx ? wfx->nSamplesPerSec : 0,
             wfx ? wfx->wBitsPerSample : 0, wfx ? wfx->nChannels : 0);

    if (!name || mode > SPFM_CREATE_ALWAYS) return E_INVALIDARG;
    wav = format ? IsEqualGUID(format, &FORMAT_WaveFormatEx)
                 : reading && wcslen(name) > 4 && !_wcsicmp(name + wcslen(name) - 4, L".wav");
    if (wav && !reading && !wfx) return E_INVALIDARG;

    EnterCriticalSection(&This->cs);
    if (This->base || This->closed)
    {
        LeaveCriticalSection(&This->cs);
        return SPERR_ALREADY_INITIALIZED;
    }

    if (mode == SPFM_CREATE)
    {
        /* Opened if it is there, made if not (SHCreateStreamOnFileEx does one or the other). */
        HANDLE file = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    stgm = (mode == SPFM_OPEN_READONLY ? STGM_READ : STGM_READWRITE) | STGM_SHARE_DENY_WRITE |
           (mode == SPFM_CREATE_ALWAYS ? STGM_CREATE : 0);
    hr = SHCreateStreamOnFileEx(name, stgm, FILE_ATTRIBUTE_NORMAL, mode == SPFM_CREATE_ALWAYS, NULL, &This->base);

    if (SUCCEEDED(hr))
    {
        This->format = format ? *format : wav ? FORMAT_WaveFormatEx : no_format;
        if (wav && reading) hr = read_wav_header(This);
        else if (wfx && !(This->wfx = copy_wfx(wfx))) hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr) && wav && !reading)
        {
            if (mode == SPFM_CREATE)
            {
                ULARGE_INTEGER zero = {{0}};
                IStream_SetSize(This->base, zero);
            }
            hr = write_wav_header(This);
        }
        /* Written to as well: its header follows. */
        if (SUCCEEDED(hr) && wav && mode == SPFM_OPEN_READWRITE) This->wav_writer = TRUE;
        if (FAILED(hr))
        {
            IStream_Release(This->base);
            This->base = NULL;
            CoTaskMemFree(This->wfx);
            This->wfx = NULL;
        }
    }
    LeaveCriticalSection(&This->cs);
    if (FAILED(hr)) log_line("stream %p: BindToFile failed: %#lx", This, hr);
    return hr;
}

static HRESULT WINAPI stream_Close(ISpStream *iface)
{
    struct stream *This = impl_from_ISpStream(iface);
    HRESULT hr;

    EnterCriticalSection(&This->cs);
    if (SUCCEEDED(hr = ready(This)))
    {
        update_header(This);
        IStream_Release(This->base);
        This->base = NULL;
        This->closed = TRUE;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static const ISpStreamVtbl stream_vtbl =
{
    stream_QueryInterface,
    stream_AddRef,
    stream_Release,
    stream_Read,
    stream_Write,
    stream_Seek,
    stream_SetSize,
    stream_CopyTo,
    stream_Commit,
    stream_Revert,
    stream_LockRegion,
    stream_UnlockRegion,
    stream_Stat,
    stream_Clone,
    stream_GetFormat,
    stream_SetBaseStream,
    stream_GetBaseStream,
    stream_BindToFile,
    stream_Close,
};

/* The stream as an ISpAudio: its stream methods are the stream's, SetFormat sets up the conversion,
 * and the rest is what Wine's voice asks of an audio output (a state, an event to wait for). */

static HRESULT WINAPI audio_QueryInterface(ISpAudio *iface, REFIID iid, void **out)
{
    return stream_QueryInterface(&impl_from_ISpAudio(iface)->ISpStream_iface, iid, out);
}

static ULONG WINAPI audio_AddRef(ISpAudio *iface)
{
    return stream_AddRef(&impl_from_ISpAudio(iface)->ISpStream_iface);
}

static ULONG WINAPI audio_Release(ISpAudio *iface)
{
    return stream_Release(&impl_from_ISpAudio(iface)->ISpStream_iface);
}

static HRESULT WINAPI audio_Read(ISpAudio *iface, void *data, ULONG size, ULONG *read)
{
    return stream_Read(&impl_from_ISpAudio(iface)->ISpStream_iface, data, size, read);
}

static HRESULT WINAPI audio_Write(ISpAudio *iface, const void *data, ULONG size, ULONG *written)
{
    return stream_Write(&impl_from_ISpAudio(iface)->ISpStream_iface, data, size, written);
}

static HRESULT WINAPI audio_Seek(ISpAudio *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    return stream_Seek(&impl_from_ISpAudio(iface)->ISpStream_iface, move, origin, pos);
}

static HRESULT WINAPI audio_SetSize(ISpAudio *iface, ULARGE_INTEGER size)
{
    return stream_SetSize(&impl_from_ISpAudio(iface)->ISpStream_iface, size);
}

static HRESULT WINAPI audio_CopyTo(ISpAudio *iface, IStream *dest, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    return stream_CopyTo(&impl_from_ISpAudio(iface)->ISpStream_iface, dest, size, read, written);
}

static HRESULT WINAPI audio_Commit(ISpAudio *iface, DWORD flags)
{
    return stream_Commit(&impl_from_ISpAudio(iface)->ISpStream_iface, flags);
}

static HRESULT WINAPI audio_Revert(ISpAudio *iface)
{
    return stream_Revert(&impl_from_ISpAudio(iface)->ISpStream_iface);
}

static HRESULT WINAPI audio_LockRegion(ISpAudio *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return stream_LockRegion(&impl_from_ISpAudio(iface)->ISpStream_iface, offset, size, type);
}

static HRESULT WINAPI audio_UnlockRegion(ISpAudio *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return stream_UnlockRegion(&impl_from_ISpAudio(iface)->ISpStream_iface, offset, size, type);
}

static HRESULT WINAPI audio_Stat(ISpAudio *iface, STATSTG *stat, DWORD flags)
{
    return stream_Stat(&impl_from_ISpAudio(iface)->ISpStream_iface, stat, flags);
}

static HRESULT WINAPI audio_Clone(ISpAudio *iface, IStream **out)
{
    return stream_Clone(&impl_from_ISpAudio(iface)->ISpStream_iface, out);
}

static HRESULT WINAPI audio_GetFormat(ISpAudio *iface, GUID *format, WAVEFORMATEX **wfx)
{
    return stream_GetFormat(&impl_from_ISpAudio(iface)->ISpStream_iface, format, wfx);
}

static HRESULT WINAPI audio_SetState(ISpAudio *iface, SPAUDIOSTATE state, ULONGLONG reserved)
{
    return S_OK;
}

static HRESULT WINAPI audio_SetFormat(ISpAudio *iface, REFGUID format, const WAVEFORMATEX *wfx)
{
    struct stream *This = impl_from_ISpAudio(iface);
    HRESULT hr = S_OK;

    if (!format || !wfx || !IsEqualGUID(format, &FORMAT_WaveFormatEx)) return SPERR_UNSUPPORTED_FORMAT;

    EnterCriticalSection(&This->cs);
    if (FAILED(hr = ready(This))) ;
    else if (!This->wfx || !convertible(This->wfx) || !convertible(wfx)) hr = SPERR_UNSUPPORTED_FORMAT;
    else
    {
        BOOL same = wfx->wFormatTag == This->wfx->wFormatTag && wfx->nChannels == This->wfx->nChannels &&
                    wfx->nSamplesPerSec == This->wfx->nSamplesPerSec && wfx->wBitsPerSample == This->wfx->wBitsPerSample;

        if (same != !This->convert || (!same && memcmp(&This->in, wfx, sizeof(This->in))))
            log_line("stream %p: written as %lu Hz %u-bit %u ch%s", This, wfx->nSamplesPerSec, wfx->wBitsPerSample,
                     wfx->nChannels, same ? ", the file's own format" : ", converted to the file's");
        This->convert = !same;
        This->in = *wfx;
        This->in.cbSize = 0;
        This->step = (double)wfx->nSamplesPerSec / This->wfx->nSamplesPerSec;
        This->pos = 0;
        This->have_prev = FALSE;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT WINAPI audio_GetStatus(ISpAudio *iface, SPAUDIOSTATUS *status)
{
    struct stream *This = impl_from_ISpAudio(iface);
    LARGE_INTEGER zero = {{0}};
    ULARGE_INTEGER pos = {{0}};

    if (!status) return E_POINTER;
    memset(status, 0, sizeof(*status));
    stream_Seek(&This->ISpStream_iface, zero, STREAM_SEEK_CUR, &pos);
    status->cbFreeBuffSpace = 0x10000;
    status->State = SPAS_RUN;
    status->CurSeekPos = status->CurDevicePos = pos.QuadPart;
    return S_OK;
}

static HRESULT WINAPI audio_SetBufferInfo(ISpAudio *iface, const SPAUDIOBUFFERINFO *info)
{
    return S_OK;
}

static HRESULT WINAPI audio_GetBufferInfo(ISpAudio *iface, SPAUDIOBUFFERINFO *info)
{
    if (!info) return E_POINTER;
    info->ulMsMinNotification = 50;
    info->ulMsBufferSize = 500;
    info->ulMsEventBias = 0;
    return S_OK;
}

static HRESULT WINAPI audio_GetDefaultFormat(ISpAudio *iface, GUID *format, WAVEFORMATEX **wfx)
{
    return stream_GetFormat(&impl_from_ISpAudio(iface)->ISpStream_iface, format, wfx);
}

static HANDLE WINAPI audio_EventHandle(ISpAudio *iface)
{
    return impl_from_ISpAudio(iface)->drained;
}

static HRESULT WINAPI audio_GetVolumeLevel(ISpAudio *iface, ULONG *level)
{
    if (!level) return E_POINTER;
    *level = 10000;
    return S_OK;
}

static HRESULT WINAPI audio_SetVolumeLevel(ISpAudio *iface, ULONG level)
{
    return S_OK;
}

static HRESULT WINAPI audio_GetBufferNotifySize(ISpAudio *iface, ULONG *size)
{
    if (!size) return E_POINTER;
    *size = 0;
    return S_OK;
}

static HRESULT WINAPI audio_SetBufferNotifySize(ISpAudio *iface, ULONG size)
{
    return S_OK;
}

static const ISpAudioVtbl audio_vtbl =
{
    audio_QueryInterface,
    audio_AddRef,
    audio_Release,
    audio_Read,
    audio_Write,
    audio_Seek,
    audio_SetSize,
    audio_CopyTo,
    audio_Commit,
    audio_Revert,
    audio_LockRegion,
    audio_UnlockRegion,
    audio_Stat,
    audio_Clone,
    audio_GetFormat,
    audio_SetState,
    audio_SetFormat,
    audio_GetStatus,
    audio_SetBufferInfo,
    audio_GetBufferInfo,
    audio_GetDefaultFormat,
    audio_EventHandle,
    audio_GetVolumeLevel,
    audio_SetVolumeLevel,
    audio_GetBufferNotifySize,
    audio_SetBufferNotifySize,
};

/* Whether `unk` is one of this DLL's streams. */
static BOOL is_own_stream(IUnknown *unk)
{
    ISpStream *stream;
    BOOL own;

    if (!unk || FAILED(IUnknown_QueryInterface(unk, &IID_ISpStream, (void **)&stream))) return FALSE;
    own = stream->lpVtbl == &stream_vtbl;
    ISpStream_Release(stream);
    return own;
}

static HRESULT create_stream(REFIID iid, void **out)
{
    struct stream *This = calloc(1, sizeof(*This));
    HRESULT hr;

    *out = NULL;
    if (!This) return E_OUTOFMEMORY;
    This->ISpStream_iface.lpVtbl = &stream_vtbl;
    This->ISpAudio_iface.lpVtbl = &audio_vtbl;
    This->ref = 1;
    InitializeCriticalSection(&This->cs);
    if (!(This->drained = CreateEventW(NULL, TRUE, TRUE, NULL)))
    {
        DeleteCriticalSection(&This->cs);
        free(This);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = ISpStream_QueryInterface(&This->ISpStream_iface, iid, out);
    ISpStream_Release(&This->ISpStream_iface);
    return hr;
}

/* ---- the COM server ----------------------------------------------------------------------- */

struct factory
{
    IClassFactory IClassFactory_iface;
    HRESULT (*create)(REFIID iid, void **out);
};

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    struct factory *This = CONTAINING_RECORD(iface, struct factory, IClassFactory_iface);

    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return This->create(iid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, WINBOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateInstance,
    factory_LockServer,
};

static struct factory voice_factory = { { &factory_vtbl }, create_voice };
static struct factory stream_factory = { { &factory_vtbl }, create_stream };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    char guid[40];

    if (IsEqualCLSID(clsid, &CLSID_SapiFixVoice) || IsEqualCLSID(clsid, &CLSID_SpVoice))
        return IClassFactory_QueryInterface(&voice_factory.IClassFactory_iface, iid, out);
    if (IsEqualCLSID(clsid, &CLSID_SapiFixStream) || IsEqualCLSID(clsid, &CLSID_SpStream))
        return IClassFactory_QueryInterface(&stream_factory.IClassFactory_iface, iid, out);
    log_line("no class %s", guid_str(clsid, guid));
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

static LSTATUS set_value(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
    HKEY key;
    LSTATUS status = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);

    if (status) return status;
    status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (wcslen(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return status;
}

static LSTATUS register_class(const WCHAR *clsid, const WCHAR *name, const WCHAR *module)
{
    WCHAR path[128];
    LSTATUS status;

    swprintf(path, ARRAY_SIZE(path), L"Software\\Classes\\CLSID\\%ls", clsid);
    if ((status = set_value(HKEY_LOCAL_MACHINE, path, NULL, name))) return status;
    wcscat(path, L"\\InprocServer32");
    if ((status = set_value(HKEY_LOCAL_MACHINE, path, NULL, module))) return status;
    return set_value(HKEY_LOCAL_MACHINE, path, L"ThreadingModel", L"Both");
}

#define VOICE_CLSID     L"{FE430F78-ED03-4645-8FF9-11E560BC9D08}"
#define STREAM_CLSID    L"{F8BB12F6-862E-4F04-AB0C-CBF79AE522E2}"
#define SPVOICE_KEY     L"Software\\Classes\\CLSID\\{96749377-3391-11D2-9EE3-00C04F797396}\\TreatAs"
#define SPSTREAM_KEY    L"Software\\Classes\\CLSID\\{715D9C59-4442-11D2-9605-00C04F8EE628}\\TreatAs"

HRESULT WINAPI DllRegisterServer(void)
{
    WCHAR module[MAX_PATH];
    LSTATUS status;

    GetModuleFileNameW(instance, module, MAX_PATH);
    if ((status = register_class(VOICE_CLSID, L"SapiFix Voice (WoW text to speech under Wine)", module)) ||
        (status = register_class(STREAM_CLSID, L"SapiFix Stream (WoW text to speech under Wine)", module)) ||
        (status = set_value(HKEY_LOCAL_MACHINE, SPVOICE_KEY, NULL, VOICE_CLSID)) ||
        (status = set_value(HKEY_LOCAL_MACHINE, SPSTREAM_KEY, NULL, STREAM_CLSID)))
        return HRESULT_FROM_WIN32(status);
    log_line("registered: SpVoice and SpStream are treated as this DLL's");
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, SPVOICE_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, SPSTREAM_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" VOICE_CLSID);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" STREAM_CLSID);
    log_line("unregistered: SpVoice and SpStream are Wine's again");
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        WCHAR exe[MAX_PATH];
        char text[400];

        instance = dll;
        DisableThreadLibraryCalls(dll);
        InitializeCriticalSection(&log_cs);
        log_init();
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        log_line("loaded into %s", utf8(exe, text, sizeof(text), 300));
    }
    return TRUE;
}
