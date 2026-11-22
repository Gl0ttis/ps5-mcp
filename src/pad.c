/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "pad.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "klog.h"
#include "pad_klog.h"
#include "util.h"

static pthread_mutex_t g_console = PTHREAD_MUTEX_INITIALIZER;

static struct
{
    pthread_mutex_t lock;
    bool open;
    bool thread_started;
    int handle;
    uint64_t device_id;
    int user_id;
    bool exclusive;
    int last_insert;
    uint64_t inserts;
    uint64_t insert_errors;
    uint64_t opened_ms;
    pad_state_t state;
} g_pad = {.lock = PTHREAD_MUTEX_INITIALIZER, .handle = -1, .user_id = -1};

static const struct
{
    const char *name;
    uint32_t mask;
} k_buttons[] = {
    {"cross", BTN_CROSS},       {"x", BTN_CROSS},           {"circle", BTN_CIRCLE},
    {"o", BTN_CIRCLE},          {"square", BTN_SQUARE},     {"triangle", BTN_TRIANGLE},
    {"up", BTN_UP},             {"down", BTN_DOWN},         {"left", BTN_LEFT},
    {"right", BTN_RIGHT},       {"dpad_up", BTN_UP},        {"dpad_down", BTN_DOWN},
    {"dpad_left", BTN_LEFT},    {"dpad_right", BTN_RIGHT},  {"l1", BTN_L1},
    {"r1", BTN_R1},             {"l2", BTN_L2},             {"r2", BTN_R2},
    {"l3", BTN_L3},             {"r3", BTN_R3},             {"options", BTN_OPTIONS},
    {"start", BTN_OPTIONS},     {"create", BTN_CREATE},     {"share", BTN_CREATE},
    {"ps", BTN_PS},             {"home", BTN_PS},           {"touchpad", BTN_TOUCHPAD},
    {"touch", BTN_TOUCHPAD},
};

void console_lock(void)
{
    pthread_mutex_lock(&g_console);
}

void console_unlock(void)
{
    pthread_mutex_unlock(&g_console);
}

const char *pad_button_list(void)
{
    return "cross, circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3, "
           "options, create, ps, touchpad (aliases: x, o, dpad_*, start, share, home, touch)";
}

bool pad_parse_buttons(const char *text, uint32_t *mask, char *err, size_t err_len)
{
    *mask = 0;
    char token[32];
    const char *p = text;
    while (*p)
    {
        while (*p == '+' || *p == ',' || *p == ' ' || *p == '|')
            p++;
        if (*p == '\0')
            break;
        size_t n = 0;
        while (*p && *p != '+' && *p != ',' && *p != ' ' && *p != '|' && n + 1 < sizeof(token))
            token[n++] = (char)tolower((unsigned char)*p++);
        token[n] = '\0';
        bool found = false;
        for (size_t i = 0; i < ARRAY_LEN(k_buttons); i++)
            if (strcmp(token, k_buttons[i].name) == 0)
            {
                *mask |= k_buttons[i].mask;
                found = true;
                break;
            }
        if (!found)
        {
            snprintf(err, err_len, "unknown button '%s' (buttons: %s)", token, pad_button_list());
            return false;
        }
    }
    return true;
}

void pad_mask_names(uint32_t mask, char *out, size_t out_len)
{
    static const struct
    {
        const char *name;
        uint32_t mask;
    } canonical[] = {
        {"cross", BTN_CROSS}, {"circle", BTN_CIRCLE}, {"square", BTN_SQUARE},
        {"triangle", BTN_TRIANGLE}, {"up", BTN_UP}, {"down", BTN_DOWN},
        {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"l1", BTN_L1}, {"r1", BTN_R1},
        {"l2", BTN_L2}, {"r2", BTN_R2}, {"l3", BTN_L3}, {"r3", BTN_R3},
        {"options", BTN_OPTIONS}, {"create", BTN_CREATE}, {"ps", BTN_PS},
        {"touchpad", BTN_TOUCHPAD},
    };
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < ARRAY_LEN(canonical); i++)
        if (mask & canonical[i].mask)
        {
            const int n = snprintf(out + used, out_len - used, "%s%s", used ? "+" : "",
                                   canonical[i].name);
            if (n < 0 || (size_t)n >= out_len - used)
                break;
            used += (size_t)n;
        }
    if (used == 0)
        snprintf(out, out_len, "none");
}

