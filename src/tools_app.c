/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "backend.h"
#include "mcp.h"
#include "util.h"

static bool is_title_id(const char *s)
{
    if (strlen(s) != 9)
        return false;
    for (int i = 0; i < 4; i++)
        if (!isupper((unsigned char)s[i]))
            return false;
    for (int i = 4; i < 9; i++)
        if (!isdigit((unsigned char)s[i]))
            return false;
    return true;
}

/* The title's name from its param.json, in the default language. */
static void title_name(const char *tid, char *out, size_t len)
{
    out[0] = '\0';
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/param.json", backend_appmeta_dir, tid);
    uint8_t *data;
    size_t n;
    if (read_file(path, &data, &n, 4u * 1024u * 1024u) != 0)
        return;
    cJSON *json = cJSON_ParseWithLength((const char *)data, n);
    free(data);
    if (json == NULL)
        return;
    const cJSON *loc = cJSON_GetObjectItem(json, "localizedParameters");
    const char *lang = arg_str(loc, "defaultLanguage", "en-US");
    const cJSON *entry = cJSON_GetObjectItem(loc, lang);
    const char *name = arg_str(entry, "titleName", NULL);
    if (name == NULL && cJSON_IsObject(loc))
    {
        const cJSON *it;
        cJSON_ArrayForEach(it, loc) if ((name = arg_str(it, "titleName", NULL)) != NULL) break;
    }
    if (name == NULL)
        name = arg_str(json, "titleName", "");
    snprintf(out, len, "%s", name);
    cJSON_Delete(json);
}

static void tool_app_list(const cJSON *args, tool_result_t *r)
{
    const char *filter = arg_str(args, "filter", NULL);
    app_running_t running = {0};
    const bool have_running = backend_app_running(&running) == 0;
    buf_t out;
    buf_init(&out);
    int count = 0;
    char seen[2048][10];
    int seen_n = 0;
    for (size_t d = 0; d < backend_app_dir_count; d++)
    {
        DIR *dir = opendir(backend_app_dirs[d]);
        if (dir == NULL)
            continue;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
        {
            if (!is_title_id(e->d_name))
                continue;
            bool dup = false;
            for (int i = 0; i < seen_n && !dup; i++)
                dup = strcmp(seen[i], e->d_name) == 0;
            if (dup)
                continue;
            if (seen_n < (int)ARRAY_LEN(seen))
                snprintf(seen[seen_n++], sizeof(seen[0]), "%.9s", e->d_name);
            char name[256];
            title_name(e->d_name, name, sizeof(name));
            if (filter && filter[0] && strcasestr(e->d_name, filter) == NULL &&
                strcasestr(name, filter) == NULL)
                continue;
            const bool is_running = have_running && strcmp(running.title_id, e->d_name) == 0;
            buf_printf(&out, "%s  %-40s %s%s\n", e->d_name, name[0] ? name : "?",
                       backend_app_dirs[d], is_running ? "  [running]" : "");
            count++;
        }
        closedir(dir);
    }
    if (have_running)
        buf_printf(&out, "running: %s (app id 0x%08x)\n", running.title_id,
                   (unsigned)running.app_id);
    else
        buf_puts(&out, "running: no title\n");
    tr_text(r, "%d titles\n%s", count, out.data ? out.data : "");
    buf_free(&out);
}

static void tool_app_launch(const cJSON *args, tool_result_t *r)
{
    const char *tid = arg_str(args, "title_id", NULL);
    if (tid == NULL || !is_title_id(tid))
    {
        tr_error(r, "title_id must look like PPSA01234 (see app_list)");
        return;
    }
    app_running_t running = {0};
    if (backend_app_running(&running) == 0 && !arg_bool(args, "replace_running", false))
    {
        tr_error(r, "%s is running (app id 0x%08x). Pass replace_running=true to close it "
                    "first, or quit it through its own menu.",
                 running.title_id, (unsigned)running.app_id);
        return;
    }
    cJSON *report = cJSON_CreateObject();
    const int rc = backend_app_launch(tid, arg_str(args, "args", NULL), report);
    if (rc < 0)
    {
        r->is_error = true;
        tr_text(r, "launch of %s failed", tid);
    }
    else
        tr_text(r, "launch of %s requested. It takes a few seconds; take a screenshot or "
                   "klog_read with until to see it arrive.", tid);
    tr_json(r, report);
}

static void tool_app_kill(const cJSON *args, tool_result_t *r)
{
    int app_id = arg_int(args, "app_id", 0);
    app_running_t running = {0};
    if (app_id == 0)
    {
        if (backend_app_running(&running) != 0)
        {
            tr_error(r, "no title is running");
            return;
        }
        app_id = running.app_id;
    }
    cJSON *report = cJSON_CreateObject();
    const int rc = backend_app_kill(app_id, report);
    if (rc != 0)
    {
        r->is_error = true;
        tr_text(r, "closing app 0x%08x failed", (unsigned)app_id);
    }
    else
        tr_text(r, "closed app 0x%08x %s", (unsigned)app_id, running.title_id);
    tr_json(r, report);
}

static const tool_def_t k_tools[] = {
    {
        "app_list",
        "List installed titles",
        "Installed titles (games, apps, homebrew) with their names, and which one is running.",
        "{'type':'object','properties':{'filter':{'type':'string','description':'Substring of "
        "the title id or name.'}}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_app_list,
    },
    {
        "app_launch",
        "Launch a title",
        "Launch an installed title by its title id (sceSystemServiceLaunchApp, as the "
        "ps5-payload-dev websrv does). Refuses while another title runs unless "
        "replace_running=true.",
        "{'type':'object','properties':{'title_id':{'type':'string','pattern':'^[A-Z]{4}[0-9]{5}$'},"
        "'args':{'type':'string','description':'Space separated arguments for the title.'},"
        "'replace_running':{'type':'boolean','default':false}},'required':['title_id']}",
        0,
        tool_app_launch,
    },
    {
        "app_kill",
        "Close a title",
        "Close the running title (or the given app id) through the system service, as the "
        "home screen Close Game does. Unsaved progress is lost.",
        "{'type':'object','properties':{'app_id':{'type':'integer','description':'Default: the "
        "running title.'}}}",
        TOOL_DESTRUCTIVE,
        tool_app_kill,
    },
};

void tools_app_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
