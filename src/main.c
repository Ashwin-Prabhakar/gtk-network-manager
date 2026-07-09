/*
 * gtk-network-manager - Touch-first GTK4/libnm Wi-Fi manager for Weston/Wayland.
 *
 * Runtime target:
 *   GTK4 Wayland client -> Weston DRM backend -> KMS/DRM
 *   libnm -> NetworkManager -> WLAN driver/firmware
 *
 * Virtual keyboard:
 *   Weston must be configured with [input-method] path=/usr/libexec/weston-keyboard
 *   Password GtkEntry uses GTK_INPUT_PURPOSE_PASSWORD and receives focus automatically.
 */

#include <gtk/gtk.h>
#include <NetworkManager.h>
#include <string.h>

#define APP_ID "com.qualcomm.GtkNmManager"

typedef struct {
    GtkApplication *gtk_app;
    GtkWidget *window;
    GtkWidget *wifi_switch;
    GtkWidget *scan_button;
    GtkWidget *status_label;
    GtkWidget *listbox;
    GtkWidget *spinner;

    /* Connected AP area inside top card */
    GtkWidget *connected_box;
    GtkWidget *connected_sep;

    /* Inline password panel (no floating windows) */
    GtkWidget *password_revealer;
    GtkWidget *password_title;
    GtkWidget *password_entry;
    NMAccessPoint *pending_ap;

    /* Inline PSK reveal panel */
    GtkWidget *psk_revealer;
    GtkWidget *psk_ssid_label;
    GtkWidget *psk_label;

    NMClient *client;
    NMDeviceWifi *wifi_dev;
    guint refresh_timer_id;
    guint delayed_refresh_id;
    GCancellable *cancellable;
} App;

static gboolean refresh_timer_cb(gpointer user_data);
static gboolean delayed_refresh_cb(gpointer user_data);
static void refresh_wifi_list(App *app);

static void schedule_delayed_refresh(App *app, guint ms)
{
    if (app->delayed_refresh_id) {
        g_source_remove(app->delayed_refresh_id);
        app->delayed_refresh_id = 0;
    }
    app->delayed_refresh_id = g_timeout_add(ms, delayed_refresh_cb, app);
}

static char *ssid_to_string(GBytes *ssid)
{
    if (!ssid)
        return g_strdup("Hidden Network");

    gsize len = 0;
    const guint8 *data = g_bytes_get_data(ssid, &len);
    if (!data || len == 0)
        return g_strdup("Hidden Network");

    char *utf8 = nm_utils_ssid_to_utf8(data, len);
    if (!utf8 || utf8[0] == '\0') {
        g_free(utf8);
        return g_strdup("Hidden Network");
    }
    return utf8;
}

static const char *security_to_string(NMAccessPoint *ap)
{
    NM80211ApFlags flags = nm_access_point_get_flags(ap);
    NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
    NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);

    if ((flags & NM_802_11_AP_FLAGS_PRIVACY) == 0 &&
        wpa == NM_802_11_AP_SEC_NONE &&
        rsn == NM_802_11_AP_SEC_NONE)
        return "Open";

    if (rsn != NM_802_11_AP_SEC_NONE)
        return "Secured";

    if (wpa != NM_802_11_AP_SEC_NONE)
        return "Secured";

    if (flags & NM_802_11_AP_FLAGS_PRIVACY)
        return "Secured";

    return "Unknown";
}

static const char *signal_icon_name(guint8 strength)
{
    if (strength > 75) return "network-wireless-signal-excellent-symbolic";
    if (strength > 50) return "network-wireless-signal-good-symbolic";
    if (strength > 25) return "network-wireless-signal-ok-symbolic";
    return "network-wireless-signal-weak-symbolic";
}

static NMDeviceWifi *find_first_wifi_device(App *app)
{
    const GPtrArray *devices = nm_client_get_devices(app->client);
    for (guint i = 0; devices && i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (NM_IS_DEVICE_WIFI(dev))
            return NM_DEVICE_WIFI(dev);
    }
    return NULL;
}

static gboolean ap_is_active(App *app, NMAccessPoint *ap)
{
    if (!app->wifi_dev || !ap)
        return FALSE;

    NMAccessPoint *active = nm_device_wifi_get_active_access_point(app->wifi_dev);
    if (!active)
        return FALSE;

    const char *a = nm_object_get_path(NM_OBJECT(active));
    const char *b = nm_object_get_path(NM_OBJECT(ap));
    return g_strcmp0(a, b) == 0;
}

static void set_status(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text ? text : "");
}

static void hide_password_panel(App *app)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->password_revealer), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(app->password_entry), "");
    g_clear_object(&app->pending_ap);
}

static void on_password_revealer_revealed(GObject *revealer, GParamSpec *pspec, gpointer user_data)
{
    GtkWidget *entry = GTK_WIDGET(user_data);
    if (gtk_revealer_get_child_revealed(GTK_REVEALER(revealer))) {
        gtk_widget_grab_focus(entry);
        g_signal_handlers_disconnect_by_func(revealer, on_password_revealer_revealed, user_data);
    }
}

