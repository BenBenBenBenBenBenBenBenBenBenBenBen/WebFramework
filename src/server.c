#include <microhttpd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define PORT 8888
#define HTML "templates/index.html"
#define CSS "static/style.css"

typedef struct {
    const char* key;
    const char* value;
} TemplateVar;

size_t htmlSize;

const char* lookup(const char* key, TemplateVar* vars, size_t var_count) {
    for (size_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].key, key) == 0) {
            return vars[i].value;
        }
    }
    return ""; // default to empty string if key not found
}

char* render_template(const char* template, TemplateVar* vars, size_t var_count) {
    size_t result_size = htmlSize + 1;
    char* result = malloc(result_size);
    result[0] = '\0';

    const char* p = template;
    while (*p) {
        const char* start = strstr(p, "{{");
        if (!start) {
            strcat(result, p); // no more placeholders
            break;
        }

        // Append text before {{
        strncat(result, p, start - p);

        const char* end = strstr(start, "}}");
        if (!end) break; // malformed template

        // Extract key
        size_t key_len = end - (start + 2);
        char key[256] = {0}; // assuming key length < 256
        strncpy(key, start + 2, key_len);

        // Look up replacement
        const char* replacement = lookup(key, vars, var_count);

        // Resize result buffer if needed
        htmlSize = strlen(result) + strlen(replacement) + strlen(end + 2) + 1;
        if (htmlSize > result_size) {
            result_size = htmlSize * 2;
            result = realloc(result, result_size);
        }

        strcat(result, replacement);

        p = end + 2; // move past }}
    }

    result[htmlSize - 1] = '\0';

    return result;
}

static enum MHD_Result send_response(struct MHD_Connection *connection, const char *mime, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file)
        return MHD_NO;

    fseek(file, 0, SEEK_END);
    htmlSize = ftell(file);
    rewind(file);

    char *data = malloc(htmlSize);
    if (!data) {
        fclose(file);
        return MHD_NO;
    }

    fread(data, 1, htmlSize, file);
    fclose(file);
    
    TemplateVar vars[] = {{"title", "Hello World!"}};

    char* rendered_template = render_template(data, vars, 1);

    struct MHD_Response *response = MHD_create_response_from_buffer(htmlSize, rendered_template, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", mime);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version, const char *upload_data,
                                            size_t *upload_data_size, void **con_cls) {
    if (strcmp(method, "GET") != 0)
        return MHD_NO;

    // Serve root page
    if (strcmp(url, "/") == 0) {
        return send_response(connection, "text/html", "templates/index.html");
    }

    // Serve static files (e.g., /static/style.css)
    if (strncmp(url, "/static/", 8) == 0) {
        char path[256];
        snprintf(path, sizeof(path), ".%s", url);  // becomes ./static/...
        // Guess MIME type
        const char *ext = strrchr(url, '.');
        const char *mime = "text/plain";

        if (ext) {
            if (strcmp(ext, ".css") == 0) mime = "text/css";
            else if (strcmp(ext, ".js") == 0) mime = "application/javascript";
            else if (strcmp(ext, ".png") == 0) mime = "image/png";
            else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) mime = "image/jpeg";
            else if (strcmp(ext, ".svg") == 0) mime = "image/svg+xml";
            else if (strcmp(ext, ".woff") == 0) mime = "font/woff";
            else if (strcmp(ext, ".woff2") == 0) mime = "font/woff2";
            else if (strcmp(ext, ".html") == 0) mime = "text/html";
        }

        return send_response(connection, mime, path);
    }

    // Serve HTML pages from templates/ (e.g., /about.html → templates/about.html)
    if (strstr(url, ".html") != NULL) {
        char path[256];
        snprintf(path, sizeof(path), "templates%s", url);
        return send_response(connection, "text/html", path);
    }

    // Not found
    const char *not_found = "404 Not Found";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(not_found),
                                                                    (void *)not_found,
                                                                    MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return ret;
}

int main() {
    struct MHD_Daemon *daemon;

    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, PORT, NULL, NULL,
                              &answer_to_connection, NULL, MHD_OPTION_END);
    if (NULL == daemon)
        return 1;

    printf("Server running on http://localhost:%d\n", PORT);
    getchar();  // Wait for user to press Enter

    MHD_stop_daemon(daemon);
    return 0;
}

