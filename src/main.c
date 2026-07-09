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
    GtkWidget *subtitle_label;
    GtkWidget *listbox;
    GtkWidget *spinner;

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

    char *title = g_strdup_printf("Connect to %s", ssid);
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
    App *app = user_data;
    NMAccessPoint *ap = g_object_get_data(G_OBJECT(row), "ap");
    if (!ap)
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

    /* Show the SSID in the panel header */
    NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(app->wifi_dev);
    char *ssid = ssid_to_string(active_ap ? nm_access_point_get_ssid(active_ap) : NULL);
    char *title = g_strdup_printf("Password for %s", ssid);
    gtk_label_set_text(GTK_LABEL(app->psk_ssid_label), title);
    g_free(title);
    g_free(ssid);

    nm_remote_connection_get_secrets_async(NM_REMOTE_CONNECTION(conn),
                                           NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
                                           app->cancellable, secrets_cb, app);
}

static GtkWidget *make_ap_row(App *app, NMAccessPoint *ap)
{
    char *ssid = ssid_to_string(nm_access_point_get_ssid(ap));
    guint8 strength = nm_access_point_get_strength(ap);
    const char *sec = security_to_string(ap);
    gboolean active = ap_is_active(app, ap);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "ap-row");
    if (active)
        gtk_widget_add_css_class(row, "connected-row");

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(outer, 14);
    gtk_widget_set_margin_bottom(outer, 14);
    gtk_widget_set_margin_start(outer, 18);
    gtk_widget_set_margin_end(outer, 18);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);

    GtkWidget *icon = gtk_label_new(active ? "✓" : "📶");
    gtk_widget_add_css_class(icon, active ? "connected-icon" : "wifi-icon");
    gtk_box_append(GTK_BOX(outer), icon);

    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(text_box, TRUE);
    gtk_box_append(GTK_BOX(outer), text_box);

    GtkWidget *ssid_label = gtk_label_new(ssid);
    gtk_widget_add_css_class(ssid_label, "ssid");
    gtk_widget_set_halign(ssid_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(ssid_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text_box), ssid_label);

    char *sub = g_strdup_printf("%s · %u MHz%s", sec, nm_access_point_get_frequency(ap), active ? " · Connected" : "");
    GtkWidget *sub_label = gtk_label_new(sub);
    gtk_widget_add_css_class(sub_label, "muted");
    gtk_widget_set_halign(sub_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(text_box), sub_label);
    g_free(sub);

    char *pct = g_strdup_printf("%u%%", strength);
    GtkWidget *pct_label = gtk_label_new(pct);
    gtk_widget_add_css_class(pct_label, "signal-strength");
    gtk_box_append(GTK_BOX(outer), pct_label);
    g_free(pct);

    if (active) {
        GtkWidget *show_btn = gtk_button_new_with_label("Show Password");
        gtk_widget_add_css_class(show_btn, "flat");
        gtk_widget_set_valign(show_btn, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(outer), show_btn);
        g_signal_connect(show_btn, "clicked", G_CALLBACK(show_psk_clicked), app);
    }

    g_object_set_data_full(G_OBJECT(row), "ap", g_object_ref(ap), g_object_unref);
    g_free(ssid);
    return row;
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

