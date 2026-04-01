#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <dirent.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SUSE_MONAD_DEFAULT_CONFIG "/etc/suse-monad/suse-monad.json"
#define SUSE_MONAD_DEFAULT_LOG    "/var/log/suse-monad.log"
#define SUSE_MONAD_DEFAULT_STATE  "/var/lib/suse-monad/state.json"
#define SUSE_MONAD_MAX_CMD        8192
#define SUSE_MONAD_MAX_PATH       4096
#define SUSE_MONAD_MAX_WARNINGS   512
#define SUSE_MONAD_MAX_FAILURES   512
#define SUSE_MONAD_MAX_TRACKED    1024
#define SUSE_MONAD_CAPTURE_LIMIT  (1024 * 1024)

typedef enum {
    J_NULL,
    J_BOOL,
    J_NUMBER,
    J_STRING,
    J_ARRAY,
    J_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char **keys;
    JsonValue **values;
    size_t count;
} JsonObject;

typedef struct {
    JsonValue **items;
    size_t count;
} JsonArray;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        JsonArray array;
        JsonObject object;
    } u;
};

typedef struct {
    const char *text;
    size_t pos;
    size_t len;
    char error[512];
} JsonParser;

typedef struct {
    char *items[SUSE_MONAD_MAX_TRACKED];
    size_t count;
} StringList;

typedef struct {
    char key[128];
    char value[2048];
} Token;

typedef struct {
    Token items[256];
    size_t count;
} TokenMap;

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_SUCCESS
} LogLevel;

typedef struct {
    char config_path[SUSE_MONAD_MAX_PATH];
    bool non_interactive;
    bool dry_run;
    bool assume_yes;
    bool verbose;
    char profile[128];
    char target_user[128];
    char enable_features[1024];
    char disable_features[1024];
} Options;

typedef struct {
    int exit_code;
    bool timed_out;
    char *stdout_data;
    char *stderr_data;
} CommandResult;

typedef struct {
    Options opts;
    JsonValue *root;
    FILE *log_fp;
    bool color;
    bool strict_mode;
    bool continue_on_nonfatal;
    int timeout_seconds;
    int retry_count;
    int current_step;
    int total_steps;

    char distro_id[64];
    char distro_version[128];
    char arch[64];
    bool is_tumbleweed;
    bool is_root;
    bool is_laptop;
    bool network_online;
    bool x11_present;
    bool display_manager_present;
    bool existing_desktop_present;
    unsigned long long root_free_mb;

    char detected_display_manager[128];
    char target_user[128];
    char target_group[128];
    char target_home[SUSE_MONAD_MAX_PATH];

    char selected_profile[128];
    char chosen_terminal[256];
    char chosen_browser[256];
    char chosen_editor[256];
    char chosen_file_manager[256];
    char chosen_launcher[256];
    char chosen_mod_hs[64];
    char chosen_mod_name[64];
    char wallpaper_path[SUSE_MONAD_MAX_PATH];
    char host_profile[64];

    StringList warnings;
    StringList failures;
    StringList packages_installed;
    StringList files_written;
    StringList services_enabled;
    StringList backups_written;
} Runtime;

static void free_json(JsonValue *v);

static const char *nz(const char *s, const char *fallback) {
    return (s && *s) ? s : fallback;
}

static void runtime_fail(Runtime *rt, const char *fmt, ...);
static void runtime_warn(Runtime *rt, const char *fmt, ...);
static void log_msg(Runtime *rt, LogLevel level, const char *fmt, ...);
static int run_shell(Runtime *rt, const char *cmd, int timeout_seconds, bool capture_output, CommandResult *out);
static void free_command_result(CommandResult *r);
static bool evaluate_condition(Runtime *rt, const char *cond);
static bool ensure_parent_dir(Runtime *rt, const char *path, mode_t mode, uid_t uid, gid_t gid);
static bool write_file_from_spec(Runtime *rt, JsonValue *file_spec, TokenMap *base_tokens);
static void rollback_files_if_requested(Runtime *rt);

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1);
    }
    return p;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static char *str_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

static void slist_add(StringList *list, const char *s) {
    if (!s || !*s) return;
    if (list->count >= SUSE_MONAD_MAX_TRACKED) return;
    list->items[list->count++] = xstrdup(s);
}

static bool slist_contains(const StringList *list, const char *s) {
    if (!s) return false;
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], s) == 0) return true;
    }
    return false;
}

static void slist_free(StringList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    list->count = 0;
}

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static void format_path_for_error(char *dst, size_t dstsz, const char *path) {
    if (!dst || dstsz == 0) return;
    if (!path) {
        snprintf(dst, dstsz, "?");
        return;
    }
    size_t len = strlen(path);
    if (len < dstsz) {
        snprintf(dst, dstsz, "%s", path);
        return;
    }
    if (dstsz <= 4) {
        if (dstsz > 0) dst[0] = 0;
        return;
    }
    size_t keep = dstsz - 4;
    size_t tail = keep / 2;
    size_t head = keep - tail;
    memcpy(dst, path, head);
    memcpy(dst + head, "...", 3);
    memcpy(dst + head + 3, path + len - tail, tail);
    dst[dstsz - 1] = 0;
}

static void set_errno_error(char *err, size_t errsz, const char *op, const char *path1, const char *path2) {
    char p1[160];
    char p2[160];
    format_path_for_error(p1, sizeof(p1), path1);
    format_path_for_error(p2, sizeof(p2), path2);
    if (!err || errsz == 0) return;
    if (path1 && path2) {
        snprintf(err, errsz, "%s(%s -> %s): %s", op, p1, p2, strerror(errno));
    } else if (path1) {
        snprintf(err, errsz, "%s(%s): %s", op, p1, strerror(errno));
    } else {
        snprintf(err, errsz, "%s: %s", op, strerror(errno));
    }
}

static bool write_file_atomic(const char *path, const char *content, mode_t mode, uid_t uid, gid_t gid, char *err, size_t errsz) {
    char tmp[SUSE_MONAD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        set_errno_error(err, errsz, "open", tmp, NULL);
        return false;
    }
    size_t len = strlen(content);
    ssize_t wr = write(fd, content, len);
    if (wr < 0 || (size_t)wr != len) {
        set_errno_error(err, errsz, "write", tmp, NULL);
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fchmod(fd, mode) != 0) {
        set_errno_error(err, errsz, "fchmod", tmp, NULL);
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fchown(fd, uid, gid) != 0 && errno != EPERM) {
        set_errno_error(err, errsz, "fchown", tmp, NULL);
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fsync(fd) != 0) {
        set_errno_error(err, errsz, "fsync", tmp, NULL);
        close(fd);
        unlink(tmp);
        return false;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        set_errno_error(err, errsz, "rename", tmp, path);
        unlink(tmp);
        return false;
    }
    return true;
}

static void token_set(TokenMap *map, const char *key, const char *value) {
    if (!map || !key || !*key) return;
    for (size_t i = 0; i < map->count; ++i) {
        if (strcmp(map->items[i].key, key) == 0) {
            snprintf(map->items[i].value, sizeof(map->items[i].value), "%s", value ? value : "");
            return;
        }
    }
    if (map->count >= sizeof(map->items)/sizeof(map->items[0])) return;
    snprintf(map->items[map->count].key, sizeof(map->items[map->count].key), "%s", key);
    snprintf(map->items[map->count].value, sizeof(map->items[map->count].value), "%s", value ? value : "");
    map->count++;
}

static const char *token_get(TokenMap *map, const char *key) {
    if (!map || !key) return NULL;
    for (size_t i = 0; i < map->count; ++i) {
        if (strcmp(map->items[i].key, key) == 0) return map->items[i].value;
    }
    return NULL;
}

static char *substitute_tokens(const char *input, TokenMap *map) {
    if (!input) return xstrdup("");
    size_t cap = strlen(input) + 256;
    char *out = xmalloc(cap);
    size_t olen = 0;
    for (size_t i = 0; input[i]; ) {
        if (input[i] == '{' && input[i+1] == '{') {
            const char *end = strstr(input + i + 2, "}}");
            if (end) {
                size_t klen = (size_t)(end - (input + i + 2));
                char key[256];
                if (klen >= sizeof(key)) klen = sizeof(key)-1;
                memcpy(key, input + i + 2, klen);
                key[klen] = '\0';
                while (klen > 0 && isspace((unsigned char)key[klen-1])) key[--klen] = '\0';
                char *p = key;
                while (*p && isspace((unsigned char)*p)) p++;
                const char *value = token_get(map, p);
                if (!value) value = "";
                size_t vlen = strlen(value);
                if (olen + vlen + 1 > cap) {
                    cap = (olen + vlen + 256) * 2;
                    out = realloc(out, cap);
                    if (!out) exit(1);
                }
                memcpy(out + olen, value, vlen);
                olen += vlen;
                i = (size_t)(end - input) + 2;
                continue;
            }
        }
        if (olen + 2 > cap) {
            cap *= 2;
            out = realloc(out, cap);
            if (!out) exit(1);
        }
        out[olen++] = input[i++];
    }
    out[olen] = '\0';
    return out;
}

static const char *json_type_name(JsonType t) {
    switch (t) {
        case J_NULL: return "null";
        case J_BOOL: return "bool";
        case J_NUMBER: return "number";
        case J_STRING: return "string";
        case J_ARRAY: return "array";
        case J_OBJECT: return "object";
    }
    return "unknown";
}

static void json_skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (isspace((unsigned char)c)) { p->pos++; continue; }
        if (c == '/' && p->pos + 1 < p->len && p->text[p->pos+1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->text[p->pos] != '\n') p->pos++;
            continue;
        }
        break;
    }
}