static void show_password_panel(App *app, NMAccessPoint *ap, const char *ssid)
{
    g_set_object(&app->pending_ap, ap);

    char *title = g_strdup_printf("Connect to \"%s\"", ssid);
    gtk_label_set_text(GTK_LABEL(app->password_title), title);
    g_free(title);

    gtk_editable_set_text(GTK_EDITABLE(app->password_entry), "");
    g_signal_connect(app->password_revealer, "notify::child-revealed",
                     G_CALLBACK(on_password_revealer_revealed), app->password_entry);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->password_revealer), TRUE);
}

static void activate_done_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;

    NMActiveConnection *active = nm_client_add_and_activate_connection_finish(NM_CLIENT(source), res, &error);
    if (!active) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&error);
            return;
        }
        if (app->window) {
            char *msg = g_strdup_printf("Connection failed: %s", error ? error->message : "unknown error");
            set_status(app, msg);
            g_free(msg);
        }
        g_clear_error(&error);
        return;
    }

    if (!app->window)
        return;

    set_status(app, "Connection activation requested");
    refresh_wifi_list(app);
}

static void connect_to_ap(App *app, NMAccessPoint *ap, const char *password)
{
    if (!app->wifi_dev || !ap)
        return;

    GBytes *ssid = nm_access_point_get_ssid(ap);
    char *ssid_str = ssid_to_string(ssid);
    char *uuid = g_uuid_string_random();

    NMConnection *conn = nm_simple_connection_new();

    NMSettingConnection *s_con = NM_SETTING_CONNECTION(nm_setting_connection_new());
    NMSettingWireless *s_wifi = NM_SETTING_WIRELESS(nm_setting_wireless_new());
    NMSettingIPConfig *s_ip4 = NM_SETTING_IP_CONFIG(nm_setting_ip4_config_new());
    NMSettingIPConfig *s_ip6 = NM_SETTING_IP_CONFIG(nm_setting_ip6_config_new());

    g_object_set(G_OBJECT(s_con),
                 NM_SETTING_CONNECTION_ID, ssid_str,
                 NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
                 NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
                 NULL);

    g_object_set(G_OBJECT(s_wifi),
                 NM_SETTING_WIRELESS_SSID, ssid,
                 NM_SETTING_WIRELESS_MODE, "infrastructure",
                 NULL);

    g_object_set(G_OBJECT(s_ip4),
                 NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
                 NULL);

    g_object_set(G_OBJECT(s_ip6),
                 NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
                 NULL);

    nm_connection_add_setting(conn, NM_SETTING(s_con));
    nm_connection_add_setting(conn, NM_SETTING(s_wifi));
    nm_connection_add_setting(conn, NM_SETTING(s_ip4));
    nm_connection_add_setting(conn, NM_SETTING(s_ip6));

    if (password && password[0] != '\0') {
        NMSettingWirelessSecurity *s_sec = NM_SETTING_WIRELESS_SECURITY(nm_setting_wireless_security_new());
        g_object_set(G_OBJECT(s_sec),
                     NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                     NM_SETTING_WIRELESS_SECURITY_PSK, password,
                     NULL);
        nm_connection_add_setting(conn, NM_SETTING(s_sec));
    }

    const char *ap_path = nm_object_get_path(NM_OBJECT(ap));
    char *msg = g_strdup_printf("Connecting to %s…", ssid_str);
    set_status(app, msg);
    g_free(msg);

    nm_client_add_and_activate_connection_async(app->client,
                                                conn,
                                                NM_DEVICE(app->wifi_dev),
                                                ap_path,
                                                app->cancellable,
                                                activate_done_cb,
                                                app);

    g_object_unref(conn);
    g_free(uuid);
    g_free(ssid_str);
}

static void password_cancel_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    hide_password_panel((App *)user_data);
}

static void password_connect_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;
    const char *password = gtk_editable_get_text(GTK_EDITABLE(app->password_entry));

    if (!password || password[0] == '\0') {
        set_status(app, "Password required");
        return;
    }

    connect_to_ap(app, app->pending_ap, password);
    hide_password_panel(app);
}

static void password_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    password_connect_clicked(NULL, user_data);
}

static void ap_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    App *app = user_data;
    NMAccessPoint *ap = g_object_get_data(G_OBJECT(row), "ap");
    if (!ap)
        return;

    if (ap_is_active(app, ap))
        return;

    char *ssid = ssid_to_string(nm_access_point_get_ssid(ap));
    const char *sec = security_to_string(ap);

    if (g_strcmp0(sec, "Open") == 0) {
        connect_to_ap(app, ap, NULL);
    } else {
        show_password_panel(app, ap, ssid);
    }
    g_free(ssid);
}

static void psk_hide_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->psk_revealer), FALSE);
    gtk_label_set_text(GTK_LABEL(app->psk_label), "");
}