void pad_neutral_state(pad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->lx = s->ly = s->rx = s->ry = 128;
}

static void *insert_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_pad.lock);
        if (g_pad.open)
        {
            g_pad.last_insert = backend_pad_insert(g_pad.handle, &g_pad.state);
            g_pad.inserts++;
            if (g_pad.last_insert != 0)
                g_pad.insert_errors++;
        }
        pthread_mutex_unlock(&g_pad.lock);
        sleep_ms(PAD_INSERT_INTERVAL_MS);
    }
    return NULL;
}

bool pad_is_open(void)
{
    pthread_mutex_lock(&g_pad.lock);
    const bool open = g_pad.open;
    pthread_mutex_unlock(&g_pad.lock);
    return open;
}

void pad_set(const pad_state_t *state)
{
    pad_state_t s = *state;
    /* The console reads the triggers from their analog bytes: the bit alone
     * does not register. And the analog value alone should set the bit. */
    if ((s.buttons & BTN_L2) && s.l2 == 0)
        s.l2 = 255;
    if ((s.buttons & BTN_R2) && s.r2 == 0)
        s.r2 = 255;
    if (s.l2 > 0)
        s.buttons |= BTN_L2;
    if (s.r2 > 0)
        s.buttons |= BTN_R2;
    pthread_mutex_lock(&g_pad.lock);
    g_pad.state = s;
    pthread_mutex_unlock(&g_pad.lock);
}

pad_state_t pad_get(void)
{
    pthread_mutex_lock(&g_pad.lock);
    const pad_state_t s = g_pad.state;
    pthread_mutex_unlock(&g_pad.lock);
    return s;
}

void pad_tap(uint32_t buttons, unsigned hold_ms, unsigned gap_ms)
{
    pad_state_t s;
    pad_neutral_state(&s);
    s.buttons = buttons;
    pad_set(&s);
    sleep_ms(hold_ms);
    pad_neutral_state(&s);
    pad_set(&s);
    sleep_ms(gap_ms);
}

/* A title reads its user's first controller slot. Disconnect every other
 * controller in the table the console logs after the bind. Returns how many
 * were disconnected. */
static int evict_others(uint64_t ours, uint64_t since, cJSON *report)
{
    padlog_controller_t pads[8];
    const int count = padlog_controllers(since, 2500, pads, 8);
    if (count < 0)
    {
        cJSON_AddStringToObject(report, "evict",
                                "the console logged no controller table: if a real controller "
                                "is connected, titles read it and not this pad");
        return 0;
    }
    cJSON *evicted = cJSON_GetObjectItem(report, "evicted");
    if (evicted == NULL)
        evicted = cJSON_AddArrayToObject(report, "evicted");
    int n = 0;
    for (int i = 0; i < count; i++)
    {
        const padlog_controller_t *c = &pads[i];
        /* Every controller but ours. A real DualSense reports sub type 22 too
         * (seen on 13.20: the user's USB pad 0x70301 next to ours 0xa030d), so
         * the sub type cannot tell real pads from virtual ones. */
        if (c->device == ours || c->device == 0 || c->device == 0xffffffffffffffffull)
            continue;
        char text[96];
        snprintf(text, sizeof(text), "0x%llx (main %d sub %d) -> 0x%08x",
                 (unsigned long long)c->device, c->main_type, c->sub_type,
                 (unsigned)backend_pad_disconnect(c->device));
        cJSON_AddItemToArray(evicted, cJSON_CreateString(text));
        n++;
    }
    return n;
}

static void activate(int handle, uint64_t device_id, int user_id, bool exclusive)
{
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral_state(&g_pad.state);
    g_pad.handle = handle;
    g_pad.device_id = device_id;
    g_pad.user_id = user_id;
    g_pad.exclusive = exclusive;
    g_pad.inserts = 0;
    g_pad.insert_errors = 0;
    g_pad.opened_ms = now_ms();
    g_pad.open = true;
    pthread_mutex_unlock(&g_pad.lock);
}

