#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

/* ============================================================
   SDPrep – GUI SD/USB Formatter (GTK3)  
   Full stable build with icon support, perception scoring,
   two-partition layout, and crash-proof GdkPixbuf handling.
   ============================================================ */

typedef struct {
    GtkWidget *window;
    GtkWidget *device_combo;
    GtkWidget *label_entry;
    GtkWidget *progress_bar;
    GtkWidget *status_label;
    GtkWidget *format_button;
    GtkWidget *abort_button;
    GtkWidget *refresh_button;
    GtkWidget *restrict_toggle;
    GPid child_pid;
    gboolean formatting;
} AppData;

/* ------------------------------------------------------------
   Show status text
   ------------------------------------------------------------ */
static void set_status(AppData *app, const char *msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
}

/* ------------------------------------------------------------
   Must run as root
   ------------------------------------------------------------ */
static gboolean require_root_dialog(GtkWindow *parent) {
    if (geteuid() == 0) return TRUE;

    GtkWidget *dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "SDPrep must be run as administrator (sudo or pkexec)."
    );
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return FALSE;
}

/* ------------------------------------------------------------
   Read command output safely
   ------------------------------------------------------------ */
static char *read_command_stdout(const char *cmdline) {
    gchar *out = NULL, *err = NULL;
    int status = 0;

    gboolean ok = g_spawn_command_line_sync(
        cmdline, &out, &err, &status, NULL
    );

    if (!ok || status != 0) {
        if (err) {
            g_printerr("Command failed: %s\nError: %s\n", cmdline, err);
            g_free(err);
        }
        if (out) g_free(out);
        return NULL;
    }

    if (err) g_free(err);
    return out;
}

/* ------------------------------------------------------------
   Detect if /dev/<disk> is the root parent device
   ------------------------------------------------------------ */
static gboolean is_root_parent_device(const char *devpath) {
    char *src = read_command_stdout("df -P / | tail -1 | awk '{print $1}'");
    if (!src) return FALSE;

    g_strchomp(src);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "lsblk -no PKNAME %s 2>/dev/null", src);
    g_free(src);

    char *pk = read_command_stdout(cmd);
    if (!pk) return FALSE;

    g_strchomp(pk);
    char parent[256];
    snprintf(parent, sizeof(parent), "/dev/%s", pk);
    g_free(pk);

    return strcmp(parent, devpath) == 0;
}

/* ------------------------------------------------------------
   Avoid system mountpoints
   ------------------------------------------------------------ */