static void secrets_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;

    GVariant *secrets = nm_remote_connection_get_secrets_finish(
        NM_REMOTE_CONNECTION(source), res, &error);
    if (!secrets) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&error);
            return;
        }
        if (app->window)
            set_status(app, error ? error->message : "Failed to retrieve secrets");
        g_clear_error(&error);
        return;
    }

    if (!app->window) {
        g_variant_unref(secrets);
        return;
    }

    const char *psk = NULL;
    GVariant *wifi_sec = g_variant_lookup_value(
        secrets, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME, NULL);
    if (wifi_sec) {
        g_variant_lookup(wifi_sec, NM_SETTING_WIRELESS_SECURITY_PSK, "&s", &psk);
        g_variant_unref(wifi_sec);
    }

    if (psk && psk[0] != '\0') {
        gtk_label_set_text(GTK_LABEL(app->psk_label), psk);
        gtk_revealer_set_reveal_child(GTK_REVEALER(app->psk_revealer), TRUE);
    } else {
        set_status(app, "No saved password found for this network");
    }
    g_variant_unref(secrets);
}

static void show_psk_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    if (!app->wifi_dev)
        return;

    NMActiveConnection *active_conn = nm_device_get_active_connection(NM_DEVICE(app->wifi_dev));
    if (!active_conn) {
        set_status(app, "No active connection");
        return;
    }

    NMConnection *conn = NM_CONNECTION(nm_active_connection_get_connection(active_conn));
    if (!conn || !NM_IS_REMOTE_CONNECTION(conn)) {
        set_status(app, "Cannot access connection profile");
        return;
    }

    NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(app->wifi_dev);
    char *ssid = ssid_to_string(active_ap ? nm_access_point_get_ssid(active_ap) : NULL);
    char *title = g_strdup_printf("Password for \"%s\"", ssid);
    gtk_label_set_text(GTK_LABEL(app->psk_ssid_label), title);
    g_free(title);
    g_free(ssid);

    nm_remote_connection_get_secrets_async(NM_REMOTE_CONNECTION(conn),
                                           NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
                                           app->cancellable, secrets_cb, app);
}

/* Build an iOS-style row for the networks listbox (non-connected APs) */
static GtkWidget *make_ap_row(App *app, NMAccessPoint *ap)
{
    (void)app;
    char *ssid = ssid_to_string(nm_access_point_get_ssid(ap));
    guint8 strength = nm_access_point_get_strength(ap);
    const char *sec = security_to_string(ap);
    gboolean is_open = g_strcmp0(sec, "Open") == 0;

    GtkWidget *row = gtk_list_box_row_new();

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 16);
    gtk_widget_set_margin_end(hbox, 12);
    gtk_widget_set_margin_top(hbox, 11);
    gtk_widget_set_margin_bottom(hbox, 11);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    /* SSID label (expands) */
    GtkWidget *ssid_lbl = gtk_label_new(ssid);
    gtk_widget_add_css_class(ssid_lbl, "ssid-label");
    gtk_widget_set_halign(ssid_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(ssid_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(ssid_lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(ssid_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hbox), ssid_lbl);

    /* Lock icon (secured networks only) */
    if (!is_open) {
        GtkWidget *lock = gtk_image_new_from_icon_name("changes-prevent-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(lock), 14);
        gtk_widget_add_css_class(lock, "row-icon");
        gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hbox), lock);
    }

    /* WiFi signal strength icon */
    GtkWidget *sig_img = gtk_image_new_from_icon_name(signal_icon_name(strength));
    gtk_image_set_pixel_size(GTK_IMAGE(sig_img), 18);
    gtk_widget_add_css_class(sig_img, "row-icon");
    gtk_widget_set_valign(sig_img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), sig_img);

    /* Info button (blue ⓘ circle) */
    GtkWidget *info_img = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(info_img), 20);
    gtk_widget_add_css_class(info_img, "info-icon");
    GtkWidget *info_btn = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(info_btn), info_img);
    gtk_widget_add_css_class(info_btn, "flat");
    gtk_widget_add_css_class(info_btn, "info-btn");
    gtk_widget_set_valign(info_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), info_btn);

    g_object_set_data_full(G_OBJECT(row), "ap", g_object_ref(ap), g_object_unref);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_free(ssid);
    return row;
}

