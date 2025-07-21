#include "http_server.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <strings.h>

// Status code strings
static const char *status_strings[] = {
    [200] = "OK",
    [201] = "Created",
    [400] = "Bad Request",
    [404] = "Not Found",
    [405] = "Method Not Allowed",
    [500] = "Internal Server Error"
};

http_server_t *http_server_create(const char *host, int port) {
    http_server_t *server = malloc(sizeof(http_server_t));
    if (!server) {
        log_error("Failed to allocate memory for server");
        return NULL;
    }
    
    strncpy(server->host, host, sizeof(server->host) - 1);
    server->host[sizeof(server->host) - 1] = '\0';
    server->port = port;
    server->socket_fd = -1;
    server->running = 0;
    
    // Initialize router
    server->router = router_create();
    if (!server->router) {
        log_error("Failed to create router");
        free(server);
        return NULL;
    }
    
    // Initialize mutex
    if (pthread_mutex_init(&server->mutex, NULL) != 0) {
        log_error("Failed to initialize mutex");
        router_destroy(server->router);
        free(server);
        return NULL;
    }
    
    return server;
}

int http_server_start(http_server_t *server) {
    if (!server) return -1;
    
    // Create socket
    server->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket_fd < 0) {
        log_error("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("Failed to set socket options: %s", strerror(errno));
        close(server->socket_fd);
        return -1;
    }
    
    // Configure address
    memset(&server->address, 0, sizeof(server->address));
    server->address.sin_family = AF_INET;
    server->address.sin_port = htons(server->port);
    
    if (inet_pton(AF_INET, server->host, &server->address.sin_addr) <= 0) {
        log_error("Invalid address: %s", server->host);
        close(server->socket_fd);
        return -1;
    }
    
    // Bind socket
    if (bind(server->socket_fd, (struct sockaddr *)&server->address, sizeof(server->address)) < 0) {
        log_error("Failed to bind socket: %s", strerror(errno));
        close(server->socket_fd);
        return -1;
    }
    
    // Listen for connections
    if (listen(server->socket_fd, 10) < 0) {
        log_error("Failed to listen on socket: %s", strerror(errno));
        close(server->socket_fd);
        return -1;
    }
    
    server->running = 1;
    
    // Start accepting connections in a separate thread
    pthread_t accept_thread;
    if (pthread_create(&accept_thread, NULL, (void *)accept_connections, server) != 0) {
        log_error("Failed to create accept thread");
        server->running = 0;
        close(server->socket_fd);
        return -1;
    }
    
    pthread_detach(accept_thread);
    
    return 0;
}

void *accept_connections(void *arg) {
    http_server_t *server = (http_server_t *)arg;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (server->running) {
        int client_socket = accept(server->socket_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            if (server->running) {
                log_error("Failed to accept connection: %s", strerror(errno));
            }
            continue;
        }
        
        // Create thread to handle client
        pthread_t client_thread;
        client_handler_args_t *args = malloc(sizeof(client_handler_args_t));
        if (!args) {
            log_error("Failed to allocate memory for client handler args");
            close(client_socket);
            continue;
        }
        
        args->server = server;
        args->client_socket = client_socket;
        
        if (pthread_create(&client_thread, NULL, handle_client, args) != 0) {
            log_error("Failed to create client thread");
            free(args);
            close(client_socket);
            continue;
        }
        
        pthread_detach(client_thread);
    }
    
    return NULL;
}

void *handle_client(void *args) {
    client_handler_args_t *handler_args = (client_handler_args_t *)args;
    http_server_t *server = handler_args->server;
    int client_socket = handler_args->client_socket;
    
    char buffer[MAX_REQUEST_SIZE];
    ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received < 0) {
        log_error("Failed to receive data from client");
        goto cleanup;
    }
    
    buffer[bytes_received] = '\0';
    
    // Parse HTTP request
    http_request_t *request = http_request_create();
    if (!request) {
        log_error("Failed to create HTTP request");
        goto cleanup;
    }
    
    if (http_parse_request(buffer, request) != 0) {
        log_error("Failed to parse HTTP request");
        http_request_destroy(request);
        goto cleanup;
    }
    
    log_info("Request: %s %s", request->method, request->path);
    
    // Create response
    http_response_t *response = http_response_create();
    if (!response) {
        log_error("Failed to create HTTP response");
        http_request_destroy(request);
        goto cleanup;
    }
    
    // Route request
    router_handle_request(server->router, request, response);
    
    // Serialize and send response
    char *response_str = http_serialize_response(response);
    if (response_str) {
        send(client_socket, response_str, strlen(response_str), 0);
        free(response_str);
    }
    
    // Cleanup
    http_request_destroy(request);
    http_response_destroy(response);
    
cleanup:
    close(client_socket);
    free(handler_args);
    return NULL;
}

void http_server_stop(http_server_t *server) {
    if (server) {
        server->running = 0;
        if (server->socket_fd >= 0) {
            close(server->socket_fd);
        }
    }
}

void http_server_destroy(http_server_t *server) {
    if (server) {
        http_server_stop(server);
        router_destroy(server->router);
        pthread_mutex_destroy(&server->mutex);
        free(server);
    }
}

// Request functions
http_request_t *http_request_create(void) {
    http_request_t *request = malloc(sizeof(http_request_t));
    if (request) {
        memset(request, 0, sizeof(http_request_t));
    }
    return request;
}