static gboolean device_has_system_mount(json_object *dev) {
    json_object *children = NULL;
    if (!json_object_object_get_ex(dev, "children", &children)) return FALSE;
    if (!json_object_is_type(children, json_type_array)) return FALSE;

    const char *sys[] = {
        "/", "/boot", "/boot/efi", "/usr", "/var",
        "/opt", "/snap", "/recovery", NULL
    };

    int n = json_object_array_length(children);
    for (int i = 0; i < n; i++) {
        json_object *part = json_object_array_get_idx(children, i);
        json_object *mp = NULL;
        if (!json_object_object_get_ex(part, "mountpoint", &mp)) continue;
        if (json_object_is_type(mp, json_type_null)) continue;

        const char *val = json_object_get_string(mp);
        if (!val) continue;

        for (int k = 0; sys[k]; k++) {
            if (strcmp(val, sys[k]) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------
   Perception scoring engine
   ------------------------------------------------------------ */
static int score_device(const char *name,
                        const char *tran,
                        long rm,
                        const char *size)
{
    int score = 0;

    if (name && g_str_has_prefix(name, "mmcblk")) score += 5;
    if (rm == 1) score += 3;

    if (tran) {
        if (strcmp(tran, "mmc") == 0) score += 4;
        if (strcmp(tran, "usb") == 0) score += 3;
    }

    if (size && g_str_has_suffix(size, "G")) {
        double g = atof(size);
        if (g < 512.0) score += 2;
    }

    if (size && strcmp(size, "0B") == 0) score -= 3;
    if (name && g_str_has_prefix(name, "loop")) score -= 10;
    if (name && g_str_has_prefix(name, "zram")) score -= 10;
    if (name && g_str_has_prefix(name, "nvme")) score -= 7;

    return score;
}

/* ------------------------------------------------------------
   Determine candidate disks (Safe or Maybe)
   ------------------------------------------------------------ */
static gboolean is_candidate_disk(json_object *dev,
                                  gboolean restrict_mode,
                                  char *out_path, size_t out_ps,
                                  char *out_desc, size_t out_ds,
                                  int *out_score)
{
    json_object *jname=NULL, *jtype=NULL, *jrm=NULL, *jsize=NULL;
    json_object *jmodel=NULL, *jtran=NULL;

    if (!json_object_object_get_ex(dev, "name", &jname)) return FALSE;
    if (!json_object_object_get_ex(dev, "type", &jtype)) return FALSE;

    if (strcmp(json_object_get_string(jtype), "disk") != 0) return FALSE;

    const char *name  = json_object_get_string(jname);
    const char *model = (json_object_object_get_ex(dev, "model", &jmodel) &&
                         !json_object_is_type(jmodel, json_type_null))
                        ? json_object_get_string(jmodel) : "";
    const char *tran  = (json_object_object_get_ex(dev, "tran", &jtran) &&
                         !json_object_is_type(jtran, json_type_null))
                        ? json_object_get_string(jtran) : "";

    long rm = (json_object_object_get_ex(dev, "rm", &jrm))
              ? json_object_get_int(jrm)
              : 0;

    const char *size = (json_object_object_get_ex(dev, "size", &jsize) &&
                        !json_object_is_type(jsize, json_type_null))
                        ? json_object_get_string(jsize) : "";

    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);

    if (is_root_parent_device(path)) return FALSE;
    if (device_has_system_mount(dev)) return FALSE;

    int score = score_device(name, tran, rm, size);

    if (restrict_mode && g_str_has_suffix(size, "T"))
        return FALSE;

    if (score <= 0) return FALSE;

    if (out_score) *out_score = score;
    if (out_path)  g_strlcpy(out_path, path, out_ps);

    if (out_desc)
        g_snprintf(out_desc, out_ds, "%s  %s  [%s]",
                   path,
                   (model && *model) ? model : "Removable",
                   size && *size ? size : "unknown");

    return TRUE;
}

/* ------------------------------------------------------------
   Populate device dropdown
   ------------------------------------------------------------ */
static gboolean populate_devices(AppData *app) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device_combo));

    char *js = read_command_stdout(
        "lsblk -J -o NAME,RM,SIZE,MODEL,TRAN,TYPE,MOUNTPOINT"
    );
    if (!js) {
        set_status(app, "Failed: lsblk did not return data.");
        return FALSE;
    }

    json_object *root = json_tokener_parse(js);
    g_free(js);

    if (!root) {
        set_status(app, "JSON parse error.");
        return FALSE;
    }

    json_object *arr = NULL;
    if (!json_object_object_get_ex(root, "blockdevices", &arr)) {
        json_object_put(root);
        return FALSE;
    }

    gboolean restrict_mode =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->restrict_toggle));

    int n = json_object_array_length(arr);
    int added = 0;

    for (int i = 0; i < n; i++) {
        json_object *dev = json_object_array_get_idx(arr, i);

        char path[128], desc[256];
        int score = 0;

        if (is_candidate_disk(dev, restrict_mode,
                              path, sizeof(path),
                              desc, sizeof(desc),
                              &score))
        {
            char id[160];
            char grade = (score >= 5) ? 'S' : 'M';

            snprintf(id, sizeof(id), "%c:%s", grade, path);

            gtk_combo_box_text_append(
                GTK_COMBO_BOX_TEXT(app->device_combo),
                id, desc
            );
            added++;
        }
    }

    json_object_put(root);

    if (added == 0) {
        gtk_combo_box_text_append(
            GTK_COMBO_BOX_TEXT(app->device_combo),
            "",
            "— No safe removable media detected —"
        );
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
        return FALSE;
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    return TRUE;
}

