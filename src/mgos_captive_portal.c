/*
 * Copyright (c) 2018 Myles McNamara
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <string.h>

#include "mgos_utils.h"
#include "mgos_timers.h"
#include "mgos_config.h"
#include "mgos_mongoose.h"
#include "mgos_captive_portal.h"
#include "mongoose.h"

#if CS_PLATFORM == CS_P_ESP8266
#include "user_interface.h"
#endif

static const char *s_ap_ip = "192.168.4.1";
static const char *s_portal_hostname = "setup.device.local";
static const char *s_listening_addr = "udp://:53";
static const char *s_portal_index_file = "index.html";

static int s_captive_portal_init = 0;

static struct mg_serve_http_opts s_http_server_opts;

char *get_redirect_url(void){
    static char redirect_url[256];
    // Set URI as HTTPS if ssl cert configured, otherwise use http
    c_snprintf(redirect_url, sizeof redirect_url, "%s://%s", (mgos_sys_config_get_http_ssl_cert() ? "https" : "http"), s_portal_hostname);
    return redirect_url;
}

static void send_redirect_html_generated2(struct mg_connection *nc, int status_code,
                           const struct mg_str location,
                           const struct mg_str extra_headers) {
  char bbody[100], *pbody = bbody;
  int bl = mg_asprintf(&pbody, sizeof(bbody),
                       "<head><meta http-equiv='refresh' content='0; url=%.*s'></head><body><p>Click <a href='%.*s'>here</a> to login.</p></body>\r\n",
                       (int) location.len, location.p, (int) location.len, location.p );
  char bhead[150], *phead = bhead;
  mg_asprintf(&phead, sizeof(bhead),
              "Location: %.*s\r\n"
              "Content-Type: text/html\r\n"
              "Content-Length: %d\r\n"
              "Cache-Control: no-cache\r\n"
              "%.*s%s",
              (int) location.len, location.p, bl, (int) extra_headers.len,
              extra_headers.p, (extra_headers.len > 0 ? "\r\n" : ""));
  mg_send_response_line(nc, status_code, phead);
  if (phead != bhead) free(phead);
  mg_send(nc, pbody, bl);
  if (pbody != bbody) free(pbody);
}

static void send_redirect_html_generated(struct mg_connection *nc, int status_code, const struct mg_str location ) {
  char bbody[100], *pbody = bbody;
  int bl = mg_asprintf(&pbody, sizeof(bbody),
                       "<head><meta http-equiv='refresh' content='0; url=%.*s'></head><body><p>Click <a href='%.*s'>here</a> to login.</p></body>\r\n",
                       (int) location.len, location.p, (int) location.len, location.p );
  mg_send_head(nc, status_code, bl, "Cache-Control: no-cache" );
  mg_send(nc, pbody, bl);
  if (pbody != bbody) free(pbody);
}

static void redirect_ev_handler(struct mg_connection *nc, int ev, void *ev_data, void *user_data){

    if (ev != MG_EV_HTTP_REQUEST)
        return;

    char *redirect_url = get_redirect_url();

    LOG(LL_INFO, ("Redirecting to %s for Captive Portal", redirect_url ) );
    mg_http_send_redirect(nc, 302, mg_mk_str(redirect_url), mg_mk_str(NULL));

    (void)ev_data;
    (void)user_data;
}

static void serve_redirect_ev_handler(struct mg_connection *nc, int ev, void *ev_data, void *user_data){

    if (ev != MG_EV_HTTP_REQUEST)
        return;

    char *redirect_url = get_redirect_url();

    LOG(LL_INFO, ("Serving Redirect HTML to %s for Captive Portal", redirect_url ) );
    send_redirect_html_generated( nc, 200, mg_mk_str(redirect_url) );

    (void)ev_data;
    (void)user_data;
}

static void dns_ev_handler(struct mg_connection *c, int ev, void *ev_data,
                                    void *user_data){
    struct mg_dns_message *msg = (struct mg_dns_message *)ev_data;
    struct mbuf reply_buf;
    int i;

    if (ev != MG_DNS_MESSAGE)
        return;

    mbuf_init(&reply_buf, 512);
    struct mg_dns_reply reply = mg_dns_create_reply(&reply_buf, msg);
    for (i = 0; i < msg->num_questions; i++)
    {
        char rname[256];
        struct mg_dns_resource_record *rr = &msg->questions[i];
        mg_dns_uncompress_name(msg, &rr->name, rname, sizeof(rname) - 1);
        // LOG( LL_INFO, ( "Q type %d name %s\n", rr->rtype, rname ) );
        if (rr->rtype == MG_DNS_A_RECORD)
        {
            LOG(LL_DEBUG, ("DNS A Query for %s sending IP %s", rname, s_ap_ip));
            uint32_t ip = inet_addr(s_ap_ip);
            mg_dns_reply_record(&reply, rr, NULL, rr->rtype, 10, &ip, 4);
        }
    }
    mg_dns_send_reply(c, &reply);
    mbuf_free(&reply_buf);
    (void)user_data;
}

static void http_msg_print(const struct http_message *msg){
    // LOG(LL_INFO, ("     message: \"%.*s\"\n", msg->message.len, msg->message.p));
    LOG(LL_DEBUG, ("      method: \"%.*s\"", msg->method.len, msg->method.p));
    LOG(LL_DEBUG, ("         uri: \"%.*s\"", msg->uri.len, msg->uri.p));
}

static void root_handler(struct mg_connection *nc, int ev, void *p, void *user_data){
    (void)user_data;
    if (ev != MG_EV_HTTP_REQUEST)
        return;

    struct http_message *msg = (struct http_message *)(p);
    http_msg_print(msg);

    // Init our http server options (set in mgos_captive_portal_start)
    struct mg_serve_http_opts opts;
    memcpy(&opts, &s_http_server_opts, sizeof(opts));

    // Check Host header for our hostname (to serve captive portal)
    struct mg_str *hhdr = mg_get_http_header(msg, "Host");

    if (hhdr != NULL && strstr(hhdr->p, s_portal_hostname) != NULL){
        // TODO: check Accept-Encoding header for gzip before serving gzip
        LOG(LL_INFO, ("Root Handler -- Host matches Captive Portal Host \n"));
        // Check if gzip file was requested
        struct mg_str uri = mg_mk_str_n(msg->uri.p, msg->uri.len);
        bool gzip = strncmp(uri.p + uri.len - 3, ".gz", 3) == 0;
        // Check if URI is root directory --  /wifi_portal.min.js.gz HTTP/1.1
        bool uriroot = strncmp(uri.p, "/ HTTP", 6) == 0;

        // If gzip file requested -- set Content-Encoding
        if (gzip){
            LOG(LL_INFO, ("Root Handler -- gzip Asset Requested -- Adding Content-Encoding Header \n"));
            opts.extra_headers = "Content-Encoding: gzip";
        }

        if (uriroot){
            LOG(LL_INFO, ("\nRoot Handler -- Captive Portal Root Requested\n"));

            opts.index_files = s_portal_index_file;

            if( gzip ){
                LOG(LL_INFO, ("Root Handler -- Captive Portal Serving GZIP HTML \n"));
                mg_http_serve_file(nc, msg, s_portal_index_file, mg_mk_str("text/html"), mg_mk_str("Content-Encoding: gzip"));
                return;
            } else {
                LOG(LL_INFO, ("Root Handler -- Captive Portal Serving HTML \n"));
                mg_http_serve_file(nc, msg, s_portal_index_file, mg_mk_str("text/html"), mg_mk_str("Access-Control-Allow-Origin: *"));
                return;
            }

        } else {
            LOG(LL_DEBUG, ("\n Not URI Root, Actual: %s - %d\n", uri.p, uriroot));
        }

    } else {

        LOG(LL_INFO, ("Root Handler -- Checking for CaptivePortal UserAgent"));

        // Check User-Agent string for "CaptiveNetworkSupport" to issue redirect (AFTER checking for Captive Portal Host)
        struct mg_str *uahdr = mg_get_http_header(msg, "User-Agent");
        if (uahdr != NULL){
            // LOG(LL_INFO, ("Root Handler -- Found USER AGENT: %s \n", uahdr->p));

            if (strstr(uahdr->p, "CaptiveNetworkSupport") != NULL){
                LOG(LL_INFO, ("Root Handler -- Found USER AGENT CaptiveNetworkSupport -- Sending Redirect!\n"));
                redirect_ev_handler(nc, ev, p, user_data);
                return;
            }
        }
    }

    // Serve non-root requested file
    mg_serve_http(nc, msg, opts);
}

bool mgos_captive_portal_start(void){

    if ( s_captive_portal_init ){
        LOG(LL_ERROR, ("Captive portal already init! Ignoring call to start captive portal!"));
        return false;
    }

    LOG(LL_INFO, ("Starting Captive Portal..."));

#if CS_PLATFORM == CS_P_ESP8266
    int on = 1;
    wifi_softap_set_dhcps_offer_option(OFFER_ROUTER, &on);
#endif
    /*
     *    TODO:
     *    Maybe need to figure out way to handle DNS for captive portal, if user has defined AP hostname,
     *    as WiFi lib automatically sets up it's own DNS responder for the hostname when one is set
     */
    // if (mgos_sys_config_get_wifi_ap_enable() && mgos_sys_config_get_wifi_ap_hostname() != NULL) {
    // }

    // Set IP address to respond to DNS queries with
    s_ap_ip = mgos_sys_config_get_wifi_ap_ip();
    // Set Hostname used for serving DNS captive portal
    s_portal_hostname = mgos_sys_config_get_cportal_hostname();
    s_portal_index_file = mgos_sys_config_get_cportal_index();

    // Bind DNS for Captive Portal
    struct mg_connection *dns_c = mg_bind(mgos_get_mgr(), s_listening_addr, dns_ev_handler, 0);
    mg_set_protocol_dns(dns_c);

    if (dns_c == NULL){
        LOG(LL_ERROR, ("Failed to initialize DNS listener"));
        return false;
    } else {
        LOG(LL_INFO, ("Captive Portal DNS Listening on %s", s_listening_addr));
    }

    // GZIP handling
    memset(&s_http_server_opts, 0, sizeof(s_http_server_opts));
    // s_http_server_opts.document_root = mgos_sys_config_get_http_document_root();
    s_http_server_opts.document_root = "/";
    // Add GZIP mime types for HTML, JavaScript, and CSS files
    s_http_server_opts.custom_mime_types = ".html.gz=text/html; charset=utf-8,.js.gz=application/javascript; charset=utf-8,.css.gz=text/css; charset=utf-8";
    // CORS
    s_http_server_opts.extra_headers = "Access-Control-Allow-Origin: *";

    /**
     * Root handler to check for User-Agent captive portal support, check for our redirect hostname to serve portal HTML file,
     * and to serve CSS and JS assets to client (after matching hostname in Host header)
     */
    mgos_register_http_endpoint("/", root_handler, NULL);

    // captive.apple.com - DNS request for Mac OSX
    
    // Known HTTP GET requests to check for Captive Portal
    mgos_register_http_endpoint("/mobile/status.php", serve_redirect_ev_handler, NULL);         // Android 8.0 (Samsung s9+)
    mgos_register_http_endpoint("/generate_204", serve_redirect_ev_handler, NULL);              // Android
    mgos_register_http_endpoint("/gen_204", redirect_ev_handler, NULL);                   // Android 9.0
    mgos_register_http_endpoint("/ncsi.txt", redirect_ev_handler, NULL);                  // Windows
    mgos_register_http_endpoint("/success.txt", redirect_ev_handler, NULL);       // OSX
    mgos_register_http_endpoint("/hotspot-detect.html", redirect_ev_handler, NULL);       // iOS 8/9
    mgos_register_http_endpoint("/hotspotdetect.html", redirect_ev_handler, NULL);       // iOS 8/9
    mgos_register_http_endpoint("/library/test/success.html", redirect_ev_handler, NULL); // iOS 8/9
    mgos_register_http_endpoint("/kindle-wifi/wifistub.html", redirect_ev_handler, NULL); // iOS 8/9

    s_captive_portal_init = true;

    return true;
}

bool mgos_captive_portal_init(void){

    // Check if config is set to enable captive portal on boot
    if (mgos_sys_config_get_cportal_enable()){
        mgos_captive_portal_start();
    }

    return true;
}