static bool json_match(JsonParser *p, const char *lit) {
    size_t n = strlen(lit);
    if (p->pos + n > p->len) return false;
    if (memcmp(p->text + p->pos, lit, n) == 0) {
        p->pos += n;
        return true;
    }
    return false;
}

static JsonValue *json_new(JsonType t) {
    JsonValue *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static JsonValue *json_parse_value(JsonParser *p);

static JsonValue *json_parse_string(JsonParser *p) {
    if (p->text[p->pos] != '"') {
        snprintf(p->error, sizeof(p->error), "expected string at byte %zu", p->pos);
        return NULL;
    }
    p->pos++;
    size_t cap = 64, len = 0;
    char *buf = xmalloc(cap);
    while (p->pos < p->len) {
        char c = p->text[p->pos++];
        if (c == '"') {
            buf[len] = '\0';
            JsonValue *v = json_new(J_STRING);
            v->u.string = buf;
            return v;
        }
        if (c == '\\') {
            if (p->pos >= p->len) break;
            char e = p->text[p->pos++];
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) {
                        snprintf(p->error, sizeof(p->error), "bad unicode escape at byte %zu", p->pos);
                        free(buf);
                        return NULL;
                    }
                    char hex[5];
                    memcpy(hex, p->text + p->pos, 4);
                    hex[4] = '\0';
                    p->pos += 4;
                    long code = strtol(hex, NULL, 16);
                    if (code < 0x80) c = (char)code;
                    else c = '?';
                    break;
                }
                default:
                    snprintf(p->error, sizeof(p->error), "bad escape at byte %zu", p->pos);
                    free(buf);
                    return NULL;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) exit(1);
        }
        buf[len++] = c;
    }
    snprintf(p->error, sizeof(p->error), "unterminated string at byte %zu", p->pos);
    free(buf);
    return NULL;
}

static JsonValue *json_parse_number(JsonParser *p) {
    size_t start = p->pos;
    if (p->text[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) p->pos++;
    if (p->pos < p->len && p->text[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->text[p->pos] == '+' || p->text[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) p->pos++;
    }
    char *tmp = xmalloc(p->pos - start + 1);
    memcpy(tmp, p->text + start, p->pos - start);
    tmp[p->pos - start] = '\0';
    JsonValue *v = json_new(J_NUMBER);
    v->u.number = strtod(tmp, NULL);
    free(tmp);
    return v;
}

static JsonValue *json_parse_array(JsonParser *p) {
    if (p->text[p->pos] != '[') return NULL;
    p->pos++;
    JsonValue *v = json_new(J_ARRAY);
    json_skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ']') { p->pos++; return v; }
    while (p->pos < p->len) {
        json_skip_ws(p);
        JsonValue *item = json_parse_value(p);
        if (!item) { free_json(v); return NULL; }
        v->u.array.items = realloc(v->u.array.items, sizeof(JsonValue*) * (v->u.array.count + 1));
        if (!v->u.array.items) exit(1);
        v->u.array.items[v->u.array.count++] = item;
        json_skip_ws(p);
        if (p->pos < p->len && p->text[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->text[p->pos] == ']') { p->pos++; return v; }
        break;
    }
    snprintf(p->error, sizeof(p->error), "unterminated array at byte %zu", p->pos);
    free_json(v);
    return NULL;
}

static JsonValue *json_parse_object(JsonParser *p) {
    if (p->text[p->pos] != '{') return NULL;
    p->pos++;
    JsonValue *v = json_new(J_OBJECT);
    json_skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == '}') { p->pos++; return v; }
    while (p->pos < p->len) {
        json_skip_ws(p);
        JsonValue *key = json_parse_string(p);
        if (!key) { free_json(v); return NULL; }
        json_skip_ws(p);
        if (p->pos >= p->len || p->text[p->pos] != ':') {
            snprintf(p->error, sizeof(p->error), "expected ':' at byte %zu", p->pos);
            free_json(key);
            free_json(v);
            return NULL;
        }
        p->pos++;
        json_skip_ws(p);
        JsonValue *value = json_parse_value(p);
        if (!value) {
            free_json(key);
            free_json(v);
            return NULL;
        }
        v->u.object.keys = realloc(v->u.object.keys, sizeof(char*) * (v->u.object.count + 1));
        v->u.object.values = realloc(v->u.object.values, sizeof(JsonValue*) * (v->u.object.count + 1));
        if (!v->u.object.keys || !v->u.object.values) exit(1);
        v->u.object.keys[v->u.object.count] = key->u.string;
        v->u.object.values[v->u.object.count] = value;
        v->u.object.count++;
        free(key);
        json_skip_ws(p);
        if (p->pos < p->len && p->text[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->text[p->pos] == '}') { p->pos++; return v; }
        break;
    }
    snprintf(p->error, sizeof(p->error), "unterminated object at byte %zu", p->pos);
    free_json(v);
    return NULL;
}

static JsonValue *json_parse_value(JsonParser *p) {
    json_skip_ws(p);
    if (p->pos >= p->len) {
        snprintf(p->error, sizeof(p->error), "unexpected EOF");
        return NULL;
    }
    char c = p->text[p->pos];
    if (c == '"') return json_parse_string(p);
    if (c == '{') return json_parse_object(p);
    if (c == '[') return json_parse_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return json_parse_number(p);
    if (json_match(p, "true")) { JsonValue *v = json_new(J_BOOL); v->u.boolean = true; return v; }
    if (json_match(p, "false")) { JsonValue *v = json_new(J_BOOL); v->u.boolean = false; return v; }
    if (json_match(p, "null")) { return json_new(J_NULL); }
    snprintf(p->error, sizeof(p->error), "unexpected token at byte %zu", p->pos);
    return NULL;
}

static JsonValue *json_parse_text(const char *text, char *error, size_t errsz) {
    JsonParser p = { .text = text, .len = strlen(text), .pos = 0 };
    JsonValue *root = json_parse_value(&p);
    if (!root) {
        snprintf(error, errsz, "%s", p.error[0] ? p.error : "parse error");
        return NULL;
    }
    json_skip_ws(&p);
    if (p.pos != p.len) {
        snprintf(error, errsz, "trailing content at byte %zu", p.pos);
        free_json(root);
        return NULL;
    }
    return root;
}

static void free_json(JsonValue *v) {
    if (!v) return;
    switch (v->type) {
        case J_STRING:
            free(v->u.string);
            break;
        case J_ARRAY:
            for (size_t i = 0; i < v->u.array.count; ++i) free_json(v->u.array.items[i]);
            free(v->u.array.items);
            break;
        case J_OBJECT:
            for (size_t i = 0; i < v->u.object.count; ++i) {
                free(v->u.object.keys[i]);
                free_json(v->u.object.values[i]);
            }
            free(v->u.object.keys);
            free(v->u.object.values);
            break;
        default:
            break;
    }
    free(v);
}

static JsonValue *json_obj_get(JsonValue *obj, const char *key) {
    if (!obj || obj->type != J_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.object.count; ++i) {
        if (strcmp(obj->u.object.keys[i], key) == 0) return obj->u.object.values[i];
    }
    return NULL;
}

static JsonValue *json_obj_get_path(JsonValue *root, const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    char *save = NULL;
    char *part = strtok_r(tmp, ".", &save);
    JsonValue *cur = root;
    while (part && cur) {
        cur = json_obj_get(cur, part);
        part = strtok_r(NULL, ".", &save);
    }
    return cur;
}

static const char *json_string(JsonValue *v) {
    return (v && v->type == J_STRING) ? v->u.string : NULL;
}

static bool json_bool_value(JsonValue *v, bool def) {
    return (v && v->type == J_BOOL) ? v->u.boolean : def;
}

static int json_int_value(JsonValue *v, int def) {
    return (v && v->type == J_NUMBER) ? (int)v->u.number : def;
}

static void usage(FILE *fp) {
    fprintf(fp,
        "suse-monad - openSUSE Tumbleweed xMonad desktop provisioner\n\n"
        "Usage:\n"
        "  suse-monad [options]\n\n"
        "Options:\n"
        "  --config PATH           JSON policy file (default: %s)\n"
        "  --non-interactive       Never prompt\n"
        "  --dry-run               Print actions without changing system\n"
        "  --yes                   Assume yes for confirmations\n"
        "  --profile NAME          Choose profile from JSON\n"
        "  --target-user USER      Desktop user to configure\n"
        "  --enable-features A,B   Force-enable feature flags\n"
        "  --disable-features A,B  Force-disable feature flags\n"
        "  --verbose               More console logging\n"
        "  --help                  Show this help\n",
        SUSE_MONAD_DEFAULT_CONFIG
    );
}

static bool parse_args(int argc, char **argv, Options *opts) {
    memset(opts, 0, sizeof(*opts));
    snprintf(opts->config_path, sizeof(opts->config_path), "%s", SUSE_MONAD_DEFAULT_CONFIG);
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--config") == 0 && i + 1 < argc) {
            snprintf(opts->config_path, sizeof(opts->config_path), "%s", argv[++i]);
        } else if (strcmp(a, "--non-interactive") == 0) {
            opts->non_interactive = true;
        } else if (strcmp(a, "--dry-run") == 0) {
            opts->dry_run = true;
        } else if (strcmp(a, "--yes") == 0) {
            opts->assume_yes = true;
        } else if (strcmp(a, "--verbose") == 0) {
            opts->verbose = true;
        } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
            snprintf(opts->profile, sizeof(opts->profile), "%s", argv[++i]);
        } else if (strcmp(a, "--target-user") == 0 && i + 1 < argc) {
            snprintf(opts->target_user, sizeof(opts->target_user), "%s", argv[++i]);
        } else if (strcmp(a, "--enable-features") == 0 && i + 1 < argc) {
            snprintf(opts->enable_features, sizeof(opts->enable_features), "%s", argv[++i]);
        } else if (strcmp(a, "--disable-features") == 0 && i + 1 < argc) {
            snprintf(opts->disable_features, sizeof(opts->disable_features), "%s", argv[++i]);
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return false;
        }
    }
    return true;
}