/* ------------------------------------------------------------
   Progress bar animation
   ------------------------------------------------------------ */
static gboolean update_progress_cb(gpointer data) {
    AppData *app = data;
    if (!app->formatting) return G_SOURCE_REMOVE;

    double p = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(app->progress_bar));
    p += 0.02;
    if (p > 0.95) p = 0.95;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), p);
    return TRUE;
}

/* ------------------------------------------------------------
   Child exit handler
   ------------------------------------------------------------ */
static void child_watch_cb(GPid pid, gint status, gpointer data) {
    AppData *app = data;

    app->formatting = FALSE;
    gtk_widget_set_sensitive(app->format_button, TRUE);
    gtk_widget_set_sensitive(app->abort_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, TRUE);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        set_status(app, "Format completed.");
    } else {
        set_status(app, "Format failed or aborted.");
    }

    g_spawn_close_pid(pid);
}

/* ------------------------------------------------------------
   Abort
   ------------------------------------------------------------ */
static void on_abort_clicked(GtkButton *btn, AppData *app) {
    if (app->formatting && app->child_pid > 0) {
        kill(app->child_pid, SIGTERM);
        set_status(app, "Aborting…");
    }
}

/* ------------------------------------------------------------
   Format device
   ------------------------------------------------------------ */
static void on_format_clicked(GtkButton *btn, AppData *app) {
    const gchar *raw_id_c = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device_combo));
    if (!raw_id_c) {
        set_status(app, "Select a valid removable device.");
        return;
    }

    gchar *raw_id = g_strdup(raw_id_c);
    char cls = raw_id[0];
    const char *devpath = raw_id + 2;

    if (cls == 'M') {
        GtkWidget *w = gtk_message_dialog_new(
            GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK_CANCEL,
            "This device may be a portable HDD or SSD.\nProceed?"
        );
        gint r = gtk_dialog_run(GTK_DIALOG(w));
        gtk_widget_destroy(w);
        if (r != GTK_RESPONSE_OK) {
            g_free(raw_id);
            return;
        }
    }

    /* Confirm erase */
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
        "This will ERASE ALL DATA on:\n\n  %s\n\nProceed?", devpath);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp != GTK_RESPONSE_OK) {
        g_free(raw_id);
        return;
    }

    /* Build command */
    const char *label = gtk_entry_get_text(GTK_ENTRY(app->label_entry));
    if (!label || !*label) label = "MICROPYTHON";

    gchar *qdev = g_shell_quote(devpath);
    gchar *qlabel = g_shell_quote(label);

    gchar *cmd = g_strdup_printf(
        "bash -c '"
        "set -e; "
        "dev=%s; "
        "wipefs -a \"$dev\"; "
        "parted -s \"$dev\" mklabel msdos; "

        "bytes=$(lsblk -nbdo SIZE \"$dev\"); "
        "mib=$((bytes/1024/1024)); "
        "end1=$((mib-32)); "
        "if [ $end1 -le 8 ]; then exit 1; fi; "

        "parted -s \"$dev\" mkpart primary fat32 1MiB ${end1}MiB; "
        "parted -s \"$dev\" mkpart primary ${end1}MiB 100%%; "

        "partprobe \"$dev\"; udevadm settle; "

        "if echo \"$dev\" | grep -Eq \"[0-9]$\"; then P1=\"${dev}p1\"; else P1=\"${dev}1\"; fi; "
        "mkfs.fat -F32 -I -n %s \"$P1\"; "
        "'", qdev, qlabel
    );

    g_free(qdev);
    g_free(qlabel);
    g_free(raw_id);

    gtk_widget_set_sensitive(app->format_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, FALSE);
    gtk_widget_set_sensitive(app->abort_button, TRUE);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    set_status(app, "Formatting…");
    app->formatting = TRUE;

    GError *err = NULL;
    gboolean ok = g_spawn_async(
        NULL,
        (gchar *[]){ "/bin/bash", "-c", cmd, NULL },
        NULL,
        G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &app->child_pid,
        &err
    );
    g_free(cmd);

    if (!ok) {
        set_status(app, err ? err->message : "Failed to start process.");
        if (err) g_error_free(err);
        return;
    }

    g_child_watch_add(app->child_pid, child_watch_cb, app);
    g_timeout_add(200, update_progress_cb, app);
}

