#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define PORT 8888
#define HTML "templates/index.html"
#define Lua "templates/template.lua"

typedef struct {
    const char* key;
    const char* value;
} TemplateVar;

typedef struct {
    int length;
    TemplateVar *pairs;
} TemplateList;

size_t htmlSize;

TemplateList templateList;

int update_template_data(lua_State* L);

lua_State* init_lua();

int receive_kv(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);

    printf("Received key: %s, value: %s\n", key, value);

    return 0;
}

const char* lookup(const char* key, TemplateVar* vars, size_t var_count) {
    for (size_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].key, key) == 0) {
            return vars[i].value;
        }
    }
    return "";
}

char* render_template(const char* template, TemplateVar* vars, size_t var_count) {
    size_t result_size = strlen(template) * 2 + 1;
    char* result = malloc(result_size);
    result[0] = '\0';

    const char* p = template;
    while (*p) {
        const char* start = strstr(p, "{{");
        if (!start) {
            strcat(result, p);
            break;
        }

        strncat(result, p, start - p);

        const char* end = strstr(start, "}}");
        if (!end) break;

        size_t key_len = end - (start + 2);
        char key[256] = {0};
        strncpy(key, start + 2, key_len);
        key[key_len] = '\0';

        const char* replacement = lookup(key, vars, var_count);

        size_t needed_size = strlen(result) + strlen(replacement) + strlen(end + 2) + 1;
        if (needed_size > result_size) {
            result_size = needed_size * 2;
            result = realloc(result, result_size);
        }

        strcat(result, replacement);
        p = end + 2;
    }

    return result;
}

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    if (strcmp(ext, ".woff2") == 0) return "font/woff2";
    return "text/plain";
}

void send_response(int client_fd, const char *mime, const char *filename, int is_template) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
        send(client_fd, msg, strlen(msg), 0);
        return;
    }

    fseek(file, 0, SEEK_END);
    htmlSize = ftell(file);
    rewind(file);

    char *data = malloc(htmlSize + 1);
    fread(data, 1, htmlSize, file);
    data[htmlSize] = '\0';
    fclose(file);

    char* output = data;

    if (is_template) {
        output = render_template(data, templateList.pairs, templateList.length);
        free(data);
        htmlSize = strlen(output);
    }

    dprintf(client_fd, 
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", 
        mime, htmlSize);
    send(client_fd, output, htmlSize, 0);

    if (is_template) free(output);
    else free(data);
}

void answer_to_connection(int client_fd, const char *url, const char *method) {
    if (strcmp(method, "GET") != 0) {
        const char *msg = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        send(client_fd, msg, strlen(msg), 0);
        return;
    }


    if (strcmp(url, "/") == 0) {
        send_response(client_fd, "text/html", "templates/index.html", 1);
    } else if (strncmp(url, "/static/", 8) == 0) {
        char path[256];
        snprintf(path, sizeof(path), ".%s", url);
        send_response(client_fd, get_mime_type(path), path, 0);
    } else if (strstr(url, ".html") != NULL) {
        char path[256];
        snprintf(path, sizeof(path), "templates%s", url);
        send_response(client_fd, "text/html", path, 1);
    } else {
        const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
        send(client_fd, msg, strlen(msg), 0);
    }
}

void parse_field_name(const char* field_name) {
    char* action;
    char* actionPtr = strstr(field_name, "_add");
    if (!actionPtr) {
        actionPtr = strstr(field_name, "_remove");
        action = "remove";
    } else {
        action = "add";
    }

    if (actionPtr) {
        *actionPtr = '\0';

        char* table = strtok((char*)field_name, ".");
        char* column = strtok(NULL, ".");

        if (strcmp(action, "add") == 0) {
            printf("Action: Add, Table: %s, Column: %s\n", table, column);
        } else {
            printf("Action: Remove, Table: %s, Column: %s\n", table, column);
        }
    }
}

void process_form_data(char* body) {
    char* field_start = body;
    while (field_start) {
        char* field_end = strchr(field_start, '&');
        if (field_end) {
            *field_end = '\0';
        }
        char* field_name = strtok(field_start, "=");

        parse_field_name(field_name);

        field_start = field_end ? field_end + 1 : NULL;
    }
}

void handle_post_request(int client_fd, const char* body, const char* url) {
    if (body) {
        process_form_data((char*)body);
    }

    char response[1024];
    snprintf(response, sizeof(response),
             "HTTP/1.1 302 Found\r\n"
             "Location: %s\r\n"
             "Content-Length: 0\r\n"
             "\r\n", url);
    
    send(client_fd, response, strlen(response), 0);
}

void* handle_client(void* arg) {
    int client_fd = *((int*)arg);
    free(arg);

    lua_State* L = init_lua();

    char buffer[2048] = {0};
    ssize_t bytes_received = read(client_fd, buffer, sizeof(buffer) - 1);

    if (bytes_received < 0) {
        perror("Failed to read request");
        close(client_fd);
        lua_close(L);
        return NULL;
    }

    char method[8], url[1024];
    sscanf(buffer, "%s %s", method, url);

    if (strcmp(method, "POST") == 0) {
        char* body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            handle_post_request(client_fd, body, url);
        }
    } else {
        answer_to_connection(client_fd, url, method);
    }

    lua_close(L);
    close(client_fd);
    return NULL;
}

void free_template_list() {
    for (int i = 0; i < templateList.length; ++i) {
        free((char*)templateList.pairs[i].key);
        free((char*)templateList.pairs[i].value);
    }
    free(templateList.pairs);
    templateList.length = 0;
    templateList.pairs = NULL;
}

int update_template_data(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    free_template_list();

    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        count++;
        lua_pop(L, 1);
    }

    templateList.length = count;
    templateList.pairs = malloc(sizeof(TemplateVar) * count);

    lua_pushnil(L);
    int index = 0;
    while (lua_next(L, 1) != 0) {
        const char* key = lua_tostring(L, -2);
        const char* value = lua_tostring(L, -1);

        templateList.pairs[index].key = strdup(key);
        templateList.pairs[index].value = strdup(value);

        lua_pop(L, 1);
        index++;
    }

    return 0;
}

lua_State* init_lua(){
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    lua_register(L, "send_kv_list", update_template_data);

    if (luaL_dofile(L, Lua)) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
    }
    return L;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Server running on http://localhost:%d\n", PORT);

    while (1) {
        int* client_fd = malloc(sizeof(int));
        *client_fd = accept(server_fd, NULL, NULL);
        if (*client_fd < 0) {
            free(client_fd);
            continue;
        }

        // Create a new thread for each client
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client_fd) != 0) {
            perror("pthread_create");
            close(*client_fd);
            free(client_fd);
        } else {
            pthread_detach(thread); // Detach thread to handle cleanup automatically
        }
    }

    close(server_fd);
    return 0;
}