static void open_log(Runtime *rt) {
    const char *log_path = json_string(json_obj_get_path(rt->root, "logging.file"));
    if (!log_path || !*log_path) log_path = SUSE_MONAD_DEFAULT_LOG;
    rt->log_fp = fopen(log_path, "a");
}

static const char *color_for(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "\033[36m";
        case LOG_INFO: return "\033[34m";
        case LOG_WARN: return "\033[33m";
        case LOG_ERROR: return "\033[31m";
        case LOG_SUCCESS: return "\033[32m";
    }
    return "";
}

static const char *tag_for(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO: return "INFO";
        case LOG_WARN: return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_SUCCESS: return "OK";
    }
    return "LOG";
}

static void log_msg(Runtime *rt, LogLevel level, const char *fmt, ...) {
    char msg[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    if (rt->log_fp) {
        fprintf(rt->log_fp, "%s [%s] %s\n", ts, tag_for(level), msg);
        fflush(rt->log_fp);
    }
    if (level == LOG_DEBUG && !rt->opts.verbose) return;
    if (rt->color) fprintf(stderr, "%s", color_for(level));
    fprintf(stderr, "[%02d/%02d] %-5s %s", rt->current_step, rt->total_steps, tag_for(level), msg);
    if (rt->color) fprintf(stderr, "\033[0m");
    fputc('\n', stderr);
}

static void runtime_warn(Runtime *rt, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slist_add(&rt->warnings, msg);
    log_msg(rt, LOG_WARN, "%s", msg);
}

static void runtime_fail(Runtime *rt, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slist_add(&rt->failures, msg);
    log_msg(rt, LOG_ERROR, "%s", msg);
}

static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool dir_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool command_exists(const char *cmd) {
    if (!cmd || !*cmd) return false;
    if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/sbin:/usr/bin:/sbin:/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *part = strtok_r(copy, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
        char buf[SUSE_MONAD_MAX_PATH];
        snprintf(buf, sizeof(buf), "%s/%s", part, cmd);
        if (access(buf, X_OK) == 0) { free(copy); return true; }
    }
    free(copy);
    return false;
}

static void free_command_result(CommandResult *r) {
    if (!r) return;
    free(r->stdout_data);
    free(r->stderr_data);
    memset(r, 0, sizeof(*r));
}

static char *append_buf(char *buf, size_t *len, size_t *cap, const char *data, size_t n) {
    if (!buf) {
        *cap = n + 64;
        if (*cap > SUSE_MONAD_CAPTURE_LIMIT) *cap = SUSE_MONAD_CAPTURE_LIMIT;
        buf = xmalloc(*cap);
        *len = 0;
    }
    if (*len + n + 1 > *cap) {
        size_t newcap = (*len + n + 64) * 2;
        if (newcap > SUSE_MONAD_CAPTURE_LIMIT) newcap = SUSE_MONAD_CAPTURE_LIMIT;
        if (newcap <= *len + 1) return buf;
        char *nb = realloc(buf, newcap);
        if (!nb) return buf;
        buf = nb;
        *cap = newcap;
    }
    size_t copy = n;
    if (*len + copy + 1 > *cap) copy = *cap - *len - 1;
    memcpy(buf + *len, data, copy);
    *len += copy;
    buf[*len] = '\0';
    return buf;
}

static int run_shell(Runtime *rt, const char *cmd, int timeout_seconds, bool capture_output, CommandResult *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!cmd || !*cmd) return -1;
    log_msg(rt, LOG_DEBUG, "exec: %s", cmd);
    if (rt->opts.dry_run) {
        log_msg(rt, LOG_INFO, "dry-run: %s", cmd);
        if (out) out->exit_code = 0;
        return 0;
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        runtime_fail(rt, "pipe failed: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        runtime_fail(rt, "fork failed: %s", strerror(errno));
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        setsid();
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    char *obuf = NULL, *ebuf = NULL;
    size_t olen = 0, ocap = 0, elen = 0, ecap = 0;
    bool out_open = true, err_open = true;
    time_t start = time(NULL);
    int status = 0;
    bool exited = false;
    while (out_open || err_open || !exited) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        if (out_open) { pfds[nfds].fd = out_pipe[0]; pfds[nfds].events = POLLIN; nfds++; }
        if (err_open) { pfds[nfds].fd = err_pipe[0]; pfds[nfds].events = POLLIN; nfds++; }
        int rc = poll(pfds, nfds, 200);
        if (rc > 0) {
            char buf[4096];
            if (out_open) {
                ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (capture_output && out) obuf = append_buf(obuf, &olen, &ocap, buf, (size_t)n);
                } else if (n == 0) { close(out_pipe[0]); out_open = false; }
            }
            if (err_open) {
                ssize_t n = read(err_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (capture_output && out) ebuf = append_buf(ebuf, &elen, &ecap, buf, (size_t)n);
                } else if (n == 0) { close(err_pipe[0]); err_open = false; }
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) exited = true;
        if (timeout_seconds > 0 && time(NULL) - start > timeout_seconds && !exited) {
            kill(-pid, SIGTERM);
            struct timespec ts = {0, 200000000};
            nanosleep(&ts, NULL);
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
            if (out) out->timed_out = true;
            runtime_fail(rt, "command timed out after %d seconds: %s", timeout_seconds, cmd);
        }
    }
    if (out_open) close(out_pipe[0]);
    if (err_open) close(err_pipe[0]);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (out) {
        out->exit_code = code;
        out->stdout_data = obuf ? obuf : xstrdup("");
        out->stderr_data = ebuf ? ebuf : xstrdup("");
    } else {
        free(obuf); free(ebuf);
    }
    return code;
}

static int run_shell_with_retry(Runtime *rt, const char *cmd, int timeout_seconds, bool capture_output, CommandResult *out) {
    int tries = rt->retry_count > 0 ? rt->retry_count + 1 : 1;
    CommandResult tmp;
    int rc = -1;
    for (int attempt = 1; attempt <= tries; ++attempt) {
        memset(&tmp, 0, sizeof(tmp));
        rc = run_shell(rt, cmd, timeout_seconds, capture_output, &tmp);
        if (rc == 0) {
            if (out) *out = tmp; else free_command_result(&tmp);
            return rc;
        }
        if (attempt < tries) log_msg(rt, LOG_WARN, "retrying command (%d/%d)", attempt, tries - 1);
        free_command_result(&tmp);
    }
    if (out) memset(out, 0, sizeof(*out));
    return rc;
}

static bool ask_yes_no(Runtime *rt, const char *question, bool def_yes) {
    if (rt->opts.assume_yes) return true;
    if (rt->opts.non_interactive) return def_yes;
    fprintf(stderr, "%s [%s]: ", question, def_yes ? "Y/n" : "y/N");
    fflush(stderr);
    char line[32];
    if (!fgets(line, sizeof(line), stdin)) return def_yes;
    char c = (char)tolower((unsigned char)line[0]);
    if (c == '\n' || c == '\0') return def_yes;
    return c == 'y';
}

static bool parse_os_release(Runtime *rt) {
    size_t len = 0;
    char *text = read_file_all("/etc/os-release", &len);
    if (!text) {
        runtime_fail(rt, "cannot read /etc/os-release");
        return false;
    }
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (*val == '"') {
            size_t n = strlen(val);
            if (n >= 2 && val[n-1] == '"') { val[n-1] = '\0'; val++; }
        }
        if (strcmp(key, "ID") == 0) snprintf(rt->distro_id, sizeof(rt->distro_id), "%s", val);
        else if (strcmp(key, "VERSION") == 0 || strcmp(key, "VERSION_ID") == 0) snprintf(rt->distro_version, sizeof(rt->distro_version), "%s", val);
    }
    free(text);
    if (command_exists("uname")) {
        CommandResult cr;
        run_shell(rt, "uname -m", 5, true, &cr);
        if (cr.stdout_data) snprintf(rt->arch, sizeof(rt->arch), "%s", trim(cr.stdout_data));
        free_command_result(&cr);
    } else {
        snprintf(rt->arch, sizeof(rt->arch), "x86_64");
    }
    rt->is_tumbleweed = strcasecmp(rt->distro_id, "opensuse-tumbleweed") == 0 || strstr(rt->distro_version, "Tumbleweed") != NULL;
    return true;
}

static bool detect_laptop_mode(void) {
    if (dir_exists("/sys/class/power_supply")) {
        DIR *dir = opendir("/sys/class/power_supply");
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir))) {
                if (de->d_name[0] == '.') continue;
                char path[SUSE_MONAD_MAX_PATH];
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
                size_t len = 0;
                char *text = read_file_all(path, &len);
                if (text) {
                    char *t = trim(text);
                    bool is_battery = strcasecmp(t, "Battery") == 0;
                    free(text);
                    if (is_battery) { closedir(dir); return true; }
                }
            }
            closedir(dir);
        }
    }
    return file_exists("/proc/acpi/button/lid/LID0/state");
}

static bool detect_network(void) {
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), fp)) {
        char iface[64]; unsigned long dest = 1;
        if (sscanf(line, "%63s %lx", iface, &dest) == 2) {
            if (dest == 0) { ok = true; break; }
        }
    }
    fclose(fp);
    return ok;
}

static bool detect_disk_space(Runtime *rt) {
    struct statvfs vfs;
    if (statvfs("/", &vfs) != 0) {
        runtime_warn(rt, "statvfs(/) failed: %s", strerror(errno));
        return false;
    }
    unsigned long long free_bytes = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
    rt->root_free_mb = free_bytes / (1024ULL * 1024ULL);
    return true;
}

