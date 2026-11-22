/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Controller tools. Presses and waits are timed on the console; a whole
 * sequence is checked before the first press, so a typo never leaves the
 * console halfway through a menu. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mcp.h"
#include "pad.h"
#include "screen.h"
#include "util.h"

#define MAX_STEPS 256

static uint8_t axis_from_unit(double v)
{
    v = CLAMP(v, -1.0, 1.0);
    /* -1 -> 0, 0 -> 128, 1 -> 255 */
    const double b = v < 0 ? 128.0 + v * 128.0 : 128.0 + v * 127.0;
    return (uint8_t)CLAMP((int)lround(b), 0, 255);
}

static uint8_t trigger_from_unit(double v)
{
    v = CLAMP(v, 0.0, 1.0);
    return (uint8_t)lround(v * 255.0);
}

static bool need_open(tool_result_t *r)
{
    if (pad_is_open())
        return true;
    tr_error(r, "the virtual pad is not open: call pad_open first");
    return false;
}

/* ---- pad_open / pad_close / pad_status -------------------------------- */

static void tool_pad_open(const cJSON *args, tool_result_t *r)
{
    pad_open_opts_t o = {
        .user_id = arg_int(args, "user_id", 0),
        .exclusive = arg_bool(args, "exclusive", true),
        .dismiss_prompt = arg_bool(args, "dismiss_prompt", true),
    };
    cJSON *report = cJSON_CreateObject();
    console_lock();
    const int rc = pad_open(&o, report);
    console_unlock();
    cJSON *status = cJSON_AddObjectToObject(report, "status");
    pad_status(status);
    if (rc != 0)
    {
        r->is_error = true;
        tr_text(r, "pad_open failed; every step's result follows (bring-up data for this "
                   "firmware):");
    }
    else if (o.exclusive)
        tr_text(r, "Virtual pad open. Other controllers of the user were disconnected so titles "
                   "read this pad; the PS button on a real controller reconnects it. Tell the "
                   "user when you finish.");
    else
        tr_text(r, "Virtual pad open beside the real controller (exclusive=false): the home "
                   "screen sees it, a running title may not.");
    tr_json(r, report);
}

static void tool_pad_close(const cJSON *args, tool_result_t *r)
{
    (void)args;
    cJSON *report = cJSON_CreateObject();
    console_lock();
    pad_close(report);
    console_unlock();
    tr_json(r, report);
}

static void tool_pad_status(const cJSON *args, tool_result_t *r)
{
    (void)args;
    cJSON *obj = cJSON_CreateObject();
    pad_status(obj);
    tr_json(r, obj);
}

/* Optional screenshot at the end of an input tool. */
static void maybe_shot(const cJSON *args, tool_result_t *r)
{
    if (!arg_bool(args, "screenshot_after", false))
        return;
    const int settle = CLAMP(arg_int(args, "settle_ms", 500), 0, 10000);
    sleep_ms((unsigned)settle);
    const cJSON *shot_args = arg_get(args, "screenshot");
    shot_opts_t o;
    tool_result_t tmp = {0};
    if (!shot_opts_parse(cJSON_IsObject(shot_args) ? shot_args : NULL, &o, &tmp))
    {
        cJSON_Delete(tmp.content);
        tr_error(r, "bad screenshot options");
        return;
    }
    shot_take(&o, r);
}

/* ---- pad_press ------------------------------------------------------- */

static void tool_pad_press(const cJSON *args, tool_result_t *r)
{
    const char *names = arg_str(args, "buttons", NULL);
    const cJSON *arr = arg_get(args, "buttons");
    char joined[256] = "";
    if (cJSON_IsArray(arr))
    {
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it))
        {
            strncat(joined, joined[0] ? "+" : "", sizeof(joined) - strlen(joined) - 1);
            strncat(joined, it->valuestring, sizeof(joined) - strlen(joined) - 1);
        }
        names = joined;
    }
    if (names == NULL || names[0] == '\0')
    {
        tr_error(r, "buttons is required, e.g. \"cross\" or \"l1+r1\" (%s)", pad_button_list());
        return;
    }
    uint32_t mask;
    char err[256];
    if (!pad_parse_buttons(names, &mask, err, sizeof(err)))
    {
        tr_error(r, "%s", err);
        return;
    }
    const int hold = CLAMP(arg_int(args, "hold_ms", 100), 16, 10000);
    const int repeat = CLAMP(arg_int(args, "repeat", 1), 1, 100);
    const int gap = CLAMP(arg_int(args, "gap_ms", 150), 16, 10000);
    if ((uint64_t)(hold + gap) * (uint64_t)repeat > PAD_MAX_CALL_MS)
    {
        tr_error(r, "that would take longer than %d s; split it", PAD_MAX_CALL_MS / 1000);
        return;
    }
    if (mask & BTN_CREATE && hold < 1200)
        tr_text(r, "warning: a short Create press opens the Create menu; press circle to close "
                   "it. Use the screenshot tool for screenshots.");
    console_lock();
    if (!need_open(r))
    {
        console_unlock();
        return;
    }
    for (int i = 0; i < repeat; i++)
        pad_tap(mask, (unsigned)hold, (unsigned)gap);
    char pressed[128];
    pad_mask_names(mask, pressed, sizeof(pressed));
    tr_text(r, "pressed %s x%d (hold %d ms, gap %d ms)", pressed, repeat, hold, gap);
    maybe_shot(args, r);
    console_unlock();
}

