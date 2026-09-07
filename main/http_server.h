#pragma once

#include "esp_http_server.h"

/**
 * @brief Start the HTTP server and register all URI handlers.
 *
 * @return Server handle on success, NULL on failure.
 */
httpd_handle_t http_server_start(void);

/**
 * @brief Stop the HTTP server and free its resources.
 */
void http_server_stop(httpd_handle_t server);