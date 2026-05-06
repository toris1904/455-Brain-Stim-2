
#include <string.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "dhcpserver.h"
#include "dnsserver.h"
#include "wifi_hotspot.h"
#include "waveform.h"

/* Waveform channel state owned by main.c */
extern volatile WaveformChannel g_channel_1;
extern volatile WaveformChannel g_channel_2;

/* DHCP and DNS server instances — live for the lifetime of the hotspot */
static dhcp_server_t g_dhcp_server;
static dns_server_t  g_dns_server;

#define TCP_PORT 80
#define DEBUG_printf printf
#define POLL_TIME_S 5
#define HTTP_GET "GET"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 %d OK\nContent-Length: %d\nContent-Type: text/html; charset=utf-8\nConnection: close\n\n"
#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Redirect\nLocation: http://%s/\n\n"
#define CONTROL_PATH "/"
#define STATUS_PATH  "/status"

// HTML Template for Web Interface
#define HTML_BODY \
    "<html>" \
    "<head>" \
        "<title>Brain Stim</title>" \
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" \
        "<style>" \
            "*{box-sizing:border-box;}" \
            "body{" \
                "font-family:sans-serif;" \
                "display:flex;" \
                "justify-content:center;" \
                "align-items:flex-start;" \
                "padding:16px;" \
                "margin:0;" \
            "}" \
            ".card{" \
                "border:1px solid #ccc;" \
                "border-radius:8px;" \
                "padding:16px;" \
                "width:100%%;" \
                "max-width:360px;" \
            "}" \
            ".row{" \
                "display:flex;" \
                "flex-direction:column;" \
                "margin-bottom:12px;" \
            "}" \
            "label{" \
                "font-size:0.85em;" \
                "margin-bottom:4px;" \
                "color:#555;" \
            "}" \
            "input[type=number],select{" \
                "width:100%%;" \
                "padding:8px;" \
                "font-size:1em;" \
                "border:1px solid #ccc;" \
                "border-radius:4px;" \
            "}" \
            "button{" \
                "margin-top:8px;" \
                "width:100%%;" \
                "padding:12px;" \
                "font-size:1em;" \
                "cursor:pointer;" \
                "border-radius:4px;" \
                "border:none;" \
                "background:#1a73e8;" \
                "color:#fff;" \
            "}" \
        "</style>" \
    "</head>" \
    "<body>" \
        "<div class=\"card\">" \
            "<form method=\"GET\" action=\"/\">" \
                "<div class=\"row\">" \
                    "<label>Channel 1 Frequency (Hz)</label>" \
                    "<input type=\"number\" name=\"ch1val\" min=\"10\" max=\"10000\" value=\"%d\" onchange=\"this.form.submit()\">" \
                "</div>" \
                "<div class=\"row\">" \
                    "<label>Channel 1 Shape</label>" \
                    "<select name=\"ch1shape\" onchange=\"this.form.submit()\">%s</select>" \
                "</div>" \
                "<div class=\"row\">" \
                    "<label>Channel 2 Frequency (Hz)</label>" \
                    "<input type=\"number\" name=\"ch2val\" min=\"10\" max=\"10000\" value=\"%d\" onchange=\"this.form.submit()\">" \
                "</div>" \
                "<div class=\"row\">" \
                    "<label>Channel 2 Shape</label>" \
                    "<select name=\"ch2shape\" onchange=\"this.form.submit()\">%s</select>" \
                "</div>" \
                "<button id=\"tb\" type=\"submit\" name=\"toggle\" value=\"1\">%s</button>" \
            "</form>" \
        "</div>" \
    "<script>" \
    "setInterval(function(){" \
        "fetch('/status').then(function(r){return r.text();}).then(function(s){" \
            "var b=document.getElementById('tb');" \
            "if(b){b.textContent=s==='on'?'Soft Kill':'Start';}" \
        "});" \
    "},1000);" \
    "</script>" \
    "</body>" \
    "</html>"

typedef struct TCP_CONNECT_STATE_T_ {
    struct tcp_pcb *pcb;
    int sent_len;
    char headers[128];
    char result[3072];
    int header_len;
    int result_len;
    ip_addr_t *gw;
} TCP_CONNECT_STATE_T;

static err_t tcp_close_client_connection(TCP_CONNECT_STATE_T *con_state, struct tcp_pcb *client_pcb, err_t close_err) {
    if (client_pcb) {
        assert(con_state && con_state->pcb == client_pcb);
        tcp_arg(client_pcb, NULL);
        tcp_poll(client_pcb, NULL, 0);
        tcp_sent(client_pcb, NULL);
        tcp_recv(client_pcb, NULL);
        tcp_err(client_pcb, NULL);
        err_t err = tcp_close(client_pcb);
        if (err != ERR_OK) {
            DEBUG_printf("close failed %d, calling abort\n", err);
            tcp_abort(client_pcb);
            close_err = ERR_ABRT;
        }
        if (con_state) {
            free(con_state);
        }
    }
    return close_err;
}

static void tcp_server_close(TCP_SERVER_T *state) {
    if (state->server_pcb) {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    DEBUG_printf("tcp_server_sent %u\n", len);
    con_state->sent_len += len;
    if (con_state->sent_len >= con_state->header_len + con_state->result_len) {
        DEBUG_printf("all done\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    return ERR_OK;
}

//Main webpage
static int channel1_val = 2000;   // Hz, range 10-10000
static int channel2_val = 2010;   // Hz, range 10-10000
static int channel1_shape = WAVEFORM_SHAPE_SINE;
static int channel2_shape = WAVEFORM_SHAPE_SINE;

static void build_shape_options(char *buf, int buf_len, int selected) {
    static const char *names[] = { "Sine", "Triangle", "Square" };
    int pos = 0;
    for (int i = 0; i < 3; i++) {
        pos += snprintf(buf + pos, buf_len - pos,
            "<option value=\"%d\"%s>%s</option>",
            i, (i == selected) ? " selected" : "", names[i]);
    }
}

static int test_server_content(const char *request, const char *params, char *result, size_t max_result_len) {
    int len = 0;
    if (strncmp(request, STATUS_PATH, sizeof(STATUS_PATH) - 1) == 0) {
        const char *state_str = (g_device_state == DEVICE_STATE_RUNNING ||
                                 g_device_state == DEVICE_STATE_RAMPING_UP) ? "on" : "off";
        len = snprintf(result, max_result_len, "%s", state_str);
    } else if (strncmp(request, CONTROL_PATH, sizeof(CONTROL_PATH) - 1) == 0) {
        if (params) {
            // Toggle running state when button is clicked
            if (strstr(params, "toggle=1") != NULL) {
                if (g_device_state == DEVICE_STATE_OFF) {
                    device_request_start();
                } else if (g_device_state == DEVICE_STATE_RUNNING ||
                           g_device_state == DEVICE_STATE_RAMPING_UP) {
                    device_request_stop();
                }
            }
            // Update channel 1 frequency
            char *v1 = strstr(params, "ch1val=");
            if (v1) {
                int new_val = atoi(v1 + 7);
                if (new_val < 10)    new_val = 10;
                if (new_val > 10000) new_val = 10000;
                if (new_val != channel1_val) {
                    channel1_val = new_val;
                    waveform_set_frequency((WaveformChannel *)&g_channel_1, (float)channel1_val);
                    printf("Channel 1 frequency set to %d Hz\n", channel1_val);
                }
            }
            // Update channel 2 frequency
            char *v2 = strstr(params, "ch2val=");
            if (v2) {
                int new_val = atoi(v2 + 7);
                if (new_val < 10)    new_val = 10;
                if (new_val > 10000) new_val = 10000;
                if (new_val != channel2_val) {
                    channel2_val = new_val;
                    waveform_set_frequency((WaveformChannel *)&g_channel_2, (float)channel2_val);
                    printf("Channel 2 frequency set to %d Hz\n", channel2_val);
                }
            }
            // Update channel 1 shape
            char *s1 = strstr(params, "ch1shape=");
            if (s1) {
                int new_shape = atoi(s1 + 9);
                if (new_shape < 0) new_shape = 0;
                if (new_shape > 2) new_shape = 2;
                if (new_shape != channel1_shape) {
                    channel1_shape = new_shape;
                    waveform_set_shape((WaveformChannel *)&g_channel_1, (WaveformShape)channel1_shape);
                    printf("Channel 1 shape set to %d\n", channel1_shape);
                }
            }
            // Update channel 2 shape
            char *s2 = strstr(params, "ch2shape=");
            if (s2) {
                int new_shape = atoi(s2 + 9);
                if (new_shape < 0) new_shape = 0;
                if (new_shape > 2) new_shape = 2;
                if (new_shape != channel2_shape) {
                    channel2_shape = new_shape;
                    waveform_set_shape((WaveformChannel *)&g_channel_2, (WaveformShape)channel2_shape);
                    printf("Channel 2 shape set to %d\n", channel2_shape);
                }
            }
        }
        char shape_opts1[192], shape_opts2[192];
        build_shape_options(shape_opts1, sizeof(shape_opts1), channel1_shape);
        build_shape_options(shape_opts2, sizeof(shape_opts2), channel2_shape);
        len = snprintf(result, max_result_len, HTML_BODY,
            channel1_val,
            shape_opts1,
            channel2_val,
            shape_opts2,
            (g_device_state == DEVICE_STATE_RUNNING ||
             g_device_state == DEVICE_STATE_RAMPING_UP) ? "Soft Kill" : "Start");
    }
    return len;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (!p) {
        DEBUG_printf("connection closed\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    assert(con_state && con_state->pcb == pcb);
    if (p->tot_len > 0) {
        DEBUG_printf("tcp_server_recv %d err %d\n", p->tot_len, err);
#if 0
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            DEBUG_printf("in: %.*s\n", q->len, q->payload);
        }
#endif
        // Copy the request into the buffer
        pbuf_copy_partial(p, con_state->headers, p->tot_len > sizeof(con_state->headers) - 1 ? sizeof(con_state->headers) - 1 : p->tot_len, 0);

        // Handle GET request
        if (strncmp(HTTP_GET, con_state->headers, sizeof(HTTP_GET) - 1) == 0) {
            char *request = con_state->headers + sizeof(HTTP_GET); // + space
            char *params = strchr(request, '?');
            if (params) {
                if (*params) {
                    char *space = strchr(request, ' ');
                    *params++ = 0;
                    if (space) {
                        *space = 0;
                    }
                } else {
                    params = NULL;
                }
            }

            // Generate content
            con_state->result_len = test_server_content(request, params, con_state->result, sizeof(con_state->result));
            DEBUG_printf("Request: %s?%s\n", request, params);
            DEBUG_printf("Result: %d\n", con_state->result_len);

            // Check we had enough buffer space
            if (con_state->result_len > sizeof(con_state->result) - 1) {
                DEBUG_printf("Too much result data %d\n", con_state->result_len);
                return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
            }

            // Generate web page
            if (con_state->result_len > 0) {
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS,
                    200, con_state->result_len);
                if (con_state->header_len > sizeof(con_state->headers) - 1) {
                    DEBUG_printf("Too much header data %d\n", con_state->header_len);
                    return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
                }
            } else {
                // Send redirect
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_REDIRECT,
                    ipaddr_ntoa(con_state->gw));
                DEBUG_printf("Sending redirect %s", con_state->headers);
            }

            // Send the headers to the client
            con_state->sent_len = 0;
            err_t err = tcp_write(pcb, con_state->headers, con_state->header_len, 0);
            if (err != ERR_OK) {
                DEBUG_printf("failed to write header data %d\n", err);
                return tcp_close_client_connection(con_state, pcb, err);
            }

            // Send the body to the client
            if (con_state->result_len) {
                err = tcp_write(pcb, con_state->result, con_state->result_len, 0);
                if (err != ERR_OK) {
                    DEBUG_printf("failed to write result data %d\n", err);
                    return tcp_close_client_connection(con_state, pcb, err);
                }
            }
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *pcb) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    DEBUG_printf("tcp_server_poll_fn\n");
    return tcp_close_client_connection(con_state, pcb, ERR_OK); // Just disconnect clent?
}

static void tcp_server_err(void *arg, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_close_client_connection(con_state, con_state->pcb, err);
    }
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("failure in accept\n");
        return ERR_VAL;
    }
    DEBUG_printf("client connected\n");

    // Create the state for the connection
    TCP_CONNECT_STATE_T *con_state = calloc(1, sizeof(TCP_CONNECT_STATE_T));
    if (!con_state) {
        DEBUG_printf("failed to allocate connect state\n");
        return ERR_MEM;
    }
    con_state->pcb = client_pcb; // for checking
    con_state->gw = &state->gw;

    // setup connection to client
    tcp_arg(client_pcb, con_state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);

    return ERR_OK;
}

static bool tcp_server_open(void *arg, const char *ap_name) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    DEBUG_printf("starting server on port %d\n", TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT);
    if (err) {
        DEBUG_printf("failed to bind to port %d\n",TCP_PORT);
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        DEBUG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    printf("Try connecting to '%s' (press 'd' to disable access point)\n", ap_name);
    return true;
}

void key_pressed_func(void *param) {
    assert(param);
    TCP_SERVER_T *state = (TCP_SERVER_T*)param;
    int key = getchar_timeout_us(0); // get any pending key press but don't wait
    if (key == 'd' || key == 'D') {
        cyw43_arch_lwip_begin();
        cyw43_arch_disable_ap_mode();
        cyw43_arch_lwip_end();
        state->complete = true;
    }
}

/* --------------------------------------------------------------------------
 * wifi_hotspot_start
 *
 * Enables AP mode, starts DHCP/DNS servers, and opens the HTTP server.
 * cyw43_arch_init() must already have been called by the caller (main.c).
 * Returns true on success, false on any failure.
 * -------------------------------------------------------------------------- */
bool wifi_hotspot_start(TCP_SERVER_T *state) {
    // Set initial frequencies
    waveform_set_frequency((WaveformChannel *)&g_channel_1, (float)channel1_val);
    waveform_set_frequency((WaveformChannel *)&g_channel_2, (float)channel2_val);

    // Register key-press callback so 'd' shuts down the AP
    stdio_set_chars_available_callback(key_pressed_func, state);

    const char *ap_name = "picow_test";
    const char *password = "password";

    cyw43_arch_enable_ap_mode(ap_name, password, CYW43_AUTH_WPA2_AES_PSK);

    #if LWIP_IPV6
    #define IP(x) ((x).u_addr.ip4)
    #else
    #define IP(x) (x)
    #endif

    ip4_addr_t mask;
    IP(state->gw).addr = PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
    IP(mask).addr = PP_HTONL(CYW43_DEFAULT_IP_MASK);

    #undef IP

    dhcp_server_init(&g_dhcp_server, &state->gw, &mask);
    dns_server_init(&g_dns_server, &state->gw);

    if (!tcp_server_open(state, ap_name)) {
        DEBUG_printf("failed to open server\n");
        return false;
    }

    state->complete = false;
    return true;
}

/* --------------------------------------------------------------------------
 * wifi_hotspot_stop
 *
 * Tears down the HTTP, DNS, and DHCP servers.
 * Does NOT call cyw43_arch_deinit() — the caller handles that.
 * -------------------------------------------------------------------------- */
void wifi_hotspot_stop(TCP_SERVER_T *state) {
    tcp_server_close(state);
    dns_server_deinit(&g_dns_server);
    dhcp_server_deinit(&g_dhcp_server);
}