/* Build the content for the connected AP shown inside the top card */
static GtkWidget *make_connected_row_content(App *app, NMAccessPoint *ap)
{
    char *ssid = ssid_to_string(nm_access_point_get_ssid(ap));
    guint8 strength = nm_access_point_get_strength(ap);
    const char *sec = security_to_string(ap);
    gboolean is_open = g_strcmp0(sec, "Open") == 0;

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 16);
    gtk_widget_set_margin_end(hbox, 12);
    gtk_widget_set_margin_top(hbox, 11);
    gtk_widget_set_margin_bottom(hbox, 11);

    /* Blue checkmark on the left */
    GtkWidget *chk_img = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(chk_img), 18);
    gtk_widget_add_css_class(chk_img, "connected-check-icon");
    gtk_widget_set_valign(chk_img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), chk_img);

    /* SSID */
    GtkWidget *ssid_lbl = gtk_label_new(ssid);
    gtk_widget_add_css_class(ssid_lbl, "ssid-label");
    gtk_widget_set_halign(ssid_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(ssid_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(ssid_lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(ssid_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hbox), ssid_lbl);

    /* Lock icon */
    if (!is_open) {
        GtkWidget *lock = gtk_image_new_from_icon_name("changes-prevent-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(lock), 14);
        gtk_widget_add_css_class(lock, "row-icon");
        gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hbox), lock);
    }

    /* WiFi signal icon */
    GtkWidget *sig_img = gtk_image_new_from_icon_name(signal_icon_name(strength));
    gtk_image_set_pixel_size(GTK_IMAGE(sig_img), 18);
    gtk_widget_add_css_class(sig_img, "row-icon");
    gtk_widget_set_valign(sig_img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), sig_img);

    /* Info button */
    GtkWidget *info_img = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(info_img), 20);
    gtk_widget_add_css_class(info_img, "info-icon");
    GtkWidget *info_btn = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(info_btn), info_img);
    gtk_widget_add_css_class(info_btn, "flat");
    gtk_widget_add_css_class(info_btn, "info-btn");
    gtk_widget_set_valign(info_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), info_btn);

    /* Show Password button for secured connected networks */
    if (!is_open) {
        GtkWidget *pw_img = gtk_image_new_from_icon_name("dialog-password-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(pw_img), 16);
        GtkWidget *show_btn = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(show_btn), pw_img);
        gtk_widget_add_css_class(show_btn, "flat");
        gtk_widget_add_css_class(show_btn, "info-btn");
        gtk_widget_set_valign(show_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(show_btn, "Show saved password");
        gtk_box_append(GTK_BOX(hbox), show_btn);
        g_signal_connect(show_btn, "clicked", G_CALLBACK(show_psk_clicked), app);
    }

    g_free(ssid);
    return hbox;
}

static gint ap_sort_cb(gconstpointer a, gconstpointer b)
{
    NMAccessPoint *ap_a = *(NMAccessPoint **)a;
    NMAccessPoint *ap_b = *(NMAccessPoint **)b;
    guint8 sa = nm_access_point_get_strength(ap_a);
    guint8 sb = nm_access_point_get_strength(ap_b);
    return (sb > sa) - (sb < sa);
}

static void clear_listbox(GtkWidget *listbox)
{
    GtkWidget *child = gtk_widget_get_first_child(listbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(listbox), child);
        child = next;
    }
}

static void clear_box_children(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_widget_unparent(child);
}

static void refresh_wifi_list(App *app)
{
    clear_listbox(app->listbox);
    clear_box_children(app->connected_box);
    gtk_widget_set_visible(app->connected_sep, FALSE);

    app->wifi_dev = find_first_wifi_device(app);
    if (!app->wifi_dev) {
        set_status(app, "No Wi-Fi interface detected");
        return;
    }

    gboolean enabled = nm_client_wireless_get_enabled(app->client);
    gtk_switch_set_active(GTK_SWITCH(app->wifi_switch), enabled);

    if (!enabled) {
        set_status(app, "Wi-Fi is disabled");
        return;
    }

    /* Show connected AP in top card */
    NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(app->wifi_dev);
    if (active_ap) {
        gtk_widget_set_visible(app->connected_sep, TRUE);
        GtkWidget *conn_content = make_connected_row_content(app, active_ap);
        gtk_box_append(GTK_BOX(app->connected_box), conn_content);
    }

    const GPtrArray *aps = nm_device_wifi_get_access_points(app->wifi_dev);
    if (!aps || aps->len == 0) {
        set_status(app, "No access points visible. Tap Scan to refresh.");
        return;
    }

    GPtrArray *sorted = g_ptr_array_new();
    for (guint i = 0; i < aps->len; i++)
        g_ptr_array_add(sorted, g_ptr_array_index(aps, i));
    g_ptr_array_sort(sorted, ap_sort_cb);

    /* Deduplicate by SSID; active AP goes to top card, not listbox */
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *unique = g_ptr_array_new();

    if (active_ap) {
        char *ssid = ssid_to_string(nm_access_point_get_ssid(active_ap));
        g_hash_table_add(seen, ssid); /* owned by hash table */
    }

    for (guint i = 0; i < sorted->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(sorted, i);
        if (ap == active_ap)
            continue;
        char *ssid = ssid_to_string(nm_access_point_get_ssid(ap));
        if (!g_hash_table_contains(seen, ssid)) {
            g_hash_table_add(seen, g_strdup(ssid));
            g_ptr_array_add(unique, ap);
        }
        g_free(ssid);
    }
    g_hash_table_destroy(seen);

    for (guint i = 0; i < unique->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(unique, i);
        GtkWidget *row = make_ap_row(app, ap);
        gtk_list_box_append(GTK_LIST_BOX(app->listbox), row);
    }

    /* "Other…" row at bottom */
    GtkWidget *other_row = gtk_list_box_row_new();
    GtkWidget *other_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(other_hbox, 16);
    gtk_widget_set_margin_end(other_hbox, 16);
    gtk_widget_set_margin_top(other_hbox, 11);
    gtk_widget_set_margin_bottom(other_hbox, 11);
    GtkWidget *other_lbl = gtk_label_new("Other…");
    gtk_widget_add_css_class(other_lbl, "ssid-label");
    gtk_widget_set_halign(other_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(other_hbox), other_lbl);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(other_row), other_hbox);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(other_row), FALSE);
    gtk_list_box_append(GTK_LIST_BOX(app->listbox), other_row);

    guint total = unique->len + (active_ap ? 1 : 0);
    char *msg = g_strdup_printf("%u network(s) found", total);
    set_status(app, msg);
    g_free(msg);
    g_ptr_array_unref(unique);
    g_ptr_array_unref(sorted);
}

