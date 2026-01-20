#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

/* ============================================================
   SDPrep – microSD FAT32 Prep (GTK3) — SD CARD ONLY
   - SD/microSD detection
   - Auto-unmount (best effort)
   - pkexec absolute path
   - FIX: Safety check accepts disk OR rom and logs lsblk values
   ============================================================ */

typedef struct {
    GtkWidget *window;
    GtkWidget *device_combo;
    GtkWidget *label_entry;
    GtkWidget *progress_bar;
    GtkWidget *status_label;
    GtkWidget *details_view;
    GtkTextBuffer *details_buf;

    GtkWidget *format_button;
    GtkWidget *abort_button;
    GtkWidget *refresh_button;

    GPid child_pid;
    gint out_fd;
    gint err_fd;
    GIOChannel *out_ch;
    GIOChannel *err_ch;
    guint out_watch;
    guint err_watch;

    guint pulse_timer;
    gboolean formatting;
} AppData;

static void set_status(AppData *app, const char *msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg ? msg : "");
}
static void details_clear(AppData *app) {
    gtk_text_buffer_set_text(app->details_buf, "", -1);
}
static void details_append(AppData *app, const char *text) {
    if (!text) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->details_buf, &end);
    gtk_text_buffer_insert(app->details_buf, &end, text, -1);
    gtk_text_buffer_insert(app->details_buf, &end, "\n", 1);
}

static const char *find_pkexec(void) {
    if (access("/usr/bin/pkexec", X_OK) == 0) return "/usr/bin/pkexec";
    if (access("/bin/pkexec", X_OK) == 0) return "/bin/pkexec";
    return NULL;
}

static char *run_capture(const char *cmdline, char **out_err) {
    gchar *out = NULL, *err = NULL;
    int status = 0;
    gboolean ok = g_spawn_command_line_sync(cmdline, &out, &err, &status, NULL);
    if (out_err) *out_err = NULL;

    if (!ok || status != 0) {
        if (out_err && err) *out_err = g_strdup(err);
        g_free(out);
        g_free(err);
        return NULL;
    }
    if (out_err && err && *err) *out_err = g_strdup(err);
    g_free(err);
    return out;
}

static void sanitize_fat_label(const char *in, char out[12]) {
    const char *fallback = "MICROPYTHON";
    if (!in || !*in) in = fallback;

    char tmp[128];
    size_t j = 0;

    for (size_t i = 0; in[i] && j < sizeof(tmp) - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        c = (unsigned char)toupper(c);

        if ((c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == ' ') {
            tmp[j++] = (char)c;
        }
    }
    tmp[j] = 0;

    char tmp2[128];
    size_t k = 0;
    gboolean prev_space = FALSE;

    for (size_t i = 0; tmp[i] && k < sizeof(tmp2) - 1; i++) {
        char c = tmp[i];
        if (c == ' ') {
            if (k == 0) continue;
            if (prev_space) continue;
            prev_space = TRUE;
            tmp2[k++] = c;
        } else {
            prev_space = FALSE;
            tmp2[k++] = c;
        }
    }
    while (k > 0 && tmp2[k - 1] == ' ') k--;
    tmp2[k] = 0;

    if (k == 0) g_strlcpy(tmp2, fallback, sizeof(tmp2));

    memset(out, 0, 12);
    g_strlcpy(out, tmp2, 12);
}

static gboolean str_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return FALSE;
    char *h = g_ascii_strdown(hay, -1);
    char *n = g_ascii_strdown(needle, -1);
    gboolean ok = (strstr(h, n) != NULL);
    g_free(h); g_free(n);
    return ok;
}

