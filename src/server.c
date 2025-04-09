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
#include <sqlite3.h>
#include <ctype.h>

#define PORT 8888
#define HTML "templates/index.html"
#define LUA "templates/template.lua"

typedef struct {
    const char* key;
    const char* value;
} TemplateVar;

typedef struct {
    int length;
    TemplateVar* pairs;
} TemplateList;

typedef struct {
    char* col;
    char* data;
} ColData;

typedef struct {
    char* tbl;    
    int length;
    ColData* colData;
    char* action;
} DataList;

size_t htmlSize;
const char* DATABASE;

TemplateList templateList;

int update_template_data(lua_State* L);

lua_State* init_lua();

int receive_database_name(lua_State* L){
   DATABASE = luaL_checkstring(L, 1);
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

int send_to_database(sqlite3* db, DataList* data){
    sqlite3_stmt *stmt;
    char placeholders[265] = {0};
    char columns[256] = {0};

    for (int i = 0; i < data->length; i++) {
        strcat(columns, data->colData[i].col);
        strcat(placeholders, "?");

        if (i < data->length - 1) {
            strcat(columns, ", ");
            strcat(placeholders, ", ");
        }
    }

    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s);", data->tbl, columns, placeholders);
    printf("%s\n", sql);

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    for(int i = 0; i < data->length; i++){
        sqlite3_bind_text(stmt, i + 1, data->colData[i].data, -1, SQLITE_STATIC);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 1;
    }

    sqlite3_finalize(stmt);

    return 0;
}

int count_col_fields(const char* body) {
    int count = 0;
    const char* ptr = body;
    while ((ptr = strstr(ptr, "col.")) != NULL) {
        // Ensure it's a full field key like col.something=
        const char* equals = strchr(ptr, '=');
        const char* amp = strchr(ptr, '&');
        if (equals && (amp == NULL || equals < amp)) {
            count++;
        }
        ptr = ptr + 4; // move past "col."
    }
    return count;
}

char from_hex(char ch) {
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

void url_decode(char *str) {
    char *p = str;
    while (*str) {
        if (*str == '%') {
            if (str[1] && str[2]) {
                *p++ = from_hex(str[1]) << 4 | from_hex(str[2]);
                str += 2;
            }
        } else if (*str == '+') {
            *p++ = ' ';
        } else {
            *p++ = *str;
        }
        str++;
    }
    *p = '\0';
}

void process_form_data(const char* body, sqlite3 *db) {
    char* input = strdup(body);  // Safe for multithreading
    if (!input) return;

    int col_count = count_col_fields(body);

    printf("%d\n", col_count);

    DataList* data = malloc(sizeof(DataList));
    data->colData = malloc(sizeof(ColData) * col_count);
    data->length = 0;
    data->tbl = NULL;
    data->action = NULL;

    char* saveptr1;
    char* pair = strtok_r(input, "&", &saveptr1);
    while (pair != NULL) {
        char* saveptr2;
        char* key = strtok_r(pair, "=", &saveptr2);
        char* value = strtok_r(NULL, "=", &saveptr2);
        if (key && value) {
            url_decode(key);
            url_decode(value);

            if (strncmp(key, "col.", 4) == 0) {
                data->colData[data->length].col = strdup(key + 4);
                data->colData[data->length].data = strdup(value);
                printf("col: %s = %s\n", data->colData[data->length].col, data->colData[data->length].data);
                data->length++;
            } else if (strncmp(key, "tbl.", 4) == 0) {
                data->tbl = strdup(key + 4);
                data->action = strdup(value);
            }
        }
        pair = strtok_r(NULL, "&", &saveptr1);
    }

    printf("table: %s\naction: %s\n", data->tbl, data->action);

    send_to_database(db, data);

    // Cleanup
    for (int i = 0; i < data->length; i++) {
        free(data->colData[i].col);
        free(data->colData[i].data);
    }
    free(data->colData);
    free(data->tbl);
    free(data->action);
    free(data);
    free(input);
}


void handle_post_request(int client_fd, const char* body, const char* url, sqlite3* db) {
    if (body) {
        process_form_data((char*)body, db);
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

    sqlite3 * db;
    if (sqlite3_open(DATABASE, &db)){
        fprintf(stderr, "Can't open DB: %s\n", sqlite3_errmsg(db));
    }

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
        printf("%s\n", buffer);
        if (body) {
            body += 4;
            handle_post_request(client_fd, body, url, db);
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

    if (luaL_dofile(L, LUA)) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
    }
    return L;
}

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    lua_register(L, "send_database_name", receive_database_name);

    if (luaL_dofile(L, LUA)) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
    }

    lua_close(L);

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