int pad_open(const pad_open_opts_t *opts, cJSON *report)
{
    pthread_mutex_lock(&g_pad.lock);
    if (!g_pad.thread_started)
    {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, insert_thread, NULL) == 0)
            g_pad.thread_started = true;
        pthread_attr_destroy(&attr);
    }
    const bool already = g_pad.open;
    pthread_mutex_unlock(&g_pad.lock);
    if (already)
    {
        cJSON_AddStringToObject(report, "note", "already open");
        return 0;
    }

    uint64_t klog_before = klog_next_seq();
    uint64_t device_id = 0;
    int user_id = -1;
    int handle = backend_pad_create(opts, report, &device_id, &user_id);
    if (handle < 0)
        return -1;
    activate(handle, device_id, user_id, opts->exclusive);

    if (opts->exclusive && evict_others(device_id, klog_before, report) > 0)
    {
        /* The pad keeps the slot it was given: with the real controller gone
         * the first slot is empty and titles still read nothing (13.20). A pad
         * created now takes that first slot, so create it again. */
        pthread_mutex_lock(&g_pad.lock);
        g_pad.open = false;
        pthread_mutex_unlock(&g_pad.lock);
        backend_pad_destroy(handle);
        cJSON *second = cJSON_AddObjectToObject(report, "recreated_for_first_slot");
        klog_before = klog_next_seq();
        handle = backend_pad_create(opts, second, &device_id, &user_id);
        if (handle < 0)
            return -1;
        activate(handle, device_id, user_id, opts->exclusive);
        /* A controller that reconnected in between would hold the first slot again. */
        evict_others(device_id, klog_before, report);
    }

    if (opts->dismiss_prompt)
    {
        /* The console still asks who the new controller belongs to although it
         * is bound already; Circle from the pad itself closes the question. */
        sleep_ms(1500);
        pad_tap(BTN_CIRCLE, 80, 100);
        cJSON_AddStringToObject(report, "prompt", "dismissed with circle");
    }
    return 0;
}

int pad_close(cJSON *report)
{
    pthread_mutex_lock(&g_pad.lock);
    const bool was_open = g_pad.open;
    const int handle = g_pad.handle;
    g_pad.open = false;
    g_pad.handle = -1;
    pthread_mutex_unlock(&g_pad.lock);
    if (!was_open)
    {
        cJSON_AddStringToObject(report, "note", "not open");
        return 0;
    }
    pad_state_t neutral;
    pad_neutral_state(&neutral);
    backend_pad_insert(handle, &neutral);
    const int r = backend_pad_destroy(handle);
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)r);
    cJSON_AddStringToObject(report, "delete", hex);
    return r;
}

void pad_status(cJSON *obj)
{
    pthread_mutex_lock(&g_pad.lock);
    char text[128];
    cJSON_AddBoolToObject(obj, "open", g_pad.open);
    if (g_pad.open)
    {
        snprintf(text, sizeof(text), "0x%llx", (unsigned long long)g_pad.device_id);
        cJSON_AddStringToObject(obj, "device_id", text);
        snprintf(text, sizeof(text), "0x%x", (unsigned)g_pad.handle);
        cJSON_AddStringToObject(obj, "handle", text);
        snprintf(text, sizeof(text), "0x%08x", (unsigned)g_pad.user_id);
        cJSON_AddStringToObject(obj, "user_id", text);
        cJSON_AddBoolToObject(obj, "exclusive", g_pad.exclusive);
        cJSON_AddNumberToObject(obj, "open_for_s", (double)(now_ms() - g_pad.opened_ms) / 1000.0);
        cJSON_AddNumberToObject(obj, "inserts", (double)g_pad.inserts);
        cJSON_AddNumberToObject(obj, "insert_errors", (double)g_pad.insert_errors);
        snprintf(text, sizeof(text), "0x%08x", (unsigned)g_pad.last_insert);
        cJSON_AddStringToObject(obj, "last_insert", text);
        cJSON *held = cJSON_AddObjectToObject(obj, "held");
        pad_mask_names(g_pad.state.buttons, text, sizeof(text));
        cJSON_AddStringToObject(held, "buttons", text);
        cJSON_AddNumberToObject(held, "lx", g_pad.state.lx);
        cJSON_AddNumberToObject(held, "ly", g_pad.state.ly);
        cJSON_AddNumberToObject(held, "rx", g_pad.state.rx);
        cJSON_AddNumberToObject(held, "ry", g_pad.state.ry);
        cJSON_AddNumberToObject(held, "l2", g_pad.state.l2);
        cJSON_AddNumberToObject(held, "r2", g_pad.state.r2);
    }
    pthread_mutex_unlock(&g_pad.lock);
}