/* ---- pad_hold / pad_release ------------------------------------------ */

static bool read_stick(const cJSON *args, const char *key, uint8_t *x, uint8_t *y)
{
    const cJSON *s = arg_get(args, key);
    if (!cJSON_IsObject(s))
        return false;
    *x = axis_from_unit(arg_num(s, "x", 0));
    *y = axis_from_unit(arg_num(s, "y", 0));
    return true;
}

static void tool_pad_hold(const cJSON *args, tool_result_t *r)
{
    pad_state_t s;
    pad_neutral_state(&s);
    const char *names = arg_str(args, "buttons", "");
    char err[256];
    if (!pad_parse_buttons(names, &s.buttons, err, sizeof(err)))
    {
        tr_error(r, "%s", err);
        return;
    }
    read_stick(args, "left_stick", &s.lx, &s.ly);
    read_stick(args, "right_stick", &s.rx, &s.ry);
    s.l2 = trigger_from_unit(arg_num(args, "l2", 0));
    s.r2 = trigger_from_unit(arg_num(args, "r2", 0));
    const cJSON *touch = arg_get(args, "touch");
    if (cJSON_IsObject(touch))
    {
        s.touch = true;
        s.touch_x = (uint16_t)CLAMP(arg_int(touch, "x", 960), 0, 1919);
        s.touch_y = (uint16_t)CLAMP(arg_int(touch, "y", 540), 0, 1079);
    }
    const int duration = arg_int(args, "duration_ms", 0);
    if (duration < 0 || duration > PAD_MAX_CALL_MS)
    {
        tr_error(r, "duration_ms is 0 (hold until pad_release) to %d", PAD_MAX_CALL_MS);
        return;
    }
    console_lock();
    if (!need_open(r))
    {
        console_unlock();
        return;
    }
    pad_set(&s);
    s = pad_get(); /* what is actually held (triggers imply their buttons) */
    char held[128];
    pad_mask_names(s.buttons, held, sizeof(held));
    if (duration > 0)
    {
        sleep_ms((unsigned)duration);
        pad_state_t n;
        pad_neutral_state(&n);
        pad_set(&n);
        tr_text(r, "held %s lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u for %d ms, then released", held,
                s.lx, s.ly, s.rx, s.ry, s.l2, s.r2, duration);
        maybe_shot(args, r);
    }
    else
        tr_text(r, "holding %s lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u until pad_release or the next "
                   "input",
                held, s.lx, s.ly, s.rx, s.ry, s.l2, s.r2);
    console_unlock();
}

static void tool_pad_release(const cJSON *args, tool_result_t *r)
{
    (void)args;
    pad_state_t n;
    pad_neutral_state(&n);
    if (!need_open(r))
        return;
    pad_set(&n);
    tr_text(r, "released: buttons up, sticks centred, triggers at rest");
}

/* ---- pad_sequence ---------------------------------------------------- */

typedef enum
{
    STEP_PRESS,
    STEP_WAIT,
    STEP_STICK,
    STEP_SHOT,
} step_kind_t;

typedef struct step
{
    step_kind_t kind;
    uint32_t mask;
    int ms;
    int repeat;
    bool right; /* STEP_STICK: right stick */
    uint8_t x, y;
    char text[48];
} step_t;