static gboolean looks_like_sd_device(const char *name,
                                     const char *tran,
                                     const char *model,
                                     long rm,
                                     const char *size) {
    if (!name) return FALSE;
    if (!size || !*size || strcmp(size, "0B") == 0) return FALSE;

    if (g_str_has_prefix(name, "mmcblk")) return TRUE;
    if (tran && strcmp(tran, "mmc") == 0) return TRUE;

    if (tran && strcmp(tran, "usb") == 0 && rm == 1) {
        if (model && *model) {
            if (str_contains_ci(model, "sd") ||
                str_contains_ci(model, "card") ||
                str_contains_ci(model, "reader") ||
                str_contains_ci(model, "massstorageclass") ||
                str_contains_ci(model, "generic")) {
                return TRUE;
            }
        } else {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean auto_unmount_partitions(AppData *app, const char *disk) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "lsblk -nrpo NAME,TYPE,MOUNTPOINT %s", disk);

        char *err = NULL;
        char *out = run_capture(cmd, &err);
        if (!out) {
            details_append(app, "Auto-unmount: lsblk failed.");
            if (err) { details_append(app, err); g_free(err); }
            return FALSE;
        }
        if (err) g_free(err);

        gboolean any = FALSE;
        gchar **lines = g_strsplit(out, "\n", -1);
        g_free(out);

        for (int i = 0; lines[i]; i++) {
            char name[256] = {0}, type[64] = {0}, mp[256] = {0};
            int n = sscanf(lines[i], "%255s %63s %255[^\n]", name, type, mp);
            if (n < 2) continue;
            if (strcmp(type, "part") != 0) continue;
            if (n == 2 || !mp[0]) continue;

            any = TRUE;
            details_append(app, "Auto-unmount:");
            details_append(app, name);

            char um1[512];
            snprintf(um1, sizeof(um1), "udisksctl unmount -b %s >/dev/null 2>&1", name);
            system(um1);

            char um2[512];
            snprintf(um2, sizeof(um2), "umount %s >/dev/null 2>&1", name);
            system(um2);
        }

        g_strfreev(lines);
        if (!any) return TRUE;

        usleep(250 * 1000);
    }
    return TRUE;
}

static gboolean populate_devices(AppData *app) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device_combo));
    details_clear(app);

    const char *cmd = "lsblk -J -o NAME,RM,SIZE,MODEL,TRAN,TYPE,MOUNTPOINT,RO";

    char *err = NULL;
    char *js = run_capture(cmd, &err);
    if (!js) {
        set_status(app, "Failed: lsblk did not return data.");
        if (err) { details_append(app, "lsblk error:"); details_append(app, err); g_free(err); }
        return FALSE;
    }
    if (err) g_free(err);

    json_object *root = json_tokener_parse(js);
    g_free(js);
    if (!root) {
        set_status(app, "JSON parse error from lsblk.");
        return FALSE;
    }

    json_object *arr = NULL;
    if (!json_object_object_get_ex(root, "blockdevices", &arr) ||
        !json_object_is_type(arr, json_type_array)) {
        json_object_put(root);
        set_status(app, "Unexpected lsblk JSON.");
        return FALSE;
    }

    int added = 0;
    int n = json_object_array_length(arr);

    for (int i = 0; i < n; i++) {
        json_object *dev = json_object_array_get_idx(arr, i);

        json_object *jname=NULL,*jtype=NULL,*jrm=NULL,*jsize=NULL,*jmodel=NULL,*jtran=NULL,*jro=NULL;
        if (!json_object_object_get_ex(dev, "name", &jname)) continue;
        if (!json_object_object_get_ex(dev, "type", &jtype)) continue;
        if (strcmp(json_object_get_string(jtype), "disk") != 0) continue;

        const char *name = json_object_get_string(jname);
        long rm = json_object_object_get_ex(dev,"rm",&jrm) ? json_object_get_int(jrm) : 0;
        long ro = json_object_object_get_ex(dev,"ro",&jro) ? json_object_get_int(jro) : 0;

        const char *size = (json_object_object_get_ex(dev,"size",&jsize) && !json_object_is_type(jsize,json_type_null))
                           ? json_object_get_string(jsize) : "";
        const char *model = (json_object_object_get_ex(dev,"model",&jmodel) && !json_object_is_type(jmodel,json_type_null))
                            ? json_object_get_string(jmodel) : "";
        const char *tran = (json_object_object_get_ex(dev,"tran",&jtran) && !json_object_is_type(jtran,json_type_null))
                           ? json_object_get_string(jtran) : "";

        if (ro == 1) continue;
        if (!looks_like_sd_device(name, tran, model, rm, size)) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", name);

        gboolean mounted = FALSE;
        json_object *children = NULL;
        if (json_object_object_get_ex(dev, "children", &children) &&
            json_object_is_type(children, json_type_array)) {
            int cn = json_object_array_length(children);
            for (int k = 0; k < cn; k++) {
                json_object *p = json_object_array_get_idx(children, k);
                json_object *mp = NULL;
                if (json_object_object_get_ex(p, "mountpoint", &mp) &&
                    !json_object_is_type(mp, json_type_null)) {
                    const char *mps = json_object_get_string(mp);
                    if (mps && *mps) { mounted = TRUE; break; }
                }
            }
        }

        char desc[256];
        g_snprintf(desc, sizeof(desc), "%s  %s  [%s]  (tran=%s rm=%ld)%s",
                   path,
                   (model && *model) ? model : "SD",
                   (size && *size) ? size : "unknown",
                   (tran && *tran) ? tran : "unknown",
                   rm,
                   mounted ? "  [mounted]" : "");

        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->device_combo), path, desc);
        added++;
        details_append(app, desc);
    }

    json_object_put(root);

    if (added == 0) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->device_combo), "", "— No SD/microSD detected —");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
        set_status(app, "No SD/microSD detected. Insert card and Refresh.");
        return FALSE;
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    set_status(app, "Ready.");
    return TRUE;
}