static bool detect_x11_present(void) {
    return file_exists("/usr/bin/startx") || dir_exists("/usr/share/xsessions") || file_exists("/usr/bin/Xorg");
}

static bool detect_display_manager(Runtime *rt) {
    if (file_exists("/etc/sysconfig/displaymanager")) {
        size_t len = 0;
        char *text = read_file_all("/etc/sysconfig/displaymanager", &len);
        if (text) {
            char *save = NULL;
            for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                if (strncmp(line, "DISPLAYMANAGER=", 15) == 0) {
                    char *v = line + 15;
                    v = trim(v);
                    if (*v == '"') {
                        size_t n = strlen(v);
                        if (n >= 2 && v[n-1] == '"') { v[n-1] = '\0'; v++; }
                    }
                    snprintf(rt->detected_display_manager, sizeof(rt->detected_display_manager), "%s", v);
                    rt->display_manager_present = *v != '\0';
                    break;
                }
            }
            free(text);
        }
    }
    return rt->display_manager_present;
}

static bool detect_existing_desktop(void) {
    const char *desktop_paths[] = {
        "/usr/bin/gnome-shell", "/usr/bin/startplasma-x11", "/usr/bin/xfce4-session",
        "/usr/bin/lxqt-session", "/usr/bin/i3", "/usr/bin/openbox", "/usr/bin/awesome",
        "/usr/bin/bspwm", "/usr/bin/icewm-session", NULL
    };
    for (int i = 0; desktop_paths[i]; ++i) {
        if (file_exists(desktop_paths[i])) return true;
    }
    return false;
}

static bool lookup_user(const char *name, char *home, size_t homesz, char *group_name, size_t groupsz) {
    struct passwd *pw = getpwnam(name);
    if (!pw) return false;
    snprintf(home, homesz, "%s", pw->pw_dir);
    struct group *gr = getgrgid(pw->pw_gid);
    snprintf(group_name, groupsz, "%s", gr ? gr->gr_name : name);
    return true;
}

static bool detect_target_user(Runtime *rt) {
    JsonValue *users = json_obj_get(rt->root, "users");
    int min_uid = json_int_value(json_obj_get(users, "min_uid"), 1000);
    if (rt->opts.target_user[0]) {
        snprintf(rt->target_user, sizeof(rt->target_user), "%s", rt->opts.target_user);
        return lookup_user(rt->target_user, rt->target_home, sizeof(rt->target_home), rt->target_group, sizeof(rt->target_group));
    }
    const char *sudo_user = getenv("SUDO_USER");
    bool prefer_sudo = json_bool_value(json_obj_get(users, "prefer_sudo_user"), true);
    if (prefer_sudo && sudo_user && *sudo_user && strcmp(sudo_user, "root") != 0) {
        snprintf(rt->target_user, sizeof(rt->target_user), "%s", sudo_user);
        if (lookup_user(rt->target_user, rt->target_home, sizeof(rt->target_home), rt->target_group, sizeof(rt->target_group))) return true;
    }
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        if ((int)pw->pw_uid < min_uid) continue;
        if (!pw->pw_dir || !*pw->pw_dir) continue;
        snprintf(rt->target_user, sizeof(rt->target_user), "%s", pw->pw_name);
        snprintf(rt->target_home, sizeof(rt->target_home), "%s", pw->pw_dir);
        struct group *gr = getgrgid(pw->pw_gid);
        snprintf(rt->target_group, sizeof(rt->target_group), "%s", gr ? gr->gr_name : pw->pw_name);
        endpwent();
        return true;
    }
    endpwent();
    return false;
}

static bool validate_schema_section(Runtime *rt, const char *name, JsonType type) {
    JsonValue *v = json_obj_get(rt->root, name);
    if (!v) {
        runtime_fail(rt, "missing required top-level JSON section '%s'", name);
        return false;
    }
    if (v->type != type) {
        runtime_fail(rt, "top-level section '%s' must be %s, got %s", name, json_type_name(type), json_type_name(v->type));
        return false;
    }
    return true;
}

static bool validate_config(Runtime *rt) {
    bool ok = true;
    ok &= validate_schema_section(rt, "meta", J_OBJECT);
    ok &= validate_schema_section(rt, "paths", J_OBJECT);
    ok &= validate_schema_section(rt, "ui", J_OBJECT);
    ok &= validate_schema_section(rt, "profiles", J_ARRAY);
    ok &= validate_schema_section(rt, "detection", J_OBJECT);
    ok &= validate_schema_section(rt, "preflight", J_OBJECT);
    ok &= validate_schema_section(rt, "repositories", J_ARRAY);
    ok &= validate_schema_section(rt, "package_groups", J_OBJECT);
    ok &= validate_schema_section(rt, "packages", J_OBJECT);
    ok &= validate_schema_section(rt, "commands", J_OBJECT);
    ok &= validate_schema_section(rt, "files", J_ARRAY);
    ok &= validate_schema_section(rt, "templates", J_OBJECT);
    ok &= validate_schema_section(rt, "xmonad", J_OBJECT);
    ok &= validate_schema_section(rt, "session", J_OBJECT);
    ok &= validate_schema_section(rt, "services", J_ARRAY);
    ok &= validate_schema_section(rt, "users", J_OBJECT);
    ok &= validate_schema_section(rt, "permissions", J_OBJECT);
    ok &= validate_schema_section(rt, "backups", J_OBJECT);
    ok &= validate_schema_section(rt, "rollback", J_OBJECT);
    ok &= validate_schema_section(rt, "verification", J_ARRAY);
    ok &= validate_schema_section(rt, "recovery", J_OBJECT);
    ok &= validate_schema_section(rt, "logging", J_OBJECT);
    ok &= validate_schema_section(rt, "defaults", J_OBJECT);
    ok &= validate_schema_section(rt, "conditionals", J_OBJECT);
    ok &= validate_schema_section(rt, "features", J_OBJECT);

    const char *project = json_string(json_obj_get_path(rt->root, "meta.project_name"));
    if (!project || strcmp(project, "suse-monad") != 0) runtime_warn(rt, "meta.project_name is not 'suse-monad'");
    const char *target = json_string(json_obj_get_path(rt->root, "meta.target_distro"));
    if (!target || strcasecmp(target, "openSUSE Tumbleweed") != 0) runtime_warn(rt, "meta.target_distro is not 'openSUSE Tumbleweed'");
    const char *schema = json_string(json_obj_get_path(rt->root, "meta.schema_version"));
    if (!schema || !*schema) { runtime_fail(rt, "meta.schema_version is required"); ok = false; }
    if (!json_string(json_obj_get_path(rt->root, "defaults.profile"))) { runtime_fail(rt, "defaults.profile is required"); ok = false; }
    if (!json_string(json_obj_get_path(rt->root, "commands.install"))) { runtime_fail(rt, "commands.install is required"); ok = false; }
    if (!json_string(json_obj_get_path(rt->root, "commands.search"))) { runtime_fail(rt, "commands.search is required"); ok = false; }
    return ok;
}

static void load_execution_policy(Runtime *rt) {
    rt->continue_on_nonfatal = json_bool_value(json_obj_get_path(rt->root, "defaults.execution.continue_on_nonfatal"), true);
    rt->strict_mode = json_bool_value(json_obj_get_path(rt->root, "defaults.execution.strict_mode"), false);
    rt->timeout_seconds = json_int_value(json_obj_get_path(rt->root, "defaults.execution.command_timeout_seconds"), 900);
    rt->retry_count = json_int_value(json_obj_get_path(rt->root, "defaults.execution.retry_count"), 1);
}

static bool feature_default_enabled(Runtime *rt, const char *name) {
    JsonValue *features = json_obj_get(rt->root, "features");
    JsonValue *f = json_obj_get(features, name);
    if (!f || f->type != J_OBJECT) return false;
    return json_bool_value(json_obj_get(f, "enabled"), false);
}

static bool csv_has_token(const char *csv, const char *name) {
    if (!csv || !*csv || !name || !*name) return false;
    char *copy = xstrdup(csv);
    char *save = NULL;
    bool found = false;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (strcasecmp(tok, name) == 0) { found = true; break; }
    }
    free(copy);
    return found;
}

static bool feature_enabled(Runtime *rt, const char *name) {
    if (!name || !*name) return false;
    if (csv_has_token(rt->opts.disable_features, name)) return false;
    if (csv_has_token(rt->opts.enable_features, name)) return true;
    JsonValue *profiles = json_obj_get(rt->root, "profiles");
    for (size_t i = 0; profiles && i < profiles->u.array.count; ++i) {
        JsonValue *p = profiles->u.array.items[i];
        if (p->type != J_OBJECT) continue;
        const char *pn = json_string(json_obj_get(p, "name"));
        if (pn && strcmp(pn, rt->selected_profile) == 0) {
            JsonValue *flags = json_obj_get(p, "features");
            JsonValue *flag = json_obj_get(flags, name);
            if (flag && flag->type == J_BOOL) return flag->u.boolean;
        }
    }
    return feature_default_enabled(rt, name);
}