void http_request_destroy(http_request_t *request) {
    if (request) {
        if (request->body) {
            free(request->body);
        }
        free(request);
    }
}

const char *http_request_get_header(http_request_t *request, const char *name) {
    if (!request || !name) return NULL;
    
    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

const char *http_request_get_body(http_request_t *request) {
    return request ? request->body : NULL;
}

// Response functions
http_response_t *http_response_create(void) {
    http_response_t *response = malloc(sizeof(http_response_t));
    if (response) {
        memset(response, 0, sizeof(http_response_t));
        response->status_code = 200;
    }
    return response;
}

void http_response_destroy(http_response_t *response) {
    if (response) {
        if (response->body) {
            free(response->body);
        }
        free(response);
    }
}

void http_response_set_status(http_response_t *response, int status) {
    if (response) {
        response->status_code = status;
    }
}

void http_response_set_header(http_response_t *response, const char *name, const char *value) {
    if (!response || !name || !value || response->header_count >= MAX_HEADERS) {
        return;
    }
    
    // Check if header already exists
    for (int i = 0; i < response->header_count; i++) {
        if (strcasecmp(response->headers[i].name, name) == 0) {
            strncpy(response->headers[i].value, value, sizeof(response->headers[i].value) - 1);
            response->headers[i].value[sizeof(response->headers[i].value) - 1] = '\0';
            return;
        }
    }
    
    // Add new header
    strncpy(response->headers[response->header_count].name, name, sizeof(response->headers[response->header_count].name) - 1);
    response->headers[response->header_count].name[sizeof(response->headers[response->header_count].name) - 1] = '\0';
    
    strncpy(response->headers[response->header_count].value, value, sizeof(response->headers[response->header_count].value) - 1);
    response->headers[response->header_count].value[sizeof(response->headers[response->header_count].value) - 1] = '\0';
    
    response->header_count++;
}

void http_response_set_body(http_response_t *response, const char *body) {
    if (!response) return;
    
    if (response->body) {
        free(response->body);
        response->body = NULL;
        response->body_length = 0;
    }
    
    if (body) {
        response->body_length = strlen(body);
        response->body = malloc(response->body_length + 1);
        if (response->body) {
            strcpy(response->body, body);
        }
    }
}

// HTTP parsing
int http_parse_request(const char *raw_request, http_request_t *request) {
    if (!raw_request || !request) return -1;
    
    char *request_copy = strdup(raw_request);
    if (!request_copy) return -1;
    
    char *lines = request_copy;
    char *line = strtok_r(lines, "\r\n", &lines);
    
    if (!line) {
        free(request_copy);
        return -1;
    }
    
    // Parse request line
    char *method = strtok(line, " ");
    char *path = strtok(NULL, " ");
    char *version = strtok(NULL, " ");
    
    if (!method || !path || !version) {
        free(request_copy);
        return -1;
    }
    
    strncpy(request->method, method, sizeof(request->method) - 1);
    request->method[sizeof(request->method) - 1] = '\0';
    
    strncpy(request->path, path, sizeof(request->path) - 1);
    request->path[sizeof(request->path) - 1] = '\0';
    
    strncpy(request->version, version, sizeof(request->version) - 1);
    request->version[sizeof(request->version) - 1] = '\0';
    
    // Parse headers
    while ((line = strtok_r(lines, "\r\n", &lines)) && strlen(line) > 0 && request->header_count < MAX_HEADERS) {
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *name = trim_whitespace(line);
            char *value = trim_whitespace(colon + 1);
            
            strncpy(request->headers[request->header_count].name, name, sizeof(request->headers[request->header_count].name) - 1);
            request->headers[request->header_count].name[sizeof(request->headers[request->header_count].name) - 1] = '\0';
            
            strncpy(request->headers[request->header_count].value, value, sizeof(request->headers[request->header_count].value) - 1);
            request->headers[request->header_count].value[sizeof(request->headers[request->header_count].value) - 1] = '\0';
            
            request->header_count++;
        }
    }
    
    // Parse body if present
    if (lines && strlen(lines) > 0) {
        request->body_length = strlen(lines);
        request->body = malloc(request->body_length + 1);
        if (request->body) {
            strcpy(request->body, lines);
        }
    }
    
    free(request_copy);
    return 0;
}

char *http_serialize_response(http_response_t *response) {
    if (!response) return NULL;
    
    const char *status_text = status_strings[response->status_code];
    if (!status_text) status_text = "Unknown";
    
    // Calculate total size needed
    size_t total_size = 1024; // Initial buffer for status line and headers
    if (response->body) {
        total_size += response->body_length;
    }
    
    char *buffer = malloc(total_size);
    if (!buffer) return NULL;
    
    // Status line
    int offset = snprintf(buffer, total_size, "HTTP/1.1 %d %s\r\n", response->status_code, status_text);
    
    // Headers
    for (int i = 0; i < response->header_count; i++) {
        offset += snprintf(buffer + offset, total_size - offset, "%s: %s\r\n",
                          response->headers[i].name, response->headers[i].value);
    }
    
    // Content-Length header if body is present
    if (response->body) {
        offset += snprintf(buffer + offset, total_size - offset, "Content-Length: %zu\r\n", response->body_length);
    }
    
    // End of headers
    offset += snprintf(buffer + offset, total_size - offset, "\r\n");
    
    // Body
    if (response->body) {
        memcpy(buffer + offset, response->body, response->body_length);
        offset += response->body_length;
    }
    
    buffer[offset] = '\0';
    return buffer;
}