static gboolean pulse_cb(gpointer data) {
    AppData *app = (AppData*)data;
    if (!app->formatting) return G_SOURCE_REMOVE;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress_bar));
    return TRUE;
}

static gboolean io_watch_cb(GIOChannel *ch, GIOCondition cond, gpointer data) {
    AppData *app = (AppData*)data;
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) return G_SOURCE_REMOVE;

    gchar *line = NULL;
    gsize len = 0;
    GError *err = NULL;

    GIOStatus st = g_io_channel_read_line(ch, &line, &len, NULL, &err);
    if (st == G_IO_STATUS_NORMAL && line) {
        g_strchomp(line);
        if (*line) details_append(app, line);
        g_free(line);
        return TRUE;
    }

    if (err) g_error_free(err);
    g_free(line);
    return TRUE;
}

static void cleanup_child_io(AppData *app) {
    if (app->out_watch) { g_source_remove(app->out_watch); app->out_watch = 0; }
    if (app->err_watch) { g_source_remove(app->err_watch); app->err_watch = 0; }

    if (app->out_ch) { g_io_channel_unref(app->out_ch); app->out_ch = NULL; }
    if (app->err_ch) { g_io_channel_unref(app->err_ch); app->err_ch = NULL; }

    if (app->out_fd >= 0) { close(app->out_fd); app->out_fd = -1; }
    if (app->err_fd >= 0) { close(app->err_fd); app->err_fd = -1; }
}

static void child_watch_cb(GPid pid, gint status, gpointer data) {
    AppData *app = (AppData*)data;

    app->formatting = FALSE;

    if (app->pulse_timer) {
        g_source_remove(app->pulse_timer);
        app->pulse_timer = 0;
    }

    gtk_widget_set_sensitive(app->format_button, TRUE);
    gtk_widget_set_sensitive(app->abort_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, TRUE);

    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        set_status(app, "Format completed (FAT32 created).");
    } else {
        set_status(app, "Format failed or canceled (see log).");
    }

    cleanup_child_io(app);
    g_spawn_close_pid(pid);
    app->child_pid = 0;
}

static void on_abort_clicked(GtkButton *btn, AppData *app) {
    (void)btn;
    if (app->formatting && app->child_pid > 0) {
        set_status(app, "Aborting…");
        kill(app->child_pid, SIGTERM);
    }
}

static gboolean spawn_privileged_pkexec(AppData *app, const char *bash_script) {
    const char *pkexec = find_pkexec();
    if (!pkexec) {
        set_status(app, "pkexec not found. Install policykit-1.");
        details_append(app, "ERROR: pkexec not found at /usr/bin/pkexec");
        return FALSE;
    }

    GError *err = NULL;
    gint out_fd = -1, err_fd = -1;

    gchar *argv[] = {
        (gchar*)pkexec,
        "/bin/bash",
        "-c",
        (gchar*)bash_script,
        NULL
    };

    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &app->child_pid,
        NULL, &out_fd, &err_fd,
        &err
    );

    if (!ok) {
        set_status(app, err ? err->message : "Failed to start pkexec.");
        if (err) g_error_free(err);
        return FALSE;
    }

    app->out_fd = out_fd;
    app->err_fd = err_fd;

    app->out_ch = g_io_channel_unix_new(app->out_fd);
    app->err_ch = g_io_channel_unix_new(app->err_fd);
    g_io_channel_set_encoding(app->out_ch, NULL, NULL);
    g_io_channel_set_encoding(app->err_ch, NULL, NULL);
    g_io_channel_set_flags(app->out_ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_flags(app->err_ch, G_IO_FLAG_NONBLOCK, NULL);

    app->out_watch = g_io_add_watch(app->out_ch, G_IO_IN | G_IO_HUP | G_IO_ERR, io_watch_cb, app);
    app->err_watch = g_io_add_watch(app->err_ch, G_IO_IN | G_IO_HUP | G_IO_ERR, io_watch_cb, app);

    g_child_watch_add(app->child_pid, child_watch_cb, app);
    return TRUE;
}

