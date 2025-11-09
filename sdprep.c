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

/* ---------- Utilities ---------- */
static void set_status(AppData *app, const char *msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
}

static gboolean require_root_dialog(GtkWindow *parent) {
    if (geteuid() == 0) return TRUE;
    GtkWidget *dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "sdprep must be run as root (sudo).");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return FALSE;
}

static char *read_command_stdout(const char *cmdline) {
    gchar *out = NULL, *err = NULL;
    int status = 0;
    gboolean ok = g_spawn_command_line_sync(cmdline, &out, &err, &status, NULL);
    if (!ok || status != 0) {
        if (out) g_free(out);
        if (err) g_free(err);
        return NULL;
    }
    if (err) g_free(err);
    return out; /* caller g_free */
}

static gboolean is_root_parent_device(const char *device_path) {
    char *root_src = read_command_stdout("df --output=source / | tail -1");
    if (!root_src) return FALSE;
    g_strchomp(root_src);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "lsblk -no PKNAME %s 2>/dev/null", root_src);
    g_free(root_src);

    char *pk = read_command_stdout(cmd);
    if (!pk) return FALSE;
    g_strchomp(pk);

    char parent[512];
    snprintf(parent, sizeof(parent), "/dev/%s", pk);
    g_free(pk);

    return (strcmp(parent, device_path) == 0);
}

static void make_partition_names(const char *dev, char *p1, size_t p1sz, char *p2, size_t p2sz) {
    size_t n = strlen(dev);
    if (n > 0 && isdigit((unsigned char)dev[n-1])) {
        g_snprintf(p1, p1sz, "%sp1", dev);
        g_snprintf(p2, p2sz, "%sp2", dev);
    } else {
        g_snprintf(p1, p1sz, "%s1", dev);
        g_snprintf(p2, p2sz, "%s2", dev);
    }
}

/* Heuristic: removable/transport + not root + optional large-disk filter */
static gboolean is_candidate_disk(json_object *dev, gboolean restrict_mode,
                                  char *out_path, size_t out_sz,
                                  char *out_desc, size_t desc_sz) {
    json_object *jname=NULL, *jrm=NULL, *jtype=NULL, *jmodel=NULL, *jtran=NULL, *jsize=NULL;
    if (!json_object_object_get_ex(dev, "name", &jname)) return FALSE;
    if (!json_object_object_get_ex(dev, "type", &jtype)) return FALSE;
    const char *type = json_object_get_string(jtype);
    if (g_strcmp0(type, "disk") != 0) return FALSE;

    const char *name  = json_object_get_string(jname);
    const char *model = (json_object_object_get_ex(dev, "model", &jmodel) && !json_object_is_type(jmodel, json_type_null))
                        ? json_object_get_string(jmodel) : "";
    const char *tran  = (json_object_object_get_ex(dev, "tran", &jtran) && !json_object_is_type(jtran, json_type_null))
                        ? json_object_get_string(jtran) : "";
    long rm = (json_object_object_get_ex(dev, "rm", &jrm)) ? json_object_get_int(jrm) : 0;
    const char *size = (json_object_object_get_ex(dev, "size", &jsize) && !json_object_is_type(jsize, json_type_null))
                        ? json_object_get_string(jsize) : "";

    char path[128];
    g_snprintf(path, sizeof(path), "/dev/%s", name);

    if (is_root_parent_device(path)) return FALSE;

    gboolean removableish = (rm == 1) || g_strcmp0(tran, "usb") == 0 || g_strcmp0(tran, "mmc") == 0;
    if (!removableish) return FALSE;

    if (restrict_mode) {
        if (g_str_has_suffix(size, "T")) return FALSE;
        if (g_str_has_suffix(size, "G")) {
            double gnum = g_ascii_strtod(size, NULL);
            if (gnum >= 512.0) return FALSE;
        }
    }

    if (out_path) g_strlcpy(out_path, path, out_sz);
    if (out_desc) g_snprintf(out_desc, desc_sz, "%s  %s  [%s]",
                             path, (model && *model) ? model : "Removable", (size && *size) ? size : "");
    return TRUE;
}

