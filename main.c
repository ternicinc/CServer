#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "http_server.h"
#include "router.h"
#include "logger.h"
#include "template.h"

// Global server instance for signal handling
http_server_t *global_server = NULL;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        log_info("Received shutdown signal, stopping server...");
        if (global_server) {
            http_server_stop(global_server);
        }
        exit(0);
    }
}

// Route handlers
void server_handler(http_request_t *request, http_response_t *response) {
    template_context_t *ctx = template_context_create();
    template_context_set(ctx, "title", "Server Information");
    template_context_set(ctx, "message", "Our C based server information, running this dashboard.");
    template_context_set(ctx, "version", "1.0.0");
    
    char *rendered = template_render_file("templates/server.html", ctx);
    if (rendered) {
        http_response_set_body(response, rendered);
        http_response_set_header(response, "Content-Type", "text/html");
        http_response_set_status(response, 200);
        free(rendered);
    } else {
        http_response_set_status(response, 500);
        http_response_set_body(response, "Internal Server Error");
    }
    
    template_context_destroy(ctx);
}

void maintenance_handler(http_request_t *request, http_response_t *response) {
    template_context_t *ctx = template_context_create();
    template_context_set(ctx, "root_title", "Ternic: Maintenance");
    template_context_set(ctx, "root_message", "Maintenance Mode");

    char *rendered = template_render_file("templates/maintenance.html", ctx);
    if (rendered) {
        http_response_set_body(response, rendered);
        http_response_set_header(response, "Content-Type", "text/html");
        http_response_set_status(response, 200);
        free(rendered);
    } else {
        http_response_set_status(response, 500);
        http_response_set_body(response, "Internal Server Error");
    }

    template_context_destroy(ctx);
}



void handle_api_status(http_request_t *request, http_response_t *response) {
    template_context_t *ctx = template_context_create();
    template_context_set(ctx, "status", "running");
    template_context_set(ctx, "uptime", "available");
    
    char *json_response = "{\n  \"status\": \"running\",\n  \"server\": \"Advanced C Web Server\",\n  \"version\": \"1.0.0\"\n}";
    
    http_response_set_body(response, json_response);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_set_status(response, 200);
    
    template_context_destroy(ctx);
}

void handle_post_data(http_request_t *request, http_response_t *response) {
    const char *body = http_request_get_body(request);
    
    template_context_t *ctx = template_context_create();
    template_context_set(ctx, "title", "POST Data Received");
    template_context_set(ctx, "data", body ? body : "No data received");
    
    char *rendered = template_render_file("templates/index.html", ctx);
    if (rendered) {
        http_response_set_body(response, rendered);
        http_response_set_header(response, "Content-Type", "text/html");
        http_response_set_status(response, 200);
        free(rendered);
    } else {
        http_response_set_status(response, 500);
        http_response_set_body(response, "Internal Server Error");
    }
    
    template_context_destroy(ctx);
}

int main() {
    // Initialize logger
    if (logger_init("server.log") != 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        return 1;
    }
    
    log_info("Starting Advanced C Web Server...");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create and configure server
    http_server_t *server = http_server_create("0.0.0.0", 5000);
    if (!server) {
        log_error("Failed to create server");
        return 1;
    }
    
    global_server = server;
    
    // Setup routes
    router_add_route(server->router, "GET", "/server", server_handler);
    router_add_route(server->router, "GET", "/api/status", handle_api_status);
    router_add_route(server->router, "POST", "/submit", handle_post_data);
    router_add_route(server->router, "GET", "/", maintenance_handler);
    
    // Enable static file serving
    router_add_static_route(server->router, "/static", "static");
    
    log_info("Server configured with routes:");
    log_info("  GET  / - Home page");
    log_info("  GET  /api/status - Server status API");
    log_info("  POST /submit - Handle form submissions");
    log_info("  Static files served from /static");
    
    // Start server
    if (http_server_start(server) != 0) {
        log_error("Failed to start server");
        http_server_destroy(server);
        return 1;
    }
    
    log_info("Server started successfully on http://0.0.0.0:5000");
    
    // Keep main thread alive
    while (1) {
        sleep(1);
    }
    
    // Cleanup
    http_server_destroy(server);
    logger_cleanup();
    
    return 0;
}