static bool parse_direction(const char *d, double *x, double *y)
{
    static const struct
    {
        const char *name;
        double x, y;
    } dirs[] = {
        {"up", 0, -1},          {"down", 0, 1},          {"left", -1, 0},
        {"right", 1, 0},        {"upleft", -0.707, -0.707}, {"upright", 0.707, -0.707},
        {"downleft", -0.707, 0.707}, {"downright", 0.707, 0.707}, {"center", 0, 0},
        {"centre", 0, 0},
    };
    char clean[16];
    size_t n = 0;
    for (const char *p = d; *p && n + 1 < sizeof(clean); p++)
        if (*p != '-' && *p != '_')
            clean[n++] = (char)tolower((unsigned char)*p);
    clean[n] = '\0';
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++)
        if (strcmp(clean, dirs[i].name) == 0)
        {
            *x = dirs[i].x;
            *y = dirs[i].y;
            return true;
        }
    char *end;
    *x = strtod(d, &end);
    if (*end != ',')
        return false;
    *y = strtod(end + 1, &end);
    return *end == '\0';
}

/* Parse one token into a step. */
static bool parse_step(const char *tok, int default_hold, step_t *s, char *err, size_t err_len)
{
    memset(s, 0, sizeof(*s));
    s->repeat = 1;
    snprintf(s->text, sizeof(s->text), "%s", tok);
    char work[96];
    snprintf(work, sizeof(work), "%s", tok);
    for (char *p = work; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    char *star = strchr(work, '*');
    if (star)
    {
        *star = '\0';
        s->repeat = atoi(star + 1);
        if (s->repeat < 1 || s->repeat > 100)
        {
            snprintf(err, err_len, "'%s': repeat is 1 to 100", tok);
            return false;
        }
    }
    char *colon = strchr(work, ':');
    char *arg = NULL;
    if (colon)
    {
        *colon = '\0';
        arg = colon + 1;
    }

    if (strcmp(work, "wait") == 0 || strcmp(work, "w") == 0 || strcmp(work, "sleep") == 0)
    {
        s->kind = STEP_WAIT;
        s->ms = arg ? atoi(arg) : 500;
        if (s->ms < 0 || s->ms > PAD_MAX_CALL_MS)
        {
            snprintf(err, err_len, "'%s': wait is 0 to %d ms", tok, PAD_MAX_CALL_MS);
            return false;
        }
        return true;
    }
    if (strcmp(work, "shot") == 0 || strcmp(work, "screenshot") == 0)
    {
        s->kind = STEP_SHOT;
        return true;
    }
    if (strcmp(work, "ls") == 0 || strcmp(work, "rs") == 0)
    {
        s->kind = STEP_STICK;
        s->right = work[0] == 'r';
        if (arg == NULL)
        {
            snprintf(err, err_len, "'%s': use ls:DIRECTION[:ms] or ls:X,Y[:ms]", tok);
            return false;
        }
        char *ms = strchr(arg, ':');
        if (ms)
            *ms++ = '\0';
        double x, y;
        if (!parse_direction(arg, &x, &y))
        {
            snprintf(err, err_len, "'%s': direction is up/down/left/right/upleft/... or X,Y in "
                                   "-1..1", tok);
            return false;
        }
        s->x = axis_from_unit(x);
        s->y = axis_from_unit(y);
        s->ms = ms ? atoi(ms) : 300;
        if (s->ms < 1 || s->ms > PAD_MAX_CALL_MS)
        {
            snprintf(err, err_len, "'%s': hold is 1 to %d ms", tok, PAD_MAX_CALL_MS);
            return false;
        }
        return true;
    }
    s->kind = STEP_PRESS;
    if (!pad_parse_buttons(work, &s->mask, err, err_len))
        return false;
    s->ms = arg ? atoi(arg) : default_hold;
    if (s->ms < 16 || s->ms > PAD_MAX_CALL_MS)
    {
        snprintf(err, err_len, "'%s': hold is 16 to %d ms", tok, PAD_MAX_CALL_MS);
        return false;
    }
    return true;
}

static void tool_pad_sequence(const cJSON *args, tool_result_t *r)
{
    const int default_hold = CLAMP(arg_int(args, "hold_ms", 100), 16, 10000);
    const int gap = CLAMP(arg_int(args, "gap_ms", 150), 0, 10000);

    /* Gather tokens from a string or an array of strings. */
    buf_t text;
    buf_init(&text);
    const cJSON *steps = arg_get(args, "steps");
    if (cJSON_IsString(steps))
        buf_puts(&text, steps->valuestring);
    else if (cJSON_IsArray(steps))
    {
        const cJSON *it;
        cJSON_ArrayForEach(it, steps) if (cJSON_IsString(it))
        {
            buf_puts(&text, it->valuestring);
            buf_puts(&text, " ");
        }
    }
    if (text.len == 0)
    {
        buf_free(&text);
        tr_error(r, "steps is required, e.g. \"down*2 cross wait:1500 shot\"");
        return;
    }

    step_t *plan = calloc(MAX_STEPS, sizeof(step_t));
    int count = 0;
    uint64_t total_ms = 0;
    int shots = 0;
    char err[300];
    char *save = NULL;
    for (char *tok = strtok_r(text.data, " \t\r\n;", &save); tok;
         tok = strtok_r(NULL, " \t\r\n;", &save))
    {
        if (count == MAX_STEPS)
        {
            tr_error(r, "at most %d steps per call", MAX_STEPS);
            goto done;
        }
        if (!parse_step(tok, default_hold, &plan[count], err, sizeof(err)))
        {
            tr_error(r, "nothing was pressed: %s", err);
            goto done;
        }
        const step_t *s = &plan[count];
        if (s->kind == STEP_PRESS || s->kind == STEP_STICK)
            total_ms += (uint64_t)(s->ms + gap) * (uint64_t)s->repeat;
        else if (s->kind == STEP_WAIT)
            total_ms += (uint64_t)s->ms * (uint64_t)s->repeat;
        else
        {
            total_ms += 3500;
            shots++;
        }
        count++;
    }
    if (total_ms > PAD_MAX_CALL_MS)
    {
        tr_error(r, "nothing was pressed: the sequence would take about %llu s, more than %d s; "
                    "split it", (unsigned long long)(total_ms / 1000), PAD_MAX_CALL_MS / 1000);
        goto done;
    }
    if (shots > 8)
    {
        tr_error(r, "nothing was pressed: at most 8 shot steps per call");
        goto done;
    }

    shot_opts_t shot;
    {
        const cJSON *shot_args = arg_get(args, "screenshot");
        if (!shot_opts_parse(cJSON_IsObject(shot_args) ? shot_args : NULL, &shot, r))
            goto done;
    }

    console_lock();
    if (!need_open(r))
    {
        console_unlock();
        goto done;
    }
    buf_t log;
    buf_init(&log);
    const uint64_t start = now_ms();
    for (int i = 0; i < count; i++)
    {
        const step_t *s = &plan[i];
        for (int k = 0; k < s->repeat; k++)
        {
            if (s->kind == STEP_PRESS)
                pad_tap(s->mask, (unsigned)s->ms, (unsigned)gap);
            else if (s->kind == STEP_WAIT)
                sleep_ms((unsigned)s->ms);
            else if (s->kind == STEP_STICK)
            {
                pad_state_t st;
                pad_neutral_state(&st);
                if (s->right)
                {
                    st.rx = s->x;
                    st.ry = s->y;
                }
                else
                {
                    st.lx = s->x;
                    st.ly = s->y;
                }
                pad_set(&st);
                sleep_ms((unsigned)s->ms);
                pad_neutral_state(&st);
                pad_set(&st);
                sleep_ms((unsigned)gap);
            }
            else
            {
                buf_printf(&log, "%6llu ms  shot\n", (unsigned long long)(now_ms() - start));
                shot_take(&shot, r);
                continue;
            }
        }
        if (s->kind != STEP_SHOT)
            buf_printf(&log, "%6llu ms  %s\n", (unsigned long long)(now_ms() - start), s->text);
    }
    tr_text(r, "ran %d steps in %llu ms:\n%s", count, (unsigned long long)(now_ms() - start),
            log.data ? log.data : "");
    buf_free(&log);
    maybe_shot(args, r);
    console_unlock();

done:
    free(plan);
    buf_free(&text);
}

static const tool_def_t k_tools[] = {
    {
        "pad_open",
        "Open virtual controller",
        "Create the virtual DualSense and give it to the foreground user. Needed before any "
        "input and before screenshot mode=system. exclusive=true (default) disconnects the "
        "user's other controllers, because titles read only their user's first controller; the "
        "PS button on a real controller reconnects it. Returns what each system call returned.",
        "{'type':'object','properties':{"
        "'exclusive':{'type':'boolean','default':true,'description':'Disconnect the real "
        "controller so titles read this pad. false keeps it (home screen only).'},"
        "'dismiss_prompt':{'type':'boolean','default':true,'description':'Close the Who is "
        "using this controller prompt with circle.'},"
        "'user_id':{'type':'integer','description':'User to bind to; default the foreground "
        "user.'}}}",
        0,
        tool_pad_open,
    },
    {
        "pad_close",
        "Close virtual controller",
        "Remove the virtual controller. The real controller reconnects with its PS button.",
        "{'type':'object','properties':{}}",
        TOOL_IDEMPOTENT,
        tool_pad_close,
    },
    {
        "pad_status",
        "Controller status",
        "Whether the virtual pad is open, its device id, user, insert counters and the state "
        "currently held.",
        "{'type':'object','properties':{}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_pad_status,
    },
    {
        "pad_press",
        "Press buttons",
        "Press and release one button or a chord, optionally repeated, then optionally take a "
        "screenshot. Buttons: cross, circle, square, triangle, up, down, left, right, l1, r1, "
        "l2, r2, l3, r3, options, create, ps, touchpad.",
        "{'type':'object','properties':{"
        "'buttons':{'description':'A button or a chord: cross, l1+r1, or an array of names.',"
        "'anyOf':[{'type':'string'},{'type':'array','items':{'type':'string'}}]},"
        "'hold_ms':{'type':'integer','default':100,'minimum':16,'maximum':10000},"
        "'repeat':{'type':'integer','default':1,'minimum':1,'maximum':100},"
        "'gap_ms':{'type':'integer','default':150,'description':'Pause after each release.'},"
        "'screenshot_after':{'type':'boolean','default':false},"
        "'settle_ms':{'type':'integer','default':500,'description':'Wait before the screenshot.'},"
        "'screenshot':{'type':'object','description':'Screenshot options (as the screenshot "
        "tool).','properties':{" SHOT_SCHEMA_PROPS "}}},"
        "'required':['buttons']}",
        0,
        tool_pad_press,
    },
    {
        "pad_hold",
        "Hold buttons and sticks",
        "Hold buttons, sticks, triggers and/or a touchpad finger. With duration_ms the state is "
        "held that long and released; without it, it stays held until pad_release or the next "
        "input. Sticks are -1..1 (x right, y down), triggers 0..1.",
        "{'type':'object','properties':{"
        "'buttons':{'type':'string','description':'Chord such as r2 or l1+cross; empty for "
        "none.'},"
        "'left_stick':{'type':'object','properties':{'x':{'type':'number','minimum':-1,"
        "'maximum':1},'y':{'type':'number','minimum':-1,'maximum':1}}},"
        "'right_stick':{'type':'object','properties':{'x':{'type':'number','minimum':-1,"
        "'maximum':1},'y':{'type':'number','minimum':-1,'maximum':1}}},"
        "'l2':{'type':'number','minimum':0,'maximum':1},"
        "'r2':{'type':'number','minimum':0,'maximum':1},"
        "'touch':{'type':'object','description':'One finger on the touchpad (0..1919, 0..1079).',"
        "'properties':{'x':{'type':'integer'},'y':{'type':'integer'}}},"
        "'duration_ms':{'type':'integer','minimum':0,'maximum':120000},"
        "'screenshot_after':{'type':'boolean','default':false},"
        "'settle_ms':{'type':'integer','default':500},"
        "'screenshot':{'type':'object','properties':{" SHOT_SCHEMA_PROPS "}}}}",
        0,
        tool_pad_hold,
    },
    {
        "pad_release",
        "Release everything",
        "Return the virtual pad to neutral: buttons up, sticks centred, triggers at rest.",
        "{'type':'object','properties':{}}",
        TOOL_IDEMPOTENT,
        tool_pad_release,
    },
    {
        "pad_sequence",
        "Run an input sequence",
        "Run presses, stick moves, waits and screenshots in order, timed on the console. The "
        "whole sequence is validated before anything is pressed. Tokens (space separated): "
        "BUTTONS[:hold_ms][*repeat] e.g. cross, l1+r1, cross:300, down*3; wait:MS; "
        "ls:DIRECTION[:ms] / rs:DIRECTION[:ms] with up/down/left/right/upleft/upright/"
        "downleft/downright or X,Y in -1..1; shot (a screenshot, returned as an image). "
        "Example: \"down*2 cross wait:1500 shot\". Max 120 s per call.",
        "{'type':'object','properties':{"
        "'steps':{'description':'The tokens, as one string or an array of strings.',"
        "'anyOf':[{'type':'string'},{'type':'array','items':{'type':'string'}}]},"
        "'hold_ms':{'type':'integer','default':100,'description':'Default press length.'},"
        "'gap_ms':{'type':'integer','default':150,'description':'Pause after each press.'},"
        "'screenshot':{'type':'object','description':'Options for shot steps and "
        "screenshot_after.','properties':{" SHOT_SCHEMA_PROPS "}},"
        "'screenshot_after':{'type':'boolean','default':false},"
        "'settle_ms':{'type':'integer','default':500}},"
        "'required':['steps']}",
        0,
        tool_pad_sequence,
    },
};

void tools_pad_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