static gboolean populate_devices(AppData *app) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device_combo));

    char *json_txt = read_command_stdout("lsblk -J -o NAME,RM,SIZE,MODEL,TRAN,TYPE");
    if (!json_txt) { set_status(app, "Failed to enumerate devices (lsblk)."); return FALSE; }

    json_object *root = json_tokener_parse(json_txt);
    g_free(json_txt);
    if (!root) { set_status(app, "Failed to parse lsblk JSON."); return FALSE; }

    json_object *blockdevices = NULL;
    if (!json_object_object_get_ex(root, "blockdevices", &blockdevices) ||
        !json_object_is_type(blockdevices, json_type_array)) {
        json_object_put(root);
        set_status(app, "No block devices found.");
        return FALSE;
    }

    gboolean restrict_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->restrict_toggle));
    int n = json_object_array_length(blockdevices);
    int added = 0;
    for (int i = 0; i < n; i++) {
        json_object *dev = json_object_array_get_idx(blockdevices, i);
        char path[128], desc[256];
        if (is_candidate_disk(dev, restrict_mode, path, sizeof(path), desc, sizeof(desc))) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->device_combo), path, desc);
            added++;
        }
    }
    json_object_put(root);

    if (added == 0) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->device_combo), "", "— No safe removable media detected —");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
        set_status(app, "No candidate SD/USB media detected.");
        return FALSE;
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    set_status(app, "Ready. Select a device and click Format.");
    return TRUE;
}

/* Progress tick while formatting */
static gboolean update_progress_cb(gpointer user_data) {
    AppData *app = (AppData*)user_data;
    if (!app->formatting) return G_SOURCE_REMOVE;
    gdouble p = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(app->progress_bar));
    p += 0.03; if (p > 0.95) p = 0.95;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), p);
    return G_SOURCE_CONTINUE;
}

static void child_watch_cb(GPid pid, gint status, gpointer user_data) {
    AppData *app = (AppData*)user_data;
    app->formatting = FALSE;
    gtk_widget_set_sensitive(app->format_button, TRUE);
    gtk_widget_set_sensitive(app->abort_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        set_status(app, "Format completed successfully.");
        GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                "Format completed successfully.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
    } else {
        set_status(app, "Format failed or was aborted.");
        GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                "Format failed or was aborted.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
    }
    g_spawn_close_pid(pid);
}

static void on_abort_clicked(GtkButton *btn, AppData *app) {
    if (app->formatting && app->child_pid > 0) {
        kill(app->child_pid, SIGTERM);
        set_status(app, "Aborting…");
    }
}

static void on_format_clicked(GtkButton *btn, AppData *app) {
    gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device_combo));
    if (!id || !*id) {
        set_status(app, "Select a valid removable device.");
        g_free(id);
        return;
    }
    const char *label_text = gtk_entry_get_text(GTK_ENTRY(app->label_entry));
    if (!label_text || strlen(label_text) == 0) label_text = "MICROPYTHON";

    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
                                            "This will erase all data on %s.\nProceed?", id);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) { g_free(id); return; }

    if (is_root_parent_device(id)) {
        set_status(app, "Refusing: target appears to contain the root filesystem.");
        g_free(id);
        return;
    }

    gchar *cmdline = g_strdup_printf(
        "bash -c '"
        "set -e; "
        "dev=%1$s; "
        "wipefs -a \"$dev\"; "
        "parted -s \"$dev\" mklabel msdos; "
        "bytes=$(lsblk -nbdo SIZE \"$dev\"); "
        "mib=$((bytes/1024/1024)); "
        "start=1; reserve=32; end1=$((mib-reserve)); "
        "if [ $end1 -le $((start+8)) ]; then echo \"device too small\"; exit 1; fi; "
        "parted -s \"$dev\" mkpart primary fat32 1MiB ${end1}MiB; "
        "parted -s \"$dev\" mkpart primary ${end1}MiB 100%%; "
        "partprobe \"$dev\"; udevadm settle; "
        "if echo \"$dev\" | grep -Eq \"[0-9]$\"; then P1=\"${dev}p1\"; else P1=\"${dev}1\"; fi; "
        "mkfs.fat -F32 -v -I -n \"%2$s\" \"$P1\"'"
        , id, label_text);

    gtk_widget_set_sensitive(app->format_button, FALSE);
    gtk_widget_set_sensitive(app->refresh_button, FALSE);
    gtk_widget_set_sensitive(app->abort_button, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    set_status(app, "Formatting…");
    app->formatting = TRUE;

    GError *err = NULL;
    gboolean ok = g_spawn_async(
        NULL,
        (gchar*[]){"/bin/bash","-c",cmdline,NULL},
        NULL,
        G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,NULL,
        &app->child_pid,&err);

    g_free(cmdline);
    g_free(id);

    if (!ok) {
        set_status(app, err ? err->message : "Failed to spawn format process.");
        if (err) g_error_free(err);
        gtk_widget_set_sensitive(app->format_button, TRUE);
        gtk_widget_set_sensitive(app->abort_button, FALSE);
        gtk_widget_set_sensitive(app->refresh_button, TRUE);
        app->formatting = FALSE;
        return;
    }

    g_child_watch_add(app->child_pid, child_watch_cb, app);
    g_timeout_add(200, update_progress_cb, app);
}