static bool evaluate_condition(Runtime *rt, const char *cond) {
    if (!cond || !*cond) return true;
    if (strcasecmp(cond, "always") == 0) return true;
    if (strcasecmp(cond, "only_if_laptop") == 0) return rt->is_laptop;
    if (strcasecmp(cond, "only_if_desktop") == 0) return !rt->is_laptop;
    if (strcasecmp(cond, "only_if_user_selected") == 0) return rt->target_user[0] != '\0';
    if (strncmp(cond, "only_if_feature_enabled:", 24) == 0) return feature_enabled(rt, cond + 24);
    if (strncmp(cond, "only_if_file_missing:", 21) == 0) return !file_exists(cond + 21);
    if (strncmp(cond, "only_if_command_exists:", 23) == 0) return command_exists(cond + 23);
    if (strncmp(cond, "only_if_package_exists:", 23) == 0) {
        char *cmd = str_printf("rpm -q %s >/dev/null 2>&1", cond + 23);
        int rc = run_shell(rt, cmd, 20, false, NULL);
        free(cmd);
        return rc == 0;
    }
    if (strncmp(cond, "only_if_repo_enabled:", 21) == 0) {
        JsonValue *repos = json_obj_get(rt->root, "repositories");
        for (size_t i = 0; repos && i < repos->u.array.count; ++i) {
            JsonValue *r = repos->u.array.items[i];
            const char *alias = json_string(json_obj_get(r, "alias"));
            if (alias && strcmp(alias, cond + 21) == 0) return json_bool_value(json_obj_get(r, "enabled"), true);
        }
        return false;
    }
    if (strncmp(cond, "only_if_service_present:", 24) == 0) {
        char *cmd = str_printf("systemctl list-unit-files | awk '{print $1}' | grep -Fxq '%s.service'", cond + 24);
        int rc = run_shell(rt, cmd, 10, false, NULL);
        free(cmd);
        return rc == 0;
    }
    if (strncmp(cond, "not:", 4) == 0) return !evaluate_condition(rt, cond + 4);
    runtime_warn(rt, "unknown condition '%s' treated as false", cond);
    return false;
}

static bool evaluate_conditions_array(Runtime *rt, JsonValue *conds) {
    if (!conds) return true;
    if (conds->type == J_STRING) return evaluate_condition(rt, conds->u.string);
    if (conds->type != J_ARRAY) return true;
    for (size_t i = 0; i < conds->u.array.count; ++i) {
        JsonValue *v = conds->u.array.items[i];
        if (v->type != J_STRING) continue;
        if (!evaluate_condition(rt, v->u.string)) return false;
    }
    return true;
}

static bool preflight_checks(Runtime *rt) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Running preflight checks");
    rt->is_root = geteuid() == 0;
    if (!rt->is_root) {
        runtime_fail(rt, "this tool must be run as root");
        return false;
    }
    parse_os_release(rt);
    if (!rt->is_tumbleweed) {
        runtime_fail(rt, "unsupported distro: detected ID='%s' VERSION='%s'; expected openSUSE Tumbleweed", rt->distro_id, rt->distro_version);
        return false;
    }
    if (!command_exists("zypper")) {
        runtime_fail(rt, "zypper not found in PATH");
        return false;
    }
    rt->is_laptop = detect_laptop_mode();
    rt->network_online = detect_network();
    detect_disk_space(rt);
    rt->x11_present = detect_x11_present();
    detect_display_manager(rt);
    rt->existing_desktop_present = detect_existing_desktop();
    if (!detect_target_user(rt)) {
        runtime_fail(rt, "could not determine target desktop user; pass --target-user USER");
        return false;
    }
    if (!rt->network_online && json_bool_value(json_obj_get_path(rt->root, "preflight.require_network"), true)) {
        runtime_fail(rt, "no default network route detected; network is required by policy");
        return false;
    }
    int min_disk = json_int_value(json_obj_get_path(rt->root, "detection.minimum_root_free_mb"), 4096);
    if ((int)rt->root_free_mb < min_disk) {
        runtime_fail(rt, "insufficient free space on /: have %llu MiB, need at least %d MiB", rt->root_free_mb, min_disk);
        return false;
    }
    JsonValue *archs = json_obj_get_path(rt->root, "meta.supported_architectures");
    bool arch_ok = false;
    if (archs && archs->type == J_ARRAY) {
        for (size_t i = 0; i < archs->u.array.count; ++i) {
            const char *a = json_string(archs->u.array.items[i]);
            if (a && strcmp(a, rt->arch) == 0) arch_ok = true;
        }
    }
    if (!arch_ok) runtime_warn(rt, "architecture '%s' is not explicitly listed in meta.supported_architectures", rt->arch);
    log_msg(rt, LOG_INFO, "Detected distro=%s version=%s arch=%s user=%s home=%s laptop=%s dm=%s free_root=%lluMiB",
            rt->distro_id, rt->distro_version, rt->arch, rt->target_user, rt->target_home,
            rt->is_laptop ? "yes" : "no",
            rt->display_manager_present ? rt->detected_display_manager : "none",
            rt->root_free_mb);
    if (rt->existing_desktop_present) runtime_warn(rt, "an existing desktop or WM appears to be installed already; suse-monad will add its own session instead of removing others");
    return true;
}

static JsonValue *find_profile(Runtime *rt, const char *name) {
    JsonValue *profiles = json_obj_get(rt->root, "profiles");
    if (!profiles || profiles->type != J_ARRAY) return NULL;
    for (size_t i = 0; i < profiles->u.array.count; ++i) {
        JsonValue *p = profiles->u.array.items[i];
        const char *pn = json_string(json_obj_get(p, "name"));
        if (pn && strcmp(pn, name) == 0) return p;
    }
    return NULL;
}

static bool select_profile(Runtime *rt) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Resolving profile and defaults");
    const char *name = rt->opts.profile[0] ? rt->opts.profile : json_string(json_obj_get_path(rt->root, "defaults.profile"));
    if (!name) {
        runtime_fail(rt, "no profile selected and defaults.profile missing");
        return false;
    }
    JsonValue *profile = find_profile(rt, name);
    if (!profile) {
        runtime_fail(rt, "profile '%s' not found in JSON", name);
        return false;
    }
    snprintf(rt->selected_profile, sizeof(rt->selected_profile), "%s", name);
    snprintf(rt->chosen_terminal, sizeof(rt->chosen_terminal), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.terminal_cmd")), "alacritty"));
    snprintf(rt->chosen_browser, sizeof(rt->chosen_browser), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.browser_cmd")), "firefox"));
    snprintf(rt->chosen_editor, sizeof(rt->chosen_editor), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.editor_cmd")), "nano"));
    snprintf(rt->chosen_file_manager, sizeof(rt->chosen_file_manager), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.file_manager_cmd")), "thunar"));
    snprintf(rt->chosen_launcher, sizeof(rt->chosen_launcher), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.launcher_cmd")), "rofi -show drun"));
    snprintf(rt->chosen_mod_hs, sizeof(rt->chosen_mod_hs), "%s", nz(json_string(json_obj_get_path(rt->root, "xmonad.mod_mask_hs")), "mod4Mask"));
    snprintf(rt->chosen_mod_name, sizeof(rt->chosen_mod_name), "%s", nz(json_string(json_obj_get_path(rt->root, "xmonad.mod_key_name")), "Super"));
    snprintf(rt->wallpaper_path, sizeof(rt->wallpaper_path), "%s", nz(json_string(json_obj_get_path(rt->root, "defaults.wallpaper_path")), "/usr/share/backgrounds/suse-monad/default.jpg"));
    snprintf(rt->host_profile, sizeof(rt->host_profile), "%s", rt->is_laptop ? "laptop" : "desktop");

    JsonValue *feature_values = json_obj_get(profile, "defaults");
    if (feature_values && feature_values->type == J_OBJECT) {
        const char *s;
        if ((s = json_string(json_obj_get(feature_values, "terminal_cmd")))) snprintf(rt->chosen_terminal, sizeof(rt->chosen_terminal), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "browser_cmd")))) snprintf(rt->chosen_browser, sizeof(rt->chosen_browser), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "editor_cmd")))) snprintf(rt->chosen_editor, sizeof(rt->chosen_editor), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "file_manager_cmd")))) snprintf(rt->chosen_file_manager, sizeof(rt->chosen_file_manager), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "launcher_cmd")))) snprintf(rt->chosen_launcher, sizeof(rt->chosen_launcher), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "wallpaper_path")))) snprintf(rt->wallpaper_path, sizeof(rt->wallpaper_path), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "mod_mask_hs")))) snprintf(rt->chosen_mod_hs, sizeof(rt->chosen_mod_hs), "%s", s);
        if ((s = json_string(json_obj_get(feature_values, "mod_key_name")))) snprintf(rt->chosen_mod_name, sizeof(rt->chosen_mod_name), "%s", s);
    }

    const char *explain = json_string(json_obj_get_path(rt->root, "ui.x11_honesty_explanation"));
    if (explain) log_msg(rt, LOG_INFO, "%s", explain);

    log_msg(rt, LOG_INFO, "Profile '%s' selected. Terminal='%s' Browser='%s' Editor='%s' Launcher='%s' Host='%s'",
            rt->selected_profile, rt->chosen_terminal, rt->chosen_browser, rt->chosen_editor, rt->chosen_launcher, rt->host_profile);
    return true;
}

