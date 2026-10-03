#include "net/web_remote.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <unistd.h> // close()

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "config/network_constants.h"
#include "net/web_remote_page.h"
#include "physics/orbital_library.h"
#include "physics/slater.h"
#include "ux/element_names_it.h"
#include "ux/periodic_grid.h"
#include "ux/remote_command.h"

static const char *kWebRemoteTag = "web_remote";

namespace
{
    // ========================================================================================
    // Captive-portal DNS: every A query is answered with the soft-AP's own address, so phones
    // detect a captive portal (their connectivity probe lands on our redirect below) and
    // any hostname typed in a browser reaches the page.
    // ========================================================================================

    constexpr uint16_t kDnsPort = 53;
    constexpr int kDnsHeaderBytes = 12;
    constexpr int kDnsAnswerBytes = 16;
    constexpr uint16_t kDnsTypeA = 1;

    /// Rewrites the query in `buf` into its response in place; returns the response length,
    /// or 0 to drop the packet. Anything beyond the (single) question -- e.g. an EDNS OPT
    /// record -- is truncated off, with NSCOUNT/ARCOUNT zeroed to match.
    int buildDnsResponse(uint8_t *buf, int len, int capacity)
    {
        if (len < kDnsHeaderBytes || (buf[2] & 0x80) != 0) // too short, or already a response
            return 0;
        if (buf[4] != 0 || buf[5] != 1) // QDCOUNT != 1
            return 0;

        int pos = kDnsHeaderBytes;
        while (pos < len && buf[pos] != 0)
        {
            if ((buf[pos] & 0xC0) != 0) // compression pointer -- never valid inside a question
                return 0;
            pos += buf[pos] + 1;
        }
        pos += 1; // QNAME's terminating zero label
        if (pos + 4 > len)
            return 0;
        uint16_t qtype = uint16_t((buf[pos] << 8) | buf[pos + 1]);
        pos += 4; // QTYPE + QCLASS

        bool answer = qtype == kDnsTypeA;
        if (answer && pos + kDnsAnswerBytes > capacity)
            return 0;

        buf[2] = uint8_t(0x84 | (buf[2] & 0x01)); // QR=1, AA=1, keep the client's RD bit
        buf[3] = 0x80;                            // RA=1, RCODE=NOERROR
        buf[6] = 0;
        buf[7] = answer ? 1 : 0; // ANCOUNT (AAAA etc. get an empty NOERROR -> client falls back to A)
        std::fill(buf + 8, buf + kDnsHeaderBytes, uint8_t(0));
        if (!answer)
            return pos;

        const uint8_t record[kDnsAnswerBytes] = {
            0xC0, 0x0C,             // NAME: pointer to the question's QNAME
            0x00, 0x01, 0x00, 0x01, // TYPE A, CLASS IN
            0x00, 0x00, 0x00, 0x3C, // TTL 60s
            0x00, 0x04,             // RDLENGTH
            kWebRemoteIp[0], kWebRemoteIp[1], kWebRemoteIp[2], kWebRemoteIp[3],
        };
        std::memcpy(buf + pos, record, kDnsAnswerBytes);
        return pos + kDnsAnswerBytes;
    }

    void dnsTask(void *)
    {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kDnsPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ESP_LOGE(kWebRemoteTag, "DNS: socket/bind failed -- captive portal disabled (page still at http://%d.%d.%d.%d/)",
                     kWebRemoteIp[0], kWebRemoteIp[1], kWebRemoteIp[2], kWebRemoteIp[3]);
            if (sock >= 0)
                close(sock);
            vTaskDelete(nullptr);
            return;
        }