/* ------------------------------------------------------------
   GTK UI setup
   ------------------------------------------------------------ */
static void activate(GtkApplication *gapp, gpointer user_data) {
    if (!require_root_dialog(NULL)) {
        g_application_quit(G_APPLICATION(gapp));
        return;
    }

    AppData *app = g_new0(AppData, 1);

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "SDPrep");
    gtk_window_set_default_size(GTK_WINDOW(win), 540, 320);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(win), outer);
    gtk_widget_set_margin_start(outer, 12);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_set_margin_top(outer, 12);
    gtk_widget_set_margin_bottom(outer, 12);

    /* Header */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(outer), head, FALSE, FALSE, 0);

    /* Load icon (safe mode) */
    GtkWidget *icon = NULL;
    {
        GError *img_err = NULL;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(
            "icons/sdprep.png", 48, 48, TRUE, &img_err
        );
        if (pb) {
            icon = gtk_image_new_from_pixbuf(pb);
            gtk_window_set_icon(GTK_WINDOW(win), pb);
            g_object_unref(pb);
        } else {
            g_printerr("Icon load error: %s\n",
                       img_err ? img_err->message : "unknown");
            if (img_err) g_error_free(img_err);
            icon = gtk_image_new();
        }
        gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
    }

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(title),
        "<span weight='bold' size='large'>SD / USB Prep</span>"
    );
    gtk_box_pack_start(GTK_BOX(head), title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(outer), grid, TRUE, TRUE, 0);

    /* Device */
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_label_new("Device:"), 0, 0, 1, 1);

    app->device_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(app->device_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->device_combo, 1, 0, 3, 1);

    app->restrict_toggle =
        gtk_check_button_new_with_label(
            "Prefer SD/microSD & USB flash (avoid HDDs)"
        );
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->restrict_toggle), TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->restrict_toggle, 1, 1, 3, 1);

    /* Label */
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_label_new("Volume label:"), 0, 2, 1, 1);
    app->label_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->label_entry), "MICROPYTHON");
    gtk_grid_attach(GTK_GRID(grid), app->label_entry, 1, 2, 3, 1);

    /* Progress */
    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->progress_bar, 0, 3, 4, 1);

    /* Buttons */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    app->format_button  = gtk_button_new_with_label("Format");
    app->abort_button   = gtk_button_new_with_label("Abort");
    app->refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *quitbtn  = gtk_button_new_with_label("Quit");

    gtk_box_pack_start(GTK_BOX(row), app->format_button,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), app->abort_button,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), app->refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), quitbtn,             FALSE, FALSE, 0);

    gtk_widget_set_sensitive(app->abort_button, FALSE);

    /* Status */
    app->status_label = gtk_label_new("Ready.");
    gtk_box_pack_start(GTK_BOX(outer), app->status_label, FALSE, FALSE, 0);

    /* Connect signals */
    g_signal_connect(app->format_button, "clicked",
                     G_CALLBACK(on_format_clicked), app);
    g_signal_connect(app->abort_button, "clicked",
                     G_CALLBACK(on_abort_clicked), app);
    g_signal_connect_swapped(app->refresh_button, "clicked",
                             G_CALLBACK(populate_devices), app);
    g_signal_connect_swapped(quitbtn, "clicked",
                             G_CALLBACK(gtk_widget_destroy), win);

    app->window = win;

    populate_devices(app);
    gtk_widget_show_all(win);
}

/* ------------------------------------------------------------
   Main
   ------------------------------------------------------------ */
int main(int argc, char **argv) {
    GtkApplication *app =
        gtk_application_new("com.drflores.sdprep", 0);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