static TokenMap build_base_tokens(Runtime *rt) {
    TokenMap m = {0};
    token_set(&m, "username", rt->target_user);
    token_set(&m, "target_username", rt->target_user);
    token_set(&m, "home_dir", rt->target_home);
    token_set(&m, "target_home", rt->target_home);
    token_set(&m, "group_name", rt->target_group);
    token_set(&m, "chosen_terminal", rt->chosen_terminal);
    token_set(&m, "chosen_browser", rt->chosen_browser);
    token_set(&m, "chosen_editor", rt->chosen_editor);
    token_set(&m, "chosen_file_manager", rt->chosen_file_manager);
    token_set(&m, "chosen_launcher", rt->chosen_launcher);
    token_set(&m, "mod_mask_hs", rt->chosen_mod_hs);
    token_set(&m, "mod_key_name", rt->chosen_mod_name);
    token_set(&m, "wallpaper_path", rt->wallpaper_path);
    token_set(&m, "host_profile", rt->host_profile);
    token_set(&m, "is_laptop", rt->is_laptop ? "true" : "false");
    token_set(&m, "is_desktop", rt->is_laptop ? "false" : "true");
    token_set(&m, "display_manager", rt->display_manager_present ? rt->detected_display_manager : "");
    token_set(&m, "status_bar_launch", nz(json_string(json_obj_get_path(rt->root, "xmonad.status_bar_launch")), "xmobar ~/.config/xmobar/xmobarrc"));
    token_set(&m, "screen_lock_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.screen_lock_cmd")), "loginctl lock-session"));
    token_set(&m, "screenshot_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.screenshot_cmd")), "flameshot gui"));
    token_set(&m, "network_applet_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.network_applet_cmd")), "nm-applet"));
    token_set(&m, "bluetooth_applet_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.bluetooth_applet_cmd")), "blueman-applet"));
    token_set(&m, "notifications_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.notifications_cmd")), "dunst"));
    token_set(&m, "compositor_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.compositor_cmd")), "picom --config ~/.config/picom/picom.conf"));
    token_set(&m, "wallpaper_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.wallpaper_cmd")), "feh --bg-fill {{wallpaper_path}}"));
    token_set(&m, "audio_control_cmd", nz(json_string(json_obj_get_path(rt->root, "xmonad.audio_control_cmd")), "pavucontrol"));
    JsonValue *features = json_obj_get(rt->root, "features");
    if (features && features->type == J_OBJECT) {
        for (size_t i = 0; i < features->u.object.count; ++i) {
            char key[192];
            snprintf(key, sizeof(key), "feature_%s", features->u.object.keys[i]);
            token_set(&m, key, feature_enabled(rt, features->u.object.keys[i]) ? "true" : "false");
        }
    }
    return m;
}

static bool execute_command_template(Runtime *rt, const char *templ, TokenMap *tokens, bool fatal, const char *label) {
    char *cmd = substitute_tokens(templ, tokens);
    CommandResult cr;
    int rc = run_shell_with_retry(rt, cmd, rt->timeout_seconds, true, &cr);
    if (rc != 0) {
        if (fatal) runtime_fail(rt, "%s failed (exit %d): %s%s%s", label, rc, cmd,
                               cr.stderr_data && *cr.stderr_data ? " :: " : "",
                               cr.stderr_data && *cr.stderr_data ? trim(cr.stderr_data) : "");
        else runtime_warn(rt, "%s failed (exit %d): %s%s%s", label, rc, cmd,
                          cr.stderr_data && *cr.stderr_data ? " :: " : "",
                          cr.stderr_data && *cr.stderr_data ? trim(cr.stderr_data) : "");
        free(cmd);
        free_command_result(&cr);
        return false;
    }
    free(cmd);
    free_command_result(&cr);
    return true;
}

static bool run_hooks(Runtime *rt, const char *hook_name, TokenMap *tokens) {
    JsonValue *hooks = json_obj_get_path(rt->root, "commands.hooks");
    JsonValue *arr = json_obj_get(hooks, hook_name);
    if (!arr) return true;
    if (arr->type == J_STRING) return execute_command_template(rt, arr->u.string, tokens, false, hook_name);
    if (arr->type != J_ARRAY) return true;
    bool ok = true;
    for (size_t i = 0; i < arr->u.array.count; ++i) {
        JsonValue *v = arr->u.array.items[i];
        if (v->type != J_STRING) continue;
        if (!execute_command_template(rt, v->u.string, tokens, false, hook_name)) ok = false;
    }
    return ok;
}

static bool configure_repositories(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Configuring repositories");
    JsonValue *repos = json_obj_get(rt->root, "repositories");
    const char *verify_cmd_t = json_string(json_obj_get_path(rt->root, "commands.repo_verify"));
    const char *add_cmd_t = json_string(json_obj_get_path(rt->root, "commands.repo_add"));
    const char *refresh_cmd_t = json_string(json_obj_get_path(rt->root, "commands.repo_refresh"));
    for (size_t i = 0; repos && i < repos->u.array.count; ++i) {
        JsonValue *r = repos->u.array.items[i];
        if (!r || r->type != J_OBJECT) continue;
        if (!json_bool_value(json_obj_get(r, "enabled"), true)) continue;
        if (!evaluate_conditions_array(rt, json_obj_get(r, "conditions"))) continue;
        TokenMap tm = *base_tokens;
        const char *alias = json_string(json_obj_get(r, "alias"));
        const char *url = json_string(json_obj_get(r, "url"));
        char priority[32], enabled[16], autorefresh[16], gpg[16];
        snprintf(priority, sizeof(priority), "%d", json_int_value(json_obj_get(r, "priority"), 90));
        snprintf(enabled, sizeof(enabled), "%d", json_bool_value(json_obj_get(r, "enabled"), true) ? 1 : 0);
        snprintf(autorefresh, sizeof(autorefresh), "%d", json_bool_value(json_obj_get(r, "auto_refresh"), true) ? 1 : 0);
        snprintf(gpg, sizeof(gpg), "%d", json_bool_value(json_obj_get(r, "gpg_check"), true) ? 1 : 0);
        token_set(&tm, "repo.alias", nz(alias, ""));
        token_set(&tm, "repo.url", nz(url, ""));
        token_set(&tm, "repo.priority", priority);
        token_set(&tm, "repo.enabled", enabled);
        token_set(&tm, "repo.autorefresh", autorefresh);
        token_set(&tm, "repo.gpg_check", gpg);
        bool exists = false;
        if (verify_cmd_t && alias) {
            char *cmd = substitute_tokens(verify_cmd_t, &tm);
            int rc = run_shell(rt, cmd, 20, false, NULL);
            free(cmd);
            exists = (rc == 0);
        }
        if (!exists && add_cmd_t) {
            const char *purpose = json_string(json_obj_get(r, "purpose"));
            log_msg(rt, LOG_INFO, "Adding repository '%s'%s%s", nz(alias, "(unnamed)"), purpose ? " - " : "", purpose ? purpose : "");
            bool fatal = !json_bool_value(json_obj_get(r, "optional"), false);
            if (!execute_command_template(rt, add_cmd_t, &tm, fatal, nz(alias, "repo_add"))) {
                if (fatal) return false;
            }
        }
        if (refresh_cmd_t) execute_command_template(rt, refresh_cmd_t, &tm, false, nz(alias, "repo_refresh"));
    }
    return true;
}

static bool system_prepare(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Running system preparation stage");
    JsonValue *prepare = json_obj_get_path(rt->root, "commands.prepare");
    if (prepare && prepare->type == J_ARRAY) {
        for (size_t i = 0; i < prepare->u.array.count; ++i) {
            JsonValue *v = prepare->u.array.items[i];
            if (v->type != J_STRING) continue;
            if (!execute_command_template(rt, v->u.string, base_tokens, false, "prepare")) {
                if (!rt->continue_on_nonfatal) return false;
            }
        }
    }
    return true;
}

static bool rpm_installed(Runtime *rt, const char *name) {
    char *cmd = str_printf("rpm -q %s >/dev/null 2>&1", name);
    int rc = run_shell(rt, cmd, 20, false, NULL);
    free(cmd);
    return rc == 0;
}

static bool package_candidate_exists(Runtime *rt, const char *name, TokenMap *base_tokens) {
    const char *templ = json_string(json_obj_get_path(rt->root, "commands.search"));
    if (!templ || !name) return false;
    TokenMap tm = *base_tokens;
    token_set(&tm, "package.name", name);
    char *cmd = substitute_tokens(templ, &tm);
    int rc = run_shell(rt, cmd, 60, false, NULL);
    free(cmd);
    return rc == 0;
}

static bool install_package_name(Runtime *rt, const char *pkgname, TokenMap *base_tokens, const char *label) {
    const char *templ = json_string(json_obj_get_path(rt->root, "commands.install"));
    if (!templ || !pkgname) return false;
    TokenMap tm = *base_tokens;
    token_set(&tm, "package.name", pkgname);
    token_set(&tm, "package.label", label ? label : pkgname);
    return execute_command_template(rt, templ, &tm, false, pkgname);
}

static bool verify_package(Runtime *rt, JsonValue *pkg, TokenMap *base_tokens, const char *installed_name) {
    const char *verify = json_string(json_obj_get(pkg, "verification_command"));
    if (verify && *verify) return execute_command_template(rt, verify, base_tokens, false, nz(installed_name, "package verify"));
    return rpm_installed(rt, installed_name);
}