static void on_format_clicked(GtkButton *btn, AppData *app) {
    (void)btn;

    const gchar *devpath = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device_combo));
    if (!devpath || devpath[0] == '\0') {
        set_status(app, "Select an SD/microSD device.");
        return;
    }

    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
        "This will ERASE ALL DATA on:\n\n  %s\n\nProceed?", devpath);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) return;

    details_append(app, "Pre-step: auto-unmount mounted partitions (if any)...");
    (void)auto_unmount_partitions(app, devpath);

    char label11[12];
    sanitize_fat_label(gtk_entry_get_text(GTK_ENTRY(app->label_entry)), label11);

    gchar *qdev = g_shell_quote(devpath);
    gchar *qlabel = g_shell_quote(label11);

    /* FIX: trim whitespace from dtype/dro */
    gchar *script = g_strdup_printf(
        "set -euo pipefail; "
        "dev=%s; "
        "echo \"[1/7] Safety check...\"; "
        "dtype=$(lsblk -no TYPE \"$dev\" 2>/dev/null | head -n1 | tr -d ' \\t\\r\\n' || true); "
        "dro=$(lsblk -no RO   \"$dev\" 2>/dev/null | head -n1 | tr -d ' \\t\\r\\n' || true); "
        "echo \"    lsblk TYPE=$dtype\"; "
        "echo \"    lsblk RO=$dro\"; "
        "if [ \"$dtype\" != \"disk\" ] && [ \"$dtype\" != \"rom\" ]; then "
        "  echo \"ERROR: unexpected TYPE ($dtype).\"; "
        "  lsblk -o NAME,TYPE,RM,RO,SIZE,MODEL,TRAN,MOUNTPOINT \"$dev\" || true; "
        "  exit 1; "
        "fi; "
        "if [ -n \"$dro\" ] && [ \"$dro\" != \"0\" ]; then "
        "  echo \"ERROR: device is read-only (RO=$dro).\"; "
        "  exit 1; "
        "fi; "
        ""
        "unmount_all(){ "
        "  for p in $(lsblk -nrpo NAME,TYPE \"$dev\" | awk '$2==\"part\"{print $1}'); do "
        "    udisksctl unmount -b \"$p\" >/dev/null 2>&1 || true; "
        "    umount \"$p\" >/dev/null 2>&1 || true; "
        "  done; "
        "}; "
        "echo \"    -> ensuring unmounted...\"; "
        "for i in 1 2 3; do unmount_all; udevadm settle >/dev/null 2>&1 || true; sleep 0.2; done; "
        "mp=$(lsblk -nrpo NAME,MOUNTPOINT \"$dev\" | awk '$2!=\"\"{print}'); "
        "if [ -n \"$mp\" ]; then "
        "  echo \"ERROR: still mounted:\"; echo \"$mp\"; "
        "  exit 1; "
        "fi; "
        ""
        "echo \"[2/7] wipefs...\"; "
        "wipefs -a \"$dev\"; "
        "echo \"[3/7] partition table...\"; "
        "parted -s \"$dev\" mklabel msdos; "
        "bytes=$(lsblk -nbdo SIZE \"$dev\"); "
        "mib=$((bytes/1024/1024)); "
        "end1=$((mib-32)); "
        "if [ $end1 -le 64 ]; then echo \"Device too small\"; exit 1; fi; "
        "echo \"[4/7] create partitions...\"; "
        "parted -s \"$dev\" mkpart primary fat32 1MiB ${end1}MiB; "
        "parted -s \"$dev\" mkpart primary ${end1}MiB 100%%; "
        "echo \"[5/7] settle...\"; "
        "partprobe \"$dev\"; udevadm settle; "
        "if echo \"$dev\" | grep -Eq \"[0-9]$\"; then p1=\"${dev}p1\"; else p1=\"${dev}1\"; fi; "
        "echo \"[6/7] mkfs.fat...\"; "
        "mkfs.fat -F32 -n %s \"$p1\"; "
        "echo \"[7/7] sync...\"; "
        "sync; echo DONE;",
        qdev, qlabel
    );

    g_free(qdev);
    g_free(qlabel);

    gtk_widget_set_sensitive(app->format_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, FALSE);
    gtk_widget_set_sensitive(app->abort_button, TRUE);

    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "Working…");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    details_append(app, "Starting privileged formatter (pkexec)...");
    set_status(app, "Formatting…");

    app->formatting = TRUE;
    app->pulse_timer = g_timeout_add(120, pulse_cb, app);

    if (!spawn_privileged_pkexec(app, script)) {
        app->formatting = FALSE;
        if (app->pulse_timer) { g_source_remove(app->pulse_timer); app->pulse_timer = 0; }
        gtk_widget_set_sensitive(app->format_button, TRUE);
        gtk_widget_set_sensitive(app->refresh_button, TRUE);
        gtk_widget_set_sensitive(app->abort_button, FALSE);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "");
        g_free(script);
        return;
    }

    g_free(script);
}