static void background_scan_done_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;
    gboolean ok = nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), res, &error);
    g_clear_error(&error);
    if (ok && app->window)
        schedule_delayed_refresh(app, 1500);
}

static gboolean refresh_timer_cb(gpointer user_data)
{
    App *app = user_data;
    if (!app->window)
        return G_SOURCE_REMOVE;
    refresh_wifi_list(app);
    if (app->wifi_dev)
        nm_device_wifi_request_scan_async(app->wifi_dev, app->cancellable,
                                          background_scan_done_cb, app);
    return G_SOURCE_CONTINUE;
}

static gboolean delayed_refresh_cb(gpointer user_data)
{
    App *app = user_data;
    app->delayed_refresh_id = 0;
    if (!app->window)
        return G_SOURCE_REMOVE;
    gtk_spinner_stop(GTK_SPINNER(app->spinner));
    refresh_wifi_list(app);
    return G_SOURCE_REMOVE;
}

static void scan_done_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;

    if (!nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), res, &error)) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&error);
            return;
        }
        if (app->window) {
            gtk_spinner_stop(GTK_SPINNER(app->spinner));
            char *msg = g_strdup_printf("Scan failed: %s", error ? error->message : "unknown error");
            set_status(app, msg);
            g_free(msg);
        }
        g_clear_error(&error);
        return;
    }

    if (!app->window)
        return;

    set_status(app, "Scan requested. Updating list…");
    schedule_delayed_refresh(app, 1500);
}

static void scan_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;
    app->wifi_dev = find_first_wifi_device(app);
    if (!app->wifi_dev) {
        set_status(app, "No Wi-Fi interface detected");
        return;
    }

    gtk_spinner_start(GTK_SPINNER(app->spinner));
    set_status(app, "Scanning…");
    nm_device_wifi_request_scan_async(app->wifi_dev, app->cancellable, scan_done_cb, app);
}

static gboolean wifi_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data)
{
    App *app = user_data;
    nm_client_wireless_set_enabled(app->client, state);
    set_status(app, state ? "Wi-Fi enabled" : "Wi-Fi disabled");
    schedule_delayed_refresh(app, 500);
    return FALSE;
}

static void on_outside_press(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    (void)n_press;
    App *app = user_data;

    if (!gtk_revealer_get_reveal_child(GTK_REVEALER(app->password_revealer)))
        return;

    GtkWidget *panel = gtk_revealer_get_child(GTK_REVEALER(app->password_revealer));
    graphene_point_t wpt = GRAPHENE_POINT_INIT((float)x, (float)y);
    graphene_point_t ppt;

    if (!gtk_widget_compute_point(
            gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)),
            panel, &wpt, &ppt))
        return;

    if (ppt.x < 0 || ppt.y < 0 ||
        ppt.x > gtk_widget_get_width(panel) ||
        ppt.y > gtk_widget_get_height(panel))
        hide_password_panel(app);
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    App *app = user_data;

    if (app->refresh_timer_id) {
        g_source_remove(app->refresh_timer_id);
        app->refresh_timer_id = 0;
    }
    if (app->delayed_refresh_id) {
        g_source_remove(app->delayed_refresh_id);
        app->delayed_refresh_id = 0;
    }
    g_cancellable_cancel(app->cancellable);
    app->window = NULL;
}