        uint8_t buf[512]; // classic DNS-over-UDP max message size
        while (true)
        {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
            if (len <= 0)
                continue;
            int respLen = buildDnsResponse(buf, len, sizeof(buf));
            if (respLen > 0)
                sendto(sock, buf, respLen, 0, reinterpret_cast<sockaddr *>(&from), fromLen);
        }
    }

    // ========================================================================================
    // HTTP handlers
    // ========================================================================================

    esp_err_t handlePage(httpd_req_t *req)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, kWebRemotePage, sizeof(kWebRemotePage) - 1);
    }

    /// Streamed in chunks (one per element/orbital) rather than formatted into one big
    /// buffer, so the handler's stack/heap cost stays at one short line regardless of how
    /// many elements the table grows to.
    esp_err_t handleCatalog(httpd_req_t *req)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");

        char line[96];
        std::snprintf(line, sizeof(line), "{\"maxZ\":%d,\"elements\":[", kMaxDisplayZ);
        httpd_resp_sendstr_chunk(req, line);
        for (int z = 1; z <= kMaxDisplayZ; z++)
        {
            const ElementGridPos &pos = kElementGrid[z - 1];
            std::snprintf(line, sizeof(line), "%s[%d,\"%s\",\"%s\",%d,%d]", z > 1 ? "," : "", z, elementSymbol(z),
                          elementNameIt(z), pos.row, pos.col);
            httpd_resp_sendstr_chunk(req, line);
        }

        httpd_resp_sendstr_chunk(req, "],\"orbitals\":[");
        for (int i = 0; i < kOrbitalLibraryCount; i++)
        {
            const OrbitalDescriptor &d = kOrbitalLibrary[i];
            std::snprintf(line, sizeof(line), "%s[\"%s\",%d,%d,%d]", i > 0 ? "," : "", d.label, d.n, d.ell, d.m);
            httpd_resp_sendstr_chunk(req, line);
        }
        httpd_resp_sendstr_chunk(req, "]}");
        return httpd_resp_sendstr_chunk(req, nullptr);
    }

    const char *modeName(remote::ViewMode mode)
    {
        switch (mode)
        {
        case remote::ViewMode::kElement:
            return "element";
        case remote::ViewMode::kOrbital:
            return "orbital";
        case remote::ViewMode::kMenu:
            break;
        }
        return "menu";
    }

    esp_err_t handleState(httpd_req_t *req)
    {
        remote::ViewState state = remote::currentState();
        char body[80];
        std::snprintf(body, sizeof(body), "{\"mode\":\"%s\",\"index\":%d,\"pending\":%s}", modeName(state.mode),
                      state.index, remote::pending() ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, body);
    }

    /// Parses "?c=<name>[&v=<int>]" into a range-checked request; false on anything invalid.
    bool parseCommand(httpd_req_t *req, remote::Request &out)
    {
        char query[64], name[16], value[8] = "";
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "c", name, sizeof(name)) != ESP_OK)
            return false;

        int v = -1;
        if (httpd_query_key_value(query, "v", value, sizeof(value)) == ESP_OK)
        {
            auto [end, ec] = std::from_chars(value, value + std::strlen(value), v);
            if (ec != std::errc{} || *end != '\0')
                return false;
        }

        if (std::strcmp(name, "element") == 0 && v >= 1 && v <= kMaxDisplayZ)
            out = {remote::Command::kShowElement, v};
        else if (std::strcmp(name, "orbital") == 0 && v >= 0 && v < kOrbitalLibraryCount)
            out = {remote::Command::kShowOrbital, v};
        else if (std::strcmp(name, "next") == 0)
            out = {remote::Command::kNext, 0};
        else if (std::strcmp(name, "prev") == 0)
            out = {remote::Command::kPrev, 0};
        else if (std::strcmp(name, "dissect") == 0)
            out = {remote::Command::kDissect, 0};
        else if (std::strcmp(name, "menu") == 0)
            out = {remote::Command::kMenu, 0};
        else
            return false;
        return true;
    }

    esp_err_t handleCommand(httpd_req_t *req)
    {
        remote::Request request;
        if (!parseCommand(req, request))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad command");
        ESP_LOGI(kWebRemoteTag, "command %d arg %d", int(request.cmd), request.arg);
        remote::post(request);
        return handleState(req);
    }

    /// Every unknown URL -- including the OSes' connectivity probes (/generate_204,
    /// /hotspot-detect.html, /connecttest.txt, ...) -- redirects to the page, which is what
    /// makes phones pop up their "sign in to network" sheet showing it.
    esp_err_t redirectToPage(httpd_req_t *req, httpd_err_code_t)
    {
        char location[32];
        std::snprintf(location, sizeof(location), "http://%d.%d.%d.%d/", kWebRemoteIp[0], kWebRemoteIp[1],
                      kWebRemoteIp[2], kWebRemoteIp[3]);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_send(req, nullptr, 0);
    }

    bool startHttpServer()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.core_id = kWebRemoteTaskCore;
        config.lru_purge_enable = true; // phones hold idle keep-alive sockets; recycle them instead of refusing
        httpd_handle_t server = nullptr;
        if (httpd_start(&server, &config) != ESP_OK)
            return false;

        const httpd_uri_t routes[] = {
            {.uri = "/", .method = HTTP_GET, .handler = handlePage, .user_ctx = nullptr},
            {.uri = "/api/catalog", .method = HTTP_GET, .handler = handleCatalog, .user_ctx = nullptr},
            {.uri = "/api/state", .method = HTTP_GET, .handler = handleState, .user_ctx = nullptr},
            {.uri = "/api/cmd", .method = HTTP_POST, .handler = handleCommand, .user_ctx = nullptr},
        };
        for (const httpd_uri_t &route : routes)
            httpd_register_uri_handler(server, &route);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirectToPage);
        return true;
    }

    // ========================================================================================
    // Wi-Fi soft-AP
    // ========================================================================================

    bool check(esp_err_t err, const char *what)
    {
        if (err == ESP_OK)
            return true;
        ESP_LOGE(kWebRemoteTag, "%s failed: %s -- web remote disabled", what, esp_err_to_name(err));
        return false;
    }

    bool startAccessPoint()
    {
        esp_err_t err = nvs_flash_init(); // the Wi-Fi driver keeps its calibration data in NVS
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (!check(err, "nvs_flash_init") || !check(esp_netif_init(), "esp_netif_init"))
            return false;
        err = esp_event_loop_create_default();
        if (err != ESP_ERR_INVALID_STATE && !check(err, "esp_event_loop_create_default")) // INVALID_STATE: already exists
            return false;
        if (esp_netif_create_default_wifi_ap() == nullptr)
            return check(ESP_FAIL, "esp_netif_create_default_wifi_ap");

        wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
        if (!check(esp_wifi_init(&initConfig), "esp_wifi_init") ||
            !check(esp_wifi_set_storage(WIFI_STORAGE_RAM), "esp_wifi_set_storage"))
            return false;

        wifi_config_t apConfig{};
        size_t ssidLen = std::min(std::strlen(kWebRemoteSsid), sizeof(apConfig.ap.ssid));
        std::memcpy(apConfig.ap.ssid, kWebRemoteSsid, ssidLen);
        apConfig.ap.ssid_len = uint8_t(ssidLen);
        // -1: the driver expects a NUL-terminated passphrase, and apConfig{} already zeroed it.
        std::memcpy(apConfig.ap.password, kWebRemotePassword,
                    std::min(std::strlen(kWebRemotePassword), sizeof(apConfig.ap.password) - 1));
        apConfig.ap.channel = kWebRemoteChannel;
        apConfig.ap.max_connection = kWebRemoteMaxClients;
        apConfig.ap.authmode = kWebRemotePassword[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

        return check(esp_wifi_set_mode(WIFI_MODE_AP), "esp_wifi_set_mode") &&
               check(esp_wifi_set_config(WIFI_IF_AP, &apConfig), "esp_wifi_set_config") &&
               check(esp_wifi_start(), "esp_wifi_start");
    }
} // namespace

void startWebRemote()
{
    if (!startAccessPoint())
        return;
    if (!startHttpServer())
    {
        ESP_LOGE(kWebRemoteTag, "httpd_start failed -- web remote disabled");
        return;
    }
    xTaskCreatePinnedToCore(dnsTask, "captive_dns", kWebRemoteDnsStackBytes, nullptr, tskIDLE_PRIORITY + 2, nullptr,
                            kWebRemoteTaskCore);

    ESP_LOGI(kWebRemoteTag, "Wi-Fi \"%s\" (%s) up -- open http://%d.%d.%d.%d/ from a phone", kWebRemoteSsid,
             kWebRemotePassword[0] == '\0' ? "open" : "WPA2", kWebRemoteIp[0], kWebRemoteIp[1], kWebRemoteIp[2],
             kWebRemoteIp[3]);
}