/* ---------- UI ---------- */
static void activate(GtkApplication *gapp, gpointer user_data) {
    AppData *app = g_new0(AppData, 1);
    if (!require_root_dialog(NULL)) { g_application_quit(G_APPLICATION(gapp)); return; }

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "sdprep");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 340);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(win), outer);
    gtk_widget_set_margin_top(outer, 10);
    gtk_widget_set_margin_bottom(outer, 10);
    gtk_widget_set_margin_start(outer, 10);
    gtk_widget_set_margin_end(outer, 10);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), head, FALSE, FALSE, 0);

    GtkWidget *icon = gtk_image_new_from_resource("/com/drflores/sdprep/icons/sdprep.png");
    gtk_widget_set_size_request(icon, 32, 32);
    gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>SD/USB Prep</span>");
    gtk_box_pack_start(GTK_BOX(head), title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(outer), grid, TRUE, TRUE, 0);

    GtkWidget *lbl_dev = gtk_label_new("Device:");
    gtk_grid_attach(GTK_GRID(grid), lbl_dev, 0, 0, 1, 1);

    app->device_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(app->device_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->device_combo, 1, 0, 3, 1);

    app->restrict_toggle = gtk_check_button_new_with_label("Prefer SD/microSD & USB flash (avoid likely backup HDDs)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->restrict_toggle), TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->restrict_toggle, 1, 1, 3, 1);

    GtkWidget *lbl_lab = gtk_label_new("Volume label:");
    gtk_grid_attach(GTK_GRID(grid), lbl_lab, 0, 2, 1, 1);

    app->label_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->label_entry), "MICROPYTHON");
    gtk_grid_attach(GTK_GRID(grid), app->label_entry, 1, 2, 3, 1);

    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->progress_bar, 0, 3, 4, 1);

    GtkWidget *btnrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), btnrow, FALSE, FALSE, 0);

    app->format_button  = gtk_button_new_with_label("Format");
    app->abort_button   = gtk_button_new_with_label("Abort");
    app->refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *quitbtn  = gtk_button_new_with_label("Quit");

    gtk_box_pack_start(GTK_BOX(btnrow), app->format_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnrow), app->abort_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnrow), app->refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnrow), quitbtn, FALSE, FALSE, 0);

    app->status_label = gtk_label_new("Initializing…");
    gtk_box_pack_start(GTK_BOX(outer), app->status_label, FALSE, FALSE, 0);

    g_signal_connect(app->format_button,  "clicked", G_CALLBACK(on_format_clicked), app);
    g_signal_connect(app->abort_button,   "clicked", G_CALLBACK(on_abort_clicked), app);
    g_signal_connect_swapped(app->refresh_button, "clicked", G_CALLBACK(populate_devices), app);
    g_signal_connect_swapped(quitbtn, "clicked", G_CALLBACK(gtk_widget_destroy), win);

    app->window = win;
    app->formatting = FALSE;
    gtk_widget_set_sensitive(app->abort_button, FALSE);

    populate_devices(app);
    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.drflores.sdprep", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