static void on_refresh(GtkButton *btn, AppData *app) {
    (void)btn;
    populate_devices(app);
}

static void on_destroy(GtkWidget *w, AppData *app) {
    (void)w;
    if (!app) return;
    if (app->formatting && app->child_pid > 0) kill(app->child_pid, SIGTERM);
    cleanup_child_io(app);
    g_free(app);
}

static void activate(GtkApplication *gapp, gpointer user_data) {
    (void)user_data;

    AppData *app = g_new0(AppData, 1);
    app->child_pid = 0;
    app->out_fd = app->err_fd = -1;

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "SDPrep");
    gtk_window_set_default_size(GTK_WINDOW(win), 860, 560);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(win), outer);
    gtk_widget_set_margin_start(outer, 12);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_set_margin_top(outer, 12);
    gtk_widget_set_margin_bottom(outer, 12);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span weight='bold' size='x-large'>SDPrep</span>\n"
        "<span size='small'>SD/microSD FAT32 prep — SD CARD ONLY</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(outer), title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(outer), grid, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("SD Device:"), 0, 0, 1, 1);

    app->device_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(app->device_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->device_combo, 1, 0, 3, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("FAT32 label:"), 0, 1, 1, 1);
    app->label_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->label_entry), "MICROPYTHON");
    gtk_grid_attach(GTK_GRID(grid), app->label_entry, 1, 1, 3, 1);

    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->progress_bar, 0, 2, 4, 1);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    app->format_button  = gtk_button_new_with_label("Format FAT32");
    app->abort_button   = gtk_button_new_with_label("Abort");
    app->refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *quitbtn  = gtk_button_new_with_label("Quit");

    gtk_box_pack_start(GTK_BOX(row), app->format_button,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), app->abort_button,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), app->refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), quitbtn,             FALSE, FALSE, 0);

    gtk_widget_set_sensitive(app->abort_button, FALSE);

    app->status_label = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(outer), app->status_label, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Log / Details");
    gtk_box_pack_start(GTK_BOX(outer), frame, TRUE, TRUE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), sc);

    app->details_buf = gtk_text_buffer_new(NULL);
    app->details_view = gtk_text_view_new_with_buffer(app->details_buf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->details_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->details_view), TRUE);
    gtk_container_add(GTK_CONTAINER(sc), app->details_view);

    g_signal_connect(app->format_button, "clicked", G_CALLBACK(on_format_clicked), app);
    g_signal_connect(app->abort_button, "clicked", G_CALLBACK(on_abort_clicked), app);
    g_signal_connect(app->refresh_button, "clicked", G_CALLBACK(on_refresh), app);
    g_signal_connect_swapped(quitbtn, "clicked", G_CALLBACK(gtk_widget_destroy), win);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), app);

    app->window = win;

    populate_devices(app);
    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.drflores.sdprep", 0);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