static void apply_css(void)
{
    static const char *css =
        /* ── iOS-inspired light Wi-Fi settings ─────────────────────────────
         * Background:  #f2f2f7  (iOS systemGroupedBackground)
         * Card:        #ffffff
         * Text:        #1c1c1e  (iOS label)  secondary: #6c6c70
         * Separator:   #c6c6c8
         * Blue:        #007aff  (iOS tint)
         * Green:       #34c759  (iOS switch active)
         * ────────────────────────────────────────────────────────────────── */

        /* Window */
        "window { background: #f2f2f7; color: #1c1c1e; }"

        /* Cards — white rounded boxes */
        ".card { background: #ffffff; border-radius: 13px; }"

        /* WiFi icon badge (blue rounded square) */
        ".wifi-badge {"
        "  background: #007aff;"
        "  border-radius: 14px;"
        "  min-width: 60px; max-width: 60px;"
        "  min-height: 60px; max-height: 60px; }"
        ".wifi-badge-icon { color: #ffffff; }"

        /* Typography */
        ".wifi-title { font-size: 22px; font-weight: 700; color: #1c1c1e; }"
        ".wifi-desc  { font-size: 15px; color: #6c6c70; }"
        ".toggle-label { font-size: 17px; color: #1c1c1e; }"
        ".ssid-label   { font-size: 17px; color: #1c1c1e; }"
        ".section-header { font-size: 13px; color: #6c6c70; }"
        ".status-label   { font-size: 13px; color: #6c6c70; }"
        ".muted { font-size: 14px; color: #6c6c70; }"
        ".dialog-title { font-size: 20px; font-weight: 600; color: #1c1c1e; }"

        /* Card internal separator — inset on both sides like iOS */
        ".card-sep { background: #c6c6c8; min-height: 1px;"
        "            margin-left: 16px; margin-right: 16px; }"

        /* Switch — green when on */
        "switch { background: #e5e5ea; border-radius: 360px;"
        "         min-width: 51px; min-height: 31px;"
        "         border: none; outline: none; }"
        "switch:checked { background: #34c759; }"
        "switch slider { background: #ffffff; border-radius: 360px;"
        "                min-width: 27px; min-height: 27px;"
        "                box-shadow: 0 2px 4px rgba(0,0,0,0.25); }"

        /* Listbox (networks list) */
        /* Transparent background on the list itself; rows carry the white card look.
           First/last rows get rounded corners — this is the only reliable GTK4 approach
           since overflow:hidden does not clip child widget backgrounds. */
        ".networks-list { background: transparent; }"
        ".networks-list > row {"
        "    background: #ffffff; padding: 0; border: none; min-height: 0;"
        "    border-bottom: 1px solid rgba(0,0,0,0.1); }"
        ".networks-list > row:first-child {"
        "    border-top-left-radius: 13px;"
        "    border-top-right-radius: 13px; }"
        ".networks-list > row:last-child {"
        "    border-bottom: none;"
        "    border-bottom-left-radius: 13px;"
        "    border-bottom-right-radius: 13px; }"
        ".networks-list > row:hover { background: #f9f9f9; }"
        ".networks-list > row:active { background: #e5e5ea; }"
        ".networks-list > row:selected,"
        ".networks-list > row:focus { background: #ffffff; outline: none; }"

        /* Row icons */
        ".connected-check-icon { color: #007aff; }"
        ".row-icon { color: #3c3c43; opacity: 0.6; }"
        ".info-icon { color: #007aff; }"
        ".info-btn { padding: 2px; min-width: 30px; min-height: 30px;"
        "            border-radius: 360px; }"
        ".info-btn:hover { background: rgba(0,122,255,0.08); }"

        /* Buttons */
        "button.suggested-action { background: #007aff; color: #ffffff;"
        "                          border-radius: 8px; padding: 8px 18px;"
        "                          font-size: 15px; font-weight: 500; }"
        "button.suggested-action:hover { background: #0066d4; }"
        "button.suggested-action:active { background: #005cbf; }"
        "button.destructive-action { background: #ff3b30; color: #ffffff;"
        "                            border-radius: 8px; padding: 6px 14px;"
        "                            font-size: 14px; }"
        "button.destructive-action:hover { background: #e0352b; }"
        ".scan-btn { color: #007aff; font-size: 15px; font-weight: 500;"
        "            padding: 4px 10px; border-radius: 6px; }"
        ".scan-btn:hover { background: rgba(0,122,255,0.08); }"

        /* Password / PSK panels */
        ".password-panel { background: #ffffff; border-radius: 13px; padding: 20px; }"
        ".password-entry { min-height: 44px; font-size: 16px; border-radius: 8px; }"
        ".psk-display { font-size: 18px; font-weight: 500; color: #1c1c1e;"
        "               font-family: monospace; letter-spacing: 1px; }";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void build_ui(App *app)
{
    apply_css();

    app->window = gtk_application_window_new(app->gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "Wi-Fi");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 480, 680);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);

    /* Capture clicks to dismiss password panel on outside tap */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(click), FALSE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_outside_press), app);
    gtk_widget_add_controller(app->window, GTK_EVENT_CONTROLLER(click));

    /* Root: scrolled window so content works on small screens */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_window_set_child(GTK_WINDOW(app->window), scroll);

    /* Shell — vertical box with page margins */
    GtkWidget *shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(shell, 16);
    gtk_widget_set_margin_bottom(shell, 32);
    gtk_widget_set_margin_start(shell, 16);
    gtk_widget_set_margin_end(shell, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), shell);

    /* ── TOP CARD ──────────────────────────────────────────────────────── */
    GtkWidget *top_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(top_card, "card");
    gtk_box_append(GTK_BOX(shell), top_card);

    /* Header: badge + title + description */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(header, 20);
    gtk_widget_set_margin_bottom(header, 16);
    gtk_widget_set_margin_start(header, 16);
    gtk_widget_set_margin_end(header, 16);
    gtk_box_append(GTK_BOX(top_card), header);

    /* WiFi blue badge */
    GtkWidget *badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(badge, "wifi-badge");
    gtk_widget_set_halign(badge, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), badge);

    GtkWidget *badge_icon = gtk_image_new_from_icon_name("network-wireless-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(badge_icon), 34);
    gtk_widget_add_css_class(badge_icon, "wifi-badge-icon");
    gtk_widget_set_margin_top(badge_icon, 13);
    gtk_widget_set_margin_bottom(badge_icon, 13);
    gtk_widget_set_margin_start(badge_icon, 13);
    gtk_widget_set_margin_end(badge_icon, 13);
    gtk_box_append(GTK_BOX(badge), badge_icon);

    /* Title */
    GtkWidget *title_lbl = gtk_label_new("Wi-Fi");
    gtk_widget_add_css_class(title_lbl, "wifi-title");
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), title_lbl);

    /* Description */
    GtkWidget *desc_lbl = gtk_label_new(
        "Connect to Wi-Fi, view available networks, and manage "
        "settings for joining networks and nearby hotspots.");
    gtk_widget_add_css_class(desc_lbl, "wifi-desc");
    gtk_label_set_wrap(GTK_LABEL(desc_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.0f);
    gtk_widget_set_halign(desc_lbl, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(header), desc_lbl);

    /* Separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep1, "card-sep");
    gtk_box_append(GTK_BOX(top_card), sep1);

    /* Wi-Fi toggle row */
    GtkWidget *toggle_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toggle_row, 16);
    gtk_widget_set_margin_end(toggle_row, 16);
    gtk_widget_set_size_request(toggle_row, -1, 44);
    gtk_box_append(GTK_BOX(top_card), toggle_row);

    GtkWidget *wifi_lbl = gtk_label_new("Wi-Fi");
    gtk_widget_add_css_class(wifi_lbl, "toggle-label");
    gtk_widget_set_hexpand(wifi_lbl, TRUE);
    gtk_widget_set_halign(wifi_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(wifi_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(toggle_row), wifi_lbl);

    app->spinner = gtk_spinner_new();
    gtk_widget_set_valign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(app->spinner, 4);
    gtk_box_append(GTK_BOX(toggle_row), app->spinner);

    app->wifi_switch = gtk_switch_new();
    gtk_widget_set_valign(app->wifi_switch, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(toggle_row), app->wifi_switch);

    /* Connected AP separator (hidden until a connection exists) */
    app->connected_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(app->connected_sep, "card-sep");
    gtk_widget_set_visible(app->connected_sep, FALSE);
    gtk_box_append(GTK_BOX(top_card), app->connected_sep);

    /* Connected AP content — filled in by refresh_wifi_list() */
    app->connected_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(top_card), app->connected_box);

    /* ── SPACER ──────────────────────────────────────────────────────── */
    GtkWidget *spacer1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(spacer1, -1, 24);
    gtk_box_append(GTK_BOX(shell), spacer1);

    /* ── "Networks" section header + Scan button ─────────────────────── */
    GtkWidget *sec_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(sec_row, 4);
    gtk_widget_set_margin_bottom(sec_row, 6);
    gtk_box_append(GTK_BOX(shell), sec_row);

    GtkWidget *sec_lbl = gtk_label_new("Networks");
    gtk_widget_add_css_class(sec_lbl, "section-header");
    gtk_widget_set_halign(sec_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(sec_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(sec_lbl, TRUE);
    gtk_box_append(GTK_BOX(sec_row), sec_lbl);

    app->scan_button = gtk_button_new_with_label("Scan");
    gtk_widget_add_css_class(app->scan_button, "flat");
    gtk_widget_add_css_class(app->scan_button, "scan-btn");
    gtk_widget_set_valign(app->scan_button, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(sec_row), app->scan_button);

    /* ── NETWORKS CARD — card styling goes directly on the listbox ───── */
    app->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->listbox), GTK_SELECTION_NONE);
    gtk_list_box_set_show_separators(GTK_LIST_BOX(app->listbox), FALSE);
    gtk_widget_remove_css_class(app->listbox, "view");
    gtk_widget_add_css_class(app->listbox, "networks-list");
    gtk_widget_set_name(app->listbox, "ap-listbox");
    gtk_box_append(GTK_BOX(shell), app->listbox);

    /* ── STATUS BAR ──────────────────────────────────────────────────── */
    GtkWidget *spacer2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(spacer2, -1, 12);
    gtk_box_append(GTK_BOX(shell), spacer2);

    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(shell), bottom_bar);

    app->status_label = gtk_label_new("Ready");
    gtk_widget_add_css_class(app->status_label, "status-label");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_widget_set_valign(app->status_label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_box_append(GTK_BOX(bottom_bar), app->status_label);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close_btn, "destructive-action");
    gtk_box_append(GTK_BOX(bottom_bar), close_btn);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), app->window);

    /* ── PASSWORD PANEL ──────────────────────────────────────────────── */
    GtkWidget *spacer3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(spacer3, -1, 12);
    gtk_box_append(GTK_BOX(shell), spacer3);

    app->password_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->password_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->password_revealer), FALSE);
    gtk_box_append(GTK_BOX(shell), app->password_revealer);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(panel, "password-panel");
    gtk_revealer_set_child(GTK_REVEALER(app->password_revealer), panel);

    app->password_title = gtk_label_new("");
    gtk_widget_add_css_class(app->password_title, "dialog-title");
    gtk_widget_set_halign(app->password_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), app->password_title);

    GtkWidget *hint = gtk_label_new("Enter the network password to join this Wi-Fi network.");
    gtk_widget_add_css_class(hint, "muted");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), hint);

    app->password_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->password_entry), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(app->password_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(app->password_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_widget_add_css_class(app->password_entry, "password-entry");
    gtk_box_append(GTK_BOX(panel), app->password_entry);

    GtkWidget *pw_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(pw_buttons, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(panel), pw_buttons);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel_btn, "flat");
    GtkWidget *join_btn = gtk_button_new_with_label("Join");
    gtk_widget_add_css_class(join_btn, "suggested-action");
    gtk_box_append(GTK_BOX(pw_buttons), cancel_btn);
    gtk_box_append(GTK_BOX(pw_buttons), join_btn);

    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(password_cancel_clicked), app);
    g_signal_connect(join_btn, "clicked", G_CALLBACK(password_connect_clicked), app);
    g_signal_connect(app->password_entry, "activate", G_CALLBACK(password_entry_activate), app);

    /* ── PSK REVEAL PANEL ────────────────────────────────────────────── */
    app->psk_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->psk_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->psk_revealer), FALSE);
    gtk_box_append(GTK_BOX(shell), app->psk_revealer);

    GtkWidget *psk_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(psk_panel, "password-panel");
    gtk_revealer_set_child(GTK_REVEALER(app->psk_revealer), psk_panel);

    app->psk_ssid_label = gtk_label_new("");
    gtk_widget_add_css_class(app->psk_ssid_label, "dialog-title");
    gtk_widget_set_halign(app->psk_ssid_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(psk_panel), app->psk_ssid_label);

    GtkWidget *psk_hint = gtk_label_new("Saved Wi-Fi password");
    gtk_widget_add_css_class(psk_hint, "muted");
    gtk_widget_set_halign(psk_hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(psk_panel), psk_hint);

    app->psk_label = gtk_label_new("");
    gtk_widget_add_css_class(app->psk_label, "psk-display");
    gtk_widget_set_halign(app->psk_label, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(app->psk_label), TRUE);
    gtk_box_append(GTK_BOX(psk_panel), app->psk_label);

    GtkWidget *psk_hide_btn = gtk_button_new_with_label("Hide");
    gtk_widget_add_css_class(psk_hide_btn, "flat");
    gtk_widget_set_halign(psk_hide_btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(psk_panel), psk_hide_btn);
    g_signal_connect(psk_hide_btn, "clicked", G_CALLBACK(psk_hide_clicked), app);

    /* Wire main signals */
    g_signal_connect(app->scan_button, "clicked", G_CALLBACK(scan_clicked), app);
    g_signal_connect(app->listbox, "row-activated", G_CALLBACK(ap_row_activated), app);
    g_signal_connect(app->wifi_switch, "state-set", G_CALLBACK(wifi_switch_state_set), app);
}

