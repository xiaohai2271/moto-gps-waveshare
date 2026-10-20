#include "wifi_health_server.h"

#include "esp_http_server.h"
#include "esp_log.h"

namespace moto::wifi {
namespace {

constexpr char kTag[] = "wifi_health";
constexpr char kHealthBody[] = "{\"ok\":true,\"fw\":\"moto-gps\"}";

httpd_handle_t g_server = nullptr;

esp_err_t health_handler(httpd_req_t* request) {
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(request, kHealthBody, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

esp_err_t start_health_endpoint() {
  if (g_server != nullptr) {
    return ESP_OK;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 2;
  esp_err_t result = httpd_start(&g_server, &config);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "httpd start failed: %s", esp_err_to_name(result));
    g_server = nullptr;
    return result;
  }
  httpd_uri_t uri{};
  uri.uri = "/health";
  uri.method = HTTP_GET;
  uri.handler = health_handler;
  httpd_register_uri_handler(g_server, &uri);
  ESP_LOGI(kTag, "health endpoint listening on port 80");
  return ESP_OK;
}

void stop_health_endpoint() {
  if (g_server == nullptr) {
    return;
  }
  httpd_stop(g_server);
  g_server = nullptr;
}

}  // namespace moto::wifi