static bool install_one_package(Runtime *rt, const char *pkg_id, TokenMap *base_tokens) {
    JsonValue *packages = json_obj_get(rt->root, "packages");
    JsonValue *pkg = json_obj_get(packages, pkg_id);
    if (!pkg || pkg->type != J_OBJECT) {
        runtime_warn(rt, "package id '%s' not found in packages section", pkg_id);
        return rt->continue_on_nonfatal;
    }
    if (!evaluate_conditions_array(rt, json_obj_get(pkg, "conditions"))) {
        log_msg(rt, LOG_INFO, "Skipping package '%s' due to conditions", pkg_id);
        return true;
    }
    const char *label = json_string(json_obj_get(pkg, "label"));
    const char *preferred = json_string(json_obj_get(pkg, "name"));
    bool required = json_bool_value(json_obj_get(pkg, "required"), true);
    if (preferred && rpm_installed(rt, preferred)) {
        log_msg(rt, LOG_SUCCESS, "Package already installed: %s", preferred);
        return true;
    }
    const char *candidates[32];
    size_t ccount = 0;
    if (preferred) candidates[ccount++] = preferred;
    JsonValue *fallbacks = json_obj_get(pkg, "fallback_package_names");
    if (fallbacks && fallbacks->type == J_ARRAY) {
        for (size_t i = 0; i < fallbacks->u.array.count && ccount < 32; ++i) {
            const char *s = json_string(fallbacks->u.array.items[i]);
            if (s) candidates[ccount++] = s;
        }
    }
    const char *selected = NULL;
    for (size_t i = 0; i < ccount; ++i) {
        if (package_candidate_exists(rt, candidates[i], base_tokens)) { selected = candidates[i]; break; }
    }
    if (!selected && ccount > 0) selected = candidates[0];
    if (!selected) {
        if (required) runtime_fail(rt, "no candidate package available for '%s'", pkg_id);
        else runtime_warn(rt, "no candidate package available for optional '%s'", pkg_id);
        return !required || rt->continue_on_nonfatal;
    }
    log_msg(rt, LOG_INFO, "Installing package: %s (%s)", selected, label ? label : pkg_id);
    if (!install_package_name(rt, selected, base_tokens, nz(label, pkg_id))) {
        bool installed = false;
        for (size_t i = 0; i < ccount; ++i) {
            if (candidates[i] == selected) continue;
            runtime_warn(rt, "trying fallback package '%s' for '%s'", candidates[i], pkg_id);
            if (install_package_name(rt, candidates[i], base_tokens, nz(label, pkg_id))) {
                selected = candidates[i]; installed = true; break;
            }
        }
        if (!installed) {
            if (required) runtime_fail(rt, "failed to install required package '%s'", pkg_id);
            else runtime_warn(rt, "failed to install optional package '%s'", pkg_id);
            return !required || rt->continue_on_nonfatal;
        }
    }
    if (!verify_package(rt, pkg, base_tokens, selected)) {
        if (required) runtime_fail(rt, "package '%s' did not pass verification", selected);
        else runtime_warn(rt, "package '%s' did not pass verification", selected);
        if (required && !rt->continue_on_nonfatal) return false;
    }
    if (!slist_contains(&rt->packages_installed, selected)) slist_add(&rt->packages_installed, selected);
    JsonValue *post = json_obj_get(pkg, "post_install_actions");
    if (post && post->type == J_ARRAY) {
        TokenMap tm = *base_tokens;
        token_set(&tm, "package.name", selected);
        for (size_t i = 0; i < post->u.array.count; ++i) {
            JsonValue *cmd = post->u.array.items[i];
            if (cmd->type != J_STRING) continue;
            execute_command_template(rt, cmd->u.string, &tm, false, "post_install");
        }
    }
    return true;
}

static bool install_groups(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Installing package groups");
    JsonValue *profile = find_profile(rt, rt->selected_profile);
    if (!profile) { runtime_fail(rt, "selected profile disappeared"); return false; }
    JsonValue *groups = json_obj_get(profile, "package_groups");
    JsonValue *group_defs = json_obj_get(rt->root, "package_groups");
    run_hooks(rt, "before_all", base_tokens);
    if (!groups || groups->type != J_ARRAY) {
        runtime_fail(rt, "profile '%s' has no package_groups array", rt->selected_profile);
        return false;
    }
    for (size_t gi = 0; gi < groups->u.array.count; ++gi) {
        const char *group_name = json_string(groups->u.array.items[gi]);
        if (!group_name) continue;
        JsonValue *group = json_obj_get(group_defs, group_name);
        if (!group || group->type != J_OBJECT) {
            runtime_warn(rt, "package group '%s' missing", group_name);
            continue;
        }
        if (!evaluate_conditions_array(rt, json_obj_get(group, "conditions"))) {
            log_msg(rt, LOG_INFO, "Skipping group '%s' due to conditions", group_name);
            continue;
        }
        TokenMap group_tokens = *base_tokens;
        token_set(&group_tokens, "group.name", group_name);
        run_hooks(rt, "before_group", &group_tokens);
        log_msg(rt, LOG_INFO, "Group: %s", group_name);
        JsonValue *members = json_obj_get(group, "packages");
        if (!members || members->type != J_ARRAY) continue;
        for (size_t pi = 0; pi < members->u.array.count; ++pi) {
            const char *pkg_id = json_string(members->u.array.items[pi]);
            if (!pkg_id) continue;
            if (!install_one_package(rt, pkg_id, base_tokens) && !rt->continue_on_nonfatal) return false;
        }
        run_hooks(rt, "after_group", &group_tokens);
    }
    run_hooks(rt, "after_all", base_tokens);
    return true;
}

static bool backup_file(Runtime *rt, const char *path) {
    if (!file_exists(path)) return true;
    const char *dir = json_string(json_obj_get_path(rt->root, "backups.directory"));
    if (!dir || !*dir) dir = "/var/lib/suse-monad/backups";
    uid_t uid = 0; gid_t gid = 0;
    if (!ensure_parent_dir(rt, dir, 0755, uid, gid)) return false;
    char backup[SUSE_MONAD_MAX_PATH];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(backup, sizeof(backup), "%s/%s.%s.bak", dir, base, stamp);
    char *content = read_file_all(path, NULL);
    if (!content) return false;
    char err[512];
    bool ok = write_file_atomic(backup, content, 0644, 0, 0, err, sizeof(err));
    if (ok) slist_add(&rt->backups_written, backup); else runtime_warn(rt, "backup failed for %s: %s", path, err);
    free(content);
    return ok;
}

static bool ensure_parent_dir(Runtime *rt, const char *path, mode_t mode, uid_t uid, gid_t gid) {
    char tmp[SUSE_MONAD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (!dir_exists(tmp) && mkdir(tmp, mode) != 0 && errno != EEXIST) {
                runtime_fail(rt, "mkdir %s failed: %s", tmp, strerror(errno));
                return false;
            }
            chown(tmp, uid, gid);
            chmod(tmp, mode);
            *p = '/';
        }
    }
    return true;
}

static bool resolve_owner_group(Runtime *rt, JsonValue *file_spec, uid_t *uid, gid_t *gid) {
    const char *owner = json_string(json_obj_get(file_spec, "owner"));
    const char *group = json_string(json_obj_get(file_spec, "group"));
    if (!owner) owner = json_string(json_obj_get_path(rt->root, "permissions.default_owner"));
    if (!group) group = json_string(json_obj_get_path(rt->root, "permissions.default_group"));
    TokenMap m = build_base_tokens(rt);
    char *owner_s = substitute_tokens(nz(owner, "root"), &m);
    char *group_s = substitute_tokens(nz(group, "root"), &m);
    struct passwd *pw = getpwnam(owner_s);
    struct group *gr = getgrnam(group_s);
    free(owner_s); free(group_s);
    if (!pw) pw = getpwnam("root");
    if (!gr) gr = getgrnam("root");
    if (!pw || !gr) return false;
    *uid = pw->pw_uid; *gid = gr->gr_gid;
    return true;
}

static bool write_file_from_spec(Runtime *rt, JsonValue *file_spec, TokenMap *base_tokens) {
    if (!file_spec || file_spec->type != J_OBJECT) return true;
    if (!evaluate_conditions_array(rt, json_obj_get(file_spec, "conditions"))) return true;
    const char *dest_t = json_string(json_obj_get(file_spec, "destination"));
    if (!dest_t) { runtime_warn(rt, "file spec without destination skipped"); return true; }
    char *dest = substitute_tokens(dest_t, base_tokens);
    bool overwrite = json_bool_value(json_obj_get(file_spec, "overwrite"), true);
    bool do_backup = json_bool_value(json_obj_get(file_spec, "backup"), true);
    if (file_exists(dest) && !overwrite) {
        log_msg(rt, LOG_INFO, "Keeping existing file (overwrite=false): %s", dest);
        free(dest);
        return true;
    }
    uid_t uid = 0; gid_t gid = 0;
    if (!resolve_owner_group(rt, file_spec, &uid, &gid)) {
        runtime_fail(rt, "cannot resolve owner/group for %s", dest);
        free(dest);
        return false;
    }
    if (!ensure_parent_dir(rt, dest, 0755, uid, gid)) { free(dest); return false; }
    if (do_backup) backup_file(rt, dest);
    mode_t mode = (mode_t)strtol(nz(json_string(json_obj_get(file_spec, "mode")), "0644"), NULL, 8);

    const char *inline_content = json_string(json_obj_get(file_spec, "inline_content"));
    const char *template_name = json_string(json_obj_get(file_spec, "template"));
    const char *content_src = inline_content;
    if (!content_src && template_name) {
        JsonValue *tmpl = json_obj_get(json_obj_get(rt->root, "templates"), template_name);
        content_src = json_string(tmpl);
    }
    if (!content_src) { runtime_fail(rt, "no content/template for file %s", dest); free(dest); return false; }
    TokenMap tm = *base_tokens;
    token_set(&tm, "file.destination", dest);
    run_hooks(rt, "before_file_write", &tm);
    char *content = substitute_tokens(content_src, &tm);
    char err[512];
    bool ok = write_file_atomic(dest, content, mode, uid, gid, err, sizeof(err));
    if (!ok) runtime_fail(rt, "write failed for %s: %s", dest, err);
    else {
        log_msg(rt, LOG_SUCCESS, "Wrote %s", dest);
        slist_add(&rt->files_written, dest);
    }
    run_hooks(rt, "after_file_write", &tm);
    free(dest);
    free(content);
    return ok;
}

static bool generate_files(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Generating files and templates");
    JsonValue *files = json_obj_get(rt->root, "files");
    if (!files || files->type != J_ARRAY) return true;
    bool ok = true;
    for (size_t i = 0; i < files->u.array.count; ++i) {
        if (!write_file_from_spec(rt, files->u.array.items[i], base_tokens)) {
            ok = false;
            if (!rt->continue_on_nonfatal) return false;
        }
    }
    return ok;
}