static void app_activate(GtkApplication *gtk_app, gpointer user_data)
{
    App *app = user_data;
    app->gtk_app = gtk_app;

    GError *error = NULL;
    app->client = nm_client_new(NULL, &error);
    if (!app->client) {
        g_printerr("Failed to create NMClient: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    app->cancellable = g_cancellable_new();

    build_ui(app);
    refresh_wifi_list(app);

    app->refresh_timer_id = g_timeout_add_seconds(30, refresh_timer_cb, app);
    gtk_window_present(GTK_WINDOW(app->window));
}

static void app_shutdown(GApplication *gapp, gpointer user_data)
{
    (void)gapp;
    App *app = user_data;
    if (app->refresh_timer_id) {
        g_source_remove(app->refresh_timer_id);
        app->refresh_timer_id = 0;
    }
    if (app->delayed_refresh_id) {
        g_source_remove(app->delayed_refresh_id);
        app->delayed_refresh_id = 0;
    }
    g_clear_object(&app->cancellable);
    g_clear_object(&app->pending_ap);
    g_clear_object(&app->client);
}

int main(int argc, char **argv)
{
    g_setenv("GSK_RENDERER", "gl", FALSE);

    App app;
    memset(&app, 0, sizeof(app));

    GtkApplication *gtk_app = gtk_application_new(APP_ID, G_APPLICATION_FLAGS_NONE);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(app_activate), &app);
    g_signal_connect(gtk_app, "shutdown", G_CALLBACK(app_shutdown), &app);

    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    return status;
}
