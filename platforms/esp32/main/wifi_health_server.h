#pragma once

// Minimal health endpoint for P4 provisioning bring-up (docs §6.6): serves
// GET /health with {"ok":true,"fw":"..."} so the phone can verify that the
// device is reachable over the local network.

#include "esp_err.h"

namespace moto::wifi {

// Starts the health endpoint once the station has an IP. Safe to call again
// when the IP changes (the server rebinds automatically on the new address).
esp_err_t start_health_endpoint();

// Stops the endpoint (optional; used on forget/disconnect to free the socket).
void stop_health_endpoint();

}  // namespace moto::wifi