static bool configure_services(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Enabling configured services");
    JsonValue *services = json_obj_get(rt->root, "services");
    const char *templ = json_string(json_obj_get_path(rt->root, "commands.service_enable"));
    if (!templ) return true;
    for (size_t i = 0; services && i < services->u.array.count; ++i) {
        JsonValue *svc = services->u.array.items[i];
        if (!svc || svc->type != J_OBJECT) continue;
        if (!json_bool_value(json_obj_get(svc, "enabled"), true)) continue;
        if (!evaluate_conditions_array(rt, json_obj_get(svc, "conditions"))) continue;
        const char *name = json_string(json_obj_get(svc, "name"));
        if (!name) continue;
        const char *scope = json_string(json_obj_get(svc, "scope"));
        TokenMap tm = *base_tokens;
        token_set(&tm, "service.name", name);
        token_set(&tm, "service.scope", nz(scope, "system"));
        run_hooks(rt, "before_service_enable", &tm);
        bool fatal = json_bool_value(json_obj_get(svc, "required"), false);
        if (execute_command_template(rt, templ, &tm, fatal, name)) {
            slist_add(&rt->services_enabled, name);
        } else if (fatal && !rt->continue_on_nonfatal) {
            return false;
        }
        run_hooks(rt, "after_service_enable", &tm);
    }
    return true;
}

static bool verify_rules(Runtime *rt, TokenMap *base_tokens) {
    rt->current_step++;
    log_msg(rt, LOG_INFO, "Running verification rules");
    JsonValue *rules = json_obj_get(rt->root, "verification");
    bool ok = true;
    for (size_t i = 0; rules && i < rules->u.array.count; ++i) {
        JsonValue *r = rules->u.array.items[i];
        if (!r || r->type != J_OBJECT) continue;
        if (!evaluate_conditions_array(rt, json_obj_get(r, "conditions"))) continue;
        const char *type = json_string(json_obj_get(r, "type"));
        const char *desc = json_string(json_obj_get(r, "description"));
        bool required = json_bool_value(json_obj_get(r, "required"), true);
        bool passed = false;
        if (!type) continue;
        if (strcmp(type, "command_exists") == 0) {
            char *cmd = substitute_tokens(nz(json_string(json_obj_get(r, "command")), ""), base_tokens);
            passed = command_exists(cmd);
            free(cmd);
        } else if (strcmp(type, "package_installed") == 0) {
            char *pkg = substitute_tokens(nz(json_string(json_obj_get(r, "package")), ""), base_tokens);
            passed = rpm_installed(rt, pkg);
            free(pkg);
        } else if (strcmp(type, "service_enabled") == 0) {
            char *svc = substitute_tokens(nz(json_string(json_obj_get(r, "service")), ""), base_tokens);
            char *cmd = str_printf("systemctl is-enabled %s >/dev/null 2>&1", svc);
            passed = run_shell(rt, cmd, 15, false, NULL) == 0;
            free(cmd); free(svc);
        } else if (strcmp(type, "file_exists") == 0) {
            char *path = substitute_tokens(nz(json_string(json_obj_get(r, "path")), ""), base_tokens);
            passed = file_exists(path);
            free(path);
        } else if (strcmp(type, "file_contains") == 0) {
            char *path = substitute_tokens(nz(json_string(json_obj_get(r, "path")), ""), base_tokens);
            char *token = substitute_tokens(nz(json_string(json_obj_get(r, "token")), ""), base_tokens);
            char *text = read_file_all(path, NULL);
            passed = text && strstr(text, token) != NULL;
            free(path); free(token); free(text);
        } else if (strcmp(type, "command_success") == 0) {
            const char *templ = json_string(json_obj_get(r, "command"));
            char *cmd = substitute_tokens(nz(templ, ""), base_tokens);
            passed = run_shell(rt, cmd, rt->timeout_seconds, false, NULL) == 0;
            free(cmd);
        } else if (strcmp(type, "ownership") == 0) {
            char *path = substitute_tokens(nz(json_string(json_obj_get(r, "path")), ""), base_tokens);
            struct stat st;
            passed = false;
            if (stat(path, &st) == 0) {
                struct passwd *pw = getpwnam(rt->target_user);
                passed = pw && st.st_uid == pw->pw_uid;
            }
            free(path);
        }
        if (passed) log_msg(rt, LOG_SUCCESS, "Verified: %s", nz(desc, type));
        else {
            if (required) runtime_fail(rt, "verification failed: %s", nz(desc, type));
            else runtime_warn(rt, "verification warning: %s", nz(desc, type));
            if (required) ok = false;
        }
    }
    return ok || rt->continue_on_nonfatal;
}

static void rollback_files_if_requested(Runtime *rt) {
    if (!json_bool_value(json_obj_get_path(rt->root, "rollback.restore_files_on_failure"), false)) return;
    if (rt->failures.count == 0 || rt->backups_written.count == 0 || rt->opts.dry_run) return;
    runtime_warn(rt, "automatic package/service rollback is not attempted; file backups were preserved in the backup directory for manual restore");
}

static void final_report(Runtime *rt) {
    rt->current_step++;
    log_msg(rt, rt->failures.count ? LOG_WARN : LOG_SUCCESS, "Final report");
    fprintf(stderr, "\nSummary\n=======\n");
    fprintf(stderr, "Profile: %s\n", rt->selected_profile);
    fprintf(stderr, "Target user: %s\n", rt->target_user);
    fprintf(stderr, "Installed packages tracked: %zu\n", rt->packages_installed.count);
    fprintf(stderr, "Files written tracked: %zu\n", rt->files_written.count);
    fprintf(stderr, "Services enabled tracked: %zu\n", rt->services_enabled.count);
    fprintf(stderr, "Warnings: %zu\n", rt->warnings.count);
    fprintf(stderr, "Failures: %zu\n", rt->failures.count);

    if (rt->warnings.count) {
        fprintf(stderr, "\nWarnings\n--------\n");
        for (size_t i = 0; i < rt->warnings.count; ++i) fprintf(stderr, "- %s\n", rt->warnings.items[i]);
    }
    if (rt->failures.count) {
        fprintf(stderr, "\nFailures\n--------\n");
        for (size_t i = 0; i < rt->failures.count; ++i) fprintf(stderr, "- %s\n", rt->failures.items[i]);
    }

    const char *next = json_string(json_obj_get_path(rt->root, "ui.final_next_steps"));
    const char *rec = json_string(json_obj_get_path(rt->root, "recovery.general_hint"));
    if (next) fprintf(stderr, "\nNext steps\n----------\n%s\n", next);
    if (rt->failures.count && rec) fprintf(stderr, "\nRecovery hint\n-------------\n%s\n", rec);
}

int main(int argc, char **argv) {
    Runtime rt;
    memset(&rt, 0, sizeof(rt));
    rt.total_steps = 9;
    rt.color = isatty(STDERR_FILENO);

    if (!parse_args(argc, argv, &rt.opts)) return 2;
    size_t json_len = 0;
    char *json_text = read_file_all(rt.opts.config_path, &json_len);
    if (!json_text) {
        fprintf(stderr, "fatal: cannot read config %s: %s\n", rt.opts.config_path, strerror(errno));
        return 2;
    }
    char jerr[512];
    rt.root = json_parse_text(json_text, jerr, sizeof(jerr));
    free(json_text);
    if (!rt.root) {
        fprintf(stderr, "fatal: invalid JSON: %s\n", jerr);
        return 2;
    }
    open_log(&rt);
    load_execution_policy(&rt);
    log_msg(&rt, LOG_INFO, "suse-monad starting with config %s", rt.opts.config_path);

    if (!validate_config(&rt)) {
        final_report(&rt);
        free_json(rt.root);
        if (rt.log_fp) fclose(rt.log_fp);
        return 2;
    }
    if (!preflight_checks(&rt)) {
        rollback_files_if_requested(&rt);
        final_report(&rt);
        free_json(rt.root);
        if (rt.log_fp) fclose(rt.log_fp);
        return 1;
    }
    if (!select_profile(&rt)) {
        rollback_files_if_requested(&rt);
        final_report(&rt);
        free_json(rt.root);
        if (rt.log_fp) fclose(rt.log_fp);
        return 1;
    }

    TokenMap tokens = build_base_tokens(&rt);
    const char *summary_t = json_string(json_obj_get_path(rt.root, "ui.plan_summary"));
    if (summary_t) {
        char *summary = substitute_tokens(summary_t, &tokens);
        log_msg(&rt, LOG_INFO, "%s", summary);
        free(summary);
    }
    if (!ask_yes_no(&rt, nz(json_string(json_obj_get_path(rt.root, "ui.confirm_prompt")), "Proceed with installation?"), true)) {
        runtime_warn(&rt, "user declined to continue");
        final_report(&rt);
        free_json(rt.root);
        if (rt.log_fp) fclose(rt.log_fp);
        return 0;
    }

    if (!configure_repositories(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    if (!system_prepare(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    if (!install_groups(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    if (!generate_files(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    if (!configure_services(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    if (!verify_rules(&rt, &tokens) && !rt.continue_on_nonfatal) goto done;
    run_hooks(&rt, "final_success", &tokens);

done:
    rollback_files_if_requested(&rt);
    final_report(&rt);
    int exit_code = rt.failures.count ? 1 : 0;
    slist_free(&rt.warnings);
    slist_free(&rt.failures);
    slist_free(&rt.packages_installed);
    slist_free(&rt.files_written);
    slist_free(&rt.services_enabled);
    slist_free(&rt.backups_written);
    free_json(rt.root);
    if (rt.log_fp) fclose(rt.log_fp);
    return exit_code;
}