static void refresh_wifi_list(App *app)
{
    clear_listbox(app->listbox);

    app->wifi_dev = find_first_wifi_device(app);
    if (!app->wifi_dev) {
        gtk_label_set_text(GTK_LABEL(app->subtitle_label), "No Wi-Fi interface detected");
        set_status(app, "Check WLAN driver, firmware, and NetworkManager device state");
        return;
    }

    const char *iface = nm_device_get_iface(NM_DEVICE(app->wifi_dev));
    gboolean enabled = nm_client_wireless_get_enabled(app->client);
    gtk_switch_set_active(GTK_SWITCH(app->wifi_switch), enabled);

    char *subtitle = g_strdup_printf("Interface: %s", iface ? iface : "wlan");
    gtk_label_set_text(GTK_LABEL(app->subtitle_label), subtitle);
    g_free(subtitle);

    if (!enabled) {
        set_status(app, "Wi-Fi is disabled");
        return;
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

    for (guint i = 0; i < sorted->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(sorted, i);
        GtkWidget *row = make_ap_row(app, ap);
        gtk_list_box_append(GTK_LIST_BOX(app->listbox), row);
    }

    char *msg = g_strdup_printf("%u network(s) found", sorted->len);
    set_status(app, msg);
    g_free(msg);
    g_ptr_array_unref(sorted);
}

static gboolean refresh_timer_cb(gpointer user_data)
{
    refresh_wifi_list((App *)user_data);
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

    /* Null widget pointers so any stale callbacks can detect the dead window */
    app->window = NULL;
}

static void apply_css(void){
    static const char *css =
        "window { background: #0f172a; color: #e5e7eb; }"
        ".app-shell { background: #0f172a; }"
        ".card { background: #111827; border-radius: 22px; padding: 18px; box-shadow: 0 8px 30px rgba(0,0,0,0.35); }"
        ".title { font-size: 32px; font-weight: 800; color: #f8fafc; }"
        ".subtitle { font-size: 15px; color: #94a3b8; }"
        ".muted { color: #94a3b8; font-size: 14px; }"
        ".ssid { font-size: 21px; font-weight: 700; color: #f8fafc; }"
        ".ap-row { border-radius: 18px; margin: 6px; background: #1f2937; min-height: 76px; }"
        ".ap-row:hover { background: #263244; }"
        ".connected-row { background: #064e3b; }"
        ".wifi-icon { font-size: 26px; color: #38bdf8; }"
        ".connected-icon { font-size: 28px; color: #34d399; font-weight: 900; }"
        ".signal-strength { font-size: 15px; font-weight: 700; color: #38bdf8; min-width: 48px; }"
        ".primary-button { min-height: 48px; padding-left: 22px; padding-right: 22px; border-radius: 14px; font-weight: 700; }"
        ".status-pill { background: #020617; border-radius: 999px; padding: 10px 14px; color: #cbd5e1; }"
        ".dialog-title { font-size: 26px; font-weight: 800; color: #f8fafc; }"
        ".password-entry { min-height: 54px; font-size: 20px; border-radius: 14px; }"
        ".password-panel { background: #111827; border-radius: 22px; padding: 24px; }"
        ".psk-display { font-size: 28px; font-weight: 700; color: #34d399; font-family: monospace; letter-spacing: 2px; }"
        ".psk-panel { background: #0d2137; border-radius: 22px; padding: 24px; border: 2px solid #1e3a5f; }";

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
    gtk_window_set_title(GTK_WINDOW(app->window), "Network Settings");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 620);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);

    GtkWidget *shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(shell, "app-shell");
    gtk_widget_set_margin_top(shell, 24);
    gtk_widget_set_margin_bottom(shell, 24);
    gtk_widget_set_margin_start(shell, 24);
    gtk_widget_set_margin_end(shell, 24);
    gtk_window_set_child(GTK_WINDOW(app->window), shell);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_append(GTK_BOX(shell), header);

    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(title_box, TRUE);
    gtk_box_append(GTK_BOX(header), title_box);

    GtkWidget *title = gtk_label_new("Wi-Fi");
    gtk_widget_add_css_class(title, "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(title_box), title);

    app->subtitle_label = gtk_label_new("Interface: detecting…");
    gtk_widget_add_css_class(app->subtitle_label, "subtitle");
    gtk_widget_set_halign(app->subtitle_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(title_box), app->subtitle_label);

    app->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(header), app->spinner);

    app->wifi_switch = gtk_switch_new();
    gtk_widget_set_valign(app->wifi_switch, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), app->wifi_switch);

    app->scan_button = gtk_button_new_with_label("Scan");
    gtk_widget_add_css_class(app->scan_button, "primary-button");
    gtk_widget_add_css_class(app->scan_button, "suggested-action");
    gtk_box_append(GTK_BOX(header), app->scan_button);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_vexpand(card, TRUE);
    gtk_box_append(GTK_BOX(shell), card);

    app->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->listbox), GTK_SELECTION_NONE);
    gtk_widget_set_vexpand(app->listbox, TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->listbox);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(card), scroll);

    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(shell), bottom_bar);

    app->status_label = gtk_label_new("Ready");
    gtk_widget_add_css_class(app->status_label, "status-pill");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_box_append(GTK_BOX(bottom_bar), app->status_label);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close_btn, "primary-button");
    gtk_widget_add_css_class(close_btn, "destructive-action");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(bottom_bar), close_btn);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), app->window);

    /* Inline password panel — slides in, no floating window */
    app->password_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->password_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->password_revealer), FALSE);
    gtk_box_append(GTK_BOX(shell), app->password_revealer);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(panel, "password-panel");
    gtk_revealer_set_child(GTK_REVEALER(app->password_revealer), panel);

    app->password_title = gtk_label_new("");
    gtk_widget_add_css_class(app->password_title, "dialog-title");
    gtk_widget_set_halign(app->password_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), app->password_title);

    GtkWidget *hint = gtk_label_new("Enter the network password to join this secured Wi-Fi network.");
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

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(panel), buttons);

    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(connect_btn, "suggested-action");
    gtk_widget_add_css_class(connect_btn, "primary-button");
    gtk_widget_add_css_class(cancel, "flat");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), connect_btn);

    g_signal_connect(cancel, "clicked", G_CALLBACK(password_cancel_clicked), app);
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(password_connect_clicked), app);
    g_signal_connect(app->password_entry, "activate", G_CALLBACK(password_entry_activate), app);

    /* Inline PSK reveal panel */
    app->psk_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->psk_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->psk_revealer), FALSE);
    gtk_box_append(GTK_BOX(shell), app->psk_revealer);

    GtkWidget *psk_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(psk_panel, "psk-panel");
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

    app->refresh_timer_id = g_timeout_add_seconds(10, refresh_timer_cb, app);
    gtk_window_present(GTK_WINDOW(app->window));
}

static void app_shutdown(GApplication *gapp, gpointer user_data)
{
    (void)gapp;
    App *app = user_data;
    /* on_window_destroy handles timers and cancellable when close is used normally;
       guard here covers edge cases (e.g. session logout) where shutdown fires first */
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
    /* Avoid VK_SUBOPTIMAL_KHR spam when the virtual keyboard resizes the surface */
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
