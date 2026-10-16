/*
 * ============================================================================
 *  Expense Tracker — a complete web application written in plain C
 * ============================================================================
 *
 *  What this program does
 *  ----------------------
 *  It is a tiny web server. When you run it and open http://localhost:8080
 *  in a browser, it:
 *
 *    1. sends the browser the web page (public/index.html, style.css, app.js)
 *    2. answers the page's requests for data through a small JSON "API"
 *    3. saves every expense to a plain text file (expenses.csv) so nothing
 *       is lost when you stop the server
 *
 *  No libraries are used beyond the C standard library and the operating
 *  system's socket API. Read this file top to bottom — it is ordered from
 *  the simplest building blocks to the main() function at the very end.
 *
 *  Build:
 *    Linux / macOS :  gcc -Wall -O2 -o server server.c
 *    Windows (MinGW):  gcc -Wall -O2 -o server.exe server.c -lws2_32
 *
 *  Run:
 *    ./server            (listens on port 8080)
 *    ./server 9000       (listens on port 9000)
 * ============================================================================
 */

/* ---------------------------------------------------------------------------
 * PART 0 — Headers and platform differences
 *
 * Windows and Linux/macOS both have "sockets" (the OS feature that lets a
 * program talk over a network), but the header files and a few function
 * names differ. The #ifdef blocks below hide those differences so the rest
 * of the program can be written once.
 * ------------------------------------------------------------------------- */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* ask for POSIX functions on Linux/macOS */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define CLOSE_SOCKET closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
  #define CLOSE_SOCKET close
#endif

/* ---------------------------------------------------------------------------
 * PART 1 — Settings and the data model
 * ------------------------------------------------------------------------- */
#define DEFAULT_PORT   8080
#define REQUEST_MAX    65536          /* biggest HTTP request we accept (64 KB) */
#define MAX_EXPENSES   10000          /* how many expenses we keep in memory    */
#define DATA_FILE      "expenses.csv"
#define DATA_FILE_TMP  "expenses.csv.tmp"
#define PUBLIC_DIR     "public"
#define MAX_AMOUNT_PAISE 1000000000LL /* ₹1,00,00,000 — sanity limit per entry  */

/*
 * One expense. Notice that money is stored as a whole number of PAISE
 * (1 rupee = 100 paise) in a long long, not as a float/double.
 * Floating-point numbers cannot represent values like 0.10 exactly, so
 * adding many of them drifts (0.1 + 0.2 = 0.30000000000000004).
 * Integers never drift — this is how real banking software stores money.
 */
typedef struct {
    int       id;
    char      date[11];        /* "YYYY-MM-DD" + the '\0' terminator       */
    char      category[32];
    long long amount_paise;    /* ₹250.50 is stored as 25050               */
    char      note[121];
} Expense;

/* All expenses live in this array while the server runs. */
static Expense expenses[MAX_EXPENSES];
static int     expense_count = 0;
static int     next_id       = 1;

/* The fixed list of categories. The web page asks the server for this list,
 * so this array is the single place to edit if you want different ones. */
static const char *CATEGORIES[] = {
    "Food", "Transport", "Shopping", "Bills",
    "Entertainment", "Health", "Education", "Other"
};
#define NUM_CATEGORIES ((int)(sizeof CATEGORIES / sizeof CATEGORIES[0]))

/* ---------------------------------------------------------------------------
 * PART 2 — Small string helpers
 * ------------------------------------------------------------------------- */

/* Safe string copy: always '\0'-terminates, never writes past `size`. */
static void copy_str(char *dst, const char *src, size_t size) {
    if (size == 0) return;
    size_t i = 0;
    while (src[i] && i + 1 < size) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Case-insensitive "does `s` start with `prefix`?" (HTTP header names are
 * case-insensitive: "Content-Length" and "content-length" are the same). */
static int starts_with_nocase(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Make user text safe to store in our CSV file: commas would split the
 * line into extra columns and newlines would start a new row, so we turn
 * them (and any other control characters) into spaces. Leading/trailing
 * spaces are trimmed. */
static void clean_text(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ',' || c < 0x20 || c == 0x7f) *p = ' ';
    }
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

/* ---------------------------------------------------------------------------
 * PART 3 — Validation
 *
 * Never trust what arrives over the network. Every value the browser sends
 * is checked here before it is stored.
 * ------------------------------------------------------------------------- */

static int is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Accepts only real calendar dates written as YYYY-MM-DD. */
static int valid_date(const char *s) {
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1900 || y > 2100 || m < 1 || m > 12 || d < 1) return 0;
    int max_day = days_in_month[m - 1] + (m == 2 && is_leap_year(y) ? 1 : 0);
    return d <= max_day;
}

/* Accepts YYYY-MM (used for the month filter). */
static int valid_month(const char *s) {
    if (strlen(s) != 7 || s[4] != '-') return 0;
    for (int i = 0; i < 7; i++) {
        if (i == 4) continue;
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    int m = atoi(s + 5);
    return m >= 1 && m <= 12;
}

static int valid_category(const char *s) {
    for (int i = 0; i < NUM_CATEGORIES; i++)
        if (strcmp(s, CATEGORIES[i]) == 0) return 1;
    return 0;
}

/*
 * Turn text like "250", "250.5" or "250.50" into paise (25000, 25050, 25050)
 * WITHOUT using floating point. Returns 1 on success, 0 if the text is not
 * a valid positive amount with at most 2 decimal places.
 */
static int parse_amount(const char *s, long long *out_paise) {
    long long rupees = 0, paise = 0;
    int digits = 0, decimals = 0;

    while (isdigit((unsigned char)*s)) {
        rupees = rupees * 10 + (*s - '0');
        if (rupees > MAX_AMOUNT_PAISE / 100) return 0;   /* too big */
        s++; digits++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            if (++decimals > 2) return 0;                /* "1.234" not allowed */
            paise = paise * 10 + (*s - '0');
            s++;
        }
        if (decimals == 1) paise *= 10;                  /* "1.5" means 50 paise */
    }
    if (*s != '\0' || (digits == 0 && decimals == 0)) return 0;

    long long total = rupees * 100 + paise;
    if (total <= 0 || total > MAX_AMOUNT_PAISE) return 0;
    *out_paise = total;
    return 1;
}

/* ---------------------------------------------------------------------------
 * PART 4 — Saving to and loading from the CSV file
 *
 * expenses.csv looks like this (you can open it in Excel):
 *
 *   id,date,category,amount_paise,note
 *   1,2026-10-16,Food,25050,Lunch at canteen
 *   2,2026-10-16,Transport,4000,Auto to station
 * ------------------------------------------------------------------------- */

static void load_expenses(void) {
    FILE *f = fopen(DATA_FILE, "r");
    if (!f) {
        printf("No %s yet — starting with an empty list.\n", DATA_FILE);
        return;
    }

    char line[512];
    while (fgets(line, sizeof line, f) && expense_count < MAX_EXPENSES) {
        line[strcspn(line, "\r\n")] = '\0';          /* remove the newline */

        /* Split the line at the first 4 commas into 5 fields. */
        char *fields[5] = {0};
        int n = 0;
        char *p = line;
        fields[n++] = p;
        while (*p && n < 5) {
            if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
            p++;
        }
        if (n < 4) continue;                         /* malformed line   */

        Expense e;
        memset(&e, 0, sizeof e);
        e.id = atoi(fields[0]);
        if (e.id <= 0) continue;                     /* the header line  */
        copy_str(e.date, fields[1], sizeof e.date);
        copy_str(e.category, fields[2], sizeof e.category);
        e.amount_paise = atoll(fields[3]);
        if (n == 5) copy_str(e.note, fields[4], sizeof e.note);

        if (!valid_date(e.date) || !valid_category(e.category) || e.amount_paise <= 0)
            continue;                                /* skip corrupted rows */

        expenses[expense_count++] = e;
        if (e.id >= next_id) next_id = e.id + 1;
    }
    fclose(f);
    printf("Loaded %d expense(s) from %s.\n", expense_count, DATA_FILE);
}

/*
 * Write everything to a temporary file first, then rename it over the real
 * file. If the program crashes half-way through writing, the old file is
 * still intact — you never end up with a half-written expenses.csv.
 */
static int save_expenses(void) {
    FILE *f = fopen(DATA_FILE_TMP, "w");
    if (!f) { perror("save_expenses: fopen"); return 0; }

    fprintf(f, "id,date,category,amount_paise,note\n");
    for (int i = 0; i < expense_count; i++) {
        const Expense *e = &expenses[i];
        fprintf(f, "%d,%s,%s,%lld,%s\n",
                e->id, e->date, e->category, e->amount_paise, e->note);
    }
    if (fclose(f) != 0) { perror("save_expenses: fclose"); return 0; }

#ifdef _WIN32
    remove(DATA_FILE);          /* Windows rename() fails if the target exists */
#endif
    if (rename(DATA_FILE_TMP, DATA_FILE) != 0) { perror("save_expenses: rename"); return 0; }
    return 1;
}

/* ---------------------------------------------------------------------------
 * PART 5 — A growable text buffer, used to build JSON responses
 *
 * We don't know in advance how long a response will be (10 expenses or
 * 10,000?), so this buffer grows itself with realloc() as text is added.
 * ------------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buffer;

static void buf_init(Buffer *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = malloc(b->cap);
    if (!b->data) { fprintf(stderr, "Out of memory\n"); exit(1); }
    b->data[0] = '\0';
}

static void buf_append(Buffer *b, const char *s, size_t n) {
    while (b->len + n + 1 > b->cap) {
        b->cap *= 2;
        char *bigger = realloc(b->data, b->cap);
        if (!bigger) { fprintf(stderr, "Out of memory\n"); exit(1); }
        b->data = bigger;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_printf(Buffer *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    buf_append(b, tmp, (size_t)n);
}

/* Write a string as a JSON string: wrap it in quotes and escape any
 * " or \ inside it, otherwise the browser couldn't parse the JSON. */
static void buf_json_string(Buffer *b, const char *s) {
    buf_append(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            char esc[2] = { '\\', *s };
            buf_append(b, esc, 2);
        } else if ((unsigned char)*s >= 0x20) {
            buf_append(b, s, 1);
        }
    }
    buf_append(b, "\"", 1);
}

/* Write paise as a JSON number in rupees: 25050 -> 250.50 */
static void buf_money(Buffer *b, long long paise) {
    buf_printf(b, "%lld.%02lld", paise / 100, paise % 100);
}

static void buf_expense_json(Buffer *b, const Expense *e) {
    buf_printf(b, "{\"id\":%d,\"date\":", e->id);
    buf_json_string(b, e->date);
    buf_append(b, ",\"category\":", 12);
    buf_json_string(b, e->category);
    buf_append(b, ",\"amount\":", 10);
    buf_money(b, e->amount_paise);
    buf_append(b, ",\"note\":", 8);
    buf_json_string(b, e->note);
    buf_append(b, "}", 1);
}

/* ---------------------------------------------------------------------------
 * PART 6 — Sending HTTP responses
 *
 * Every HTTP response is: a status line, some headers, a blank line, then
 * the body. For example:
 *
 *   HTTP/1.1 200 OK
 *   Content-Type: application/json
 *   Content-Length: 27
 *
 *   {"expenses":[],"count":0}
 * ------------------------------------------------------------------------- */

/* send() may send only part of the data in one call, so loop until done. */
static void send_all(sock_t client, const char *data, size_t len) {
    while (len > 0) {
        int n = send(client, data, (int)len, 0);
        if (n <= 0) return;          /* the browser went away; give up */
        data += n;
        len  -= (size_t)n;
    }
}

static void send_response(sock_t client, int status, const char *status_text,
                          const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int n = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, (unsigned long)body_len);
    send_all(client, header, (size_t)n);
    send_all(client, body, body_len);
    printf("  -> %d %s\n", status, status_text);
}

static void send_json(sock_t client, int status, const char *status_text, Buffer *b) {
    send_response(client, status, status_text, "application/json; charset=utf-8", b->data, b->len);
}

static void send_error(sock_t client, int status, const char *status_text, const char *message) {
    Buffer b;
    buf_init(&b);
    buf_append(&b, "{\"error\":", 9);
    buf_json_string(&b, message);
    buf_append(&b, "}", 1);
    send_json(client, status, status_text, &b);
    free(b.data);
}

/* ---------------------------------------------------------------------------
 * PART 7 — Reading form data and query strings
 *
 * Browsers send form data "URL-encoded":
 *     date=2026-10-16&category=Food&amount=250.50&note=Chai+%26+samosa
 * '+' means a space and %26 is the character with hex code 26, i.e. '&'.
 * The same format is used after the '?' in a URL (the "query string").
 * ------------------------------------------------------------------------- */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t src_len, size_t dst_size) {
    size_t i = 0, j = 0;
    while (j < src_len && i + 1 < dst_size) {
        if (src[j] == '+') {
            dst[i++] = ' '; j++;
        } else if (src[j] == '%' && j + 2 < src_len
                   && hex_value(src[j + 1]) >= 0 && hex_value(src[j + 2]) >= 0) {
            dst[i++] = (char)(hex_value(src[j + 1]) * 16 + hex_value(src[j + 2]));
            j += 3;
        } else {
            dst[i++] = src[j++];
        }
    }
    dst[i] = '\0';
}

/* Find `key` in "a=1&b=2&c=3" and copy its decoded value into `out`.
 * Returns 1 if found, 0 if not (and sets out to ""). */
static int get_param(const char *data, const char *key, char *out, size_t out_size) {
    size_t key_len = strlen(key);
    out[0] = '\0';
    const char *p = data;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            url_decode(out, p + key_len + 1, len - key_len - 1, out_size);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * PART 8 — Serving the web page files
 *
 * Only the files listed here can ever be sent. A request for anything else
 * (for example "/../server.c") gets a 404, so nobody can read other files
 * on your computer through the server.
 * ------------------------------------------------------------------------- */
static void serve_static(sock_t client, const char *path) {
    static const struct { const char *url, *file, *type; } files[] = {
        { "/",           "index.html", "text/html; charset=utf-8" },
        { "/index.html", "index.html", "text/html; charset=utf-8" },
        { "/style.css",  "style.css",  "text/css; charset=utf-8" },
        { "/app.js",     "app.js",     "application/javascript; charset=utf-8" },
    };

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (strcmp(path, files[i].url) != 0) continue;

        char full_path[256];
        snprintf(full_path, sizeof full_path, "%s/%s", PUBLIC_DIR, files[i].file);

        FILE *f = fopen(full_path, "rb");             /* "rb" = read bytes as-is */
        if (!f) { send_error(client, 500, "Internal Server Error", "Page file missing — run the server from the project folder"); return; }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *content = malloc((size_t)size + 1);
        if (!content) { fclose(f); send_error(client, 500, "Internal Server Error", "Out of memory"); return; }
        size_t got = fread(content, 1, (size_t)size, f);
        fclose(f);

        send_response(client, 200, "OK", files[i].type, content, got);
        free(content);
        return;
    }
    send_error(client, 404, "Not Found", "No such page");
}

/* ---------------------------------------------------------------------------
 * PART 9 — The API handlers (the "business logic")
 * ------------------------------------------------------------------------- */

/* GET /api/categories  ->  {"categories":["Food","Transport",...]} */
static void handle_categories(sock_t client) {
    Buffer b;
    buf_init(&b);
    buf_append(&b, "{\"categories\":[", 15);
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        if (i) buf_append(&b, ",", 1);
        buf_json_string(&b, CATEGORIES[i]);
    }
    buf_append(&b, "]}", 2);
    send_json(client, 200, "OK", &b);
    free(b.data);
}

/* Sort newest first: by date, then by id for expenses on the same day. */
static int compare_newest_first(const void *a, const void *b) {
    const Expense *x = *(const Expense * const *)a;
    const Expense *y = *(const Expense * const *)b;
    int c = strcmp(y->date, x->date);
    return c != 0 ? c : (y->id - x->id);
}

/* Read the optional ?month=YYYY-MM&category=Food filters.
 * Returns 0 (after sending a 400 error) if a filter is invalid. */
static int read_filters(sock_t client, const char *query, char *month, char *category) {
    get_param(query, "month", month, 8);
    get_param(query, "category", category, 32);
    if (month[0] && !valid_month(month)) {
        send_error(client, 400, "Bad Request", "month must look like 2026-10");
        return 0;
    }
    if (category[0] && !valid_category(category)) {
        send_error(client, 400, "Bad Request", "Unknown category");
        return 0;
    }
    return 1;
}

static int matches(const Expense *e, const char *month, const char *category) {
    if (month[0] && strncmp(e->date, month, 7) != 0) return 0;
    if (category[0] && strcmp(e->category, category) != 0) return 0;
    return 1;
}

/* GET /api/expenses?month=2026-10&category=Food  ->  {"expenses":[...],"count":N} */
static void handle_list(sock_t client, const char *query) {
    char month[8], category[32];
    if (!read_filters(client, query, month, category)) return;

    /* Collect pointers to the matching expenses, then sort them. */
    const Expense **list = malloc(sizeof *list * (size_t)(expense_count + 1));
    if (!list) { send_error(client, 500, "Internal Server Error", "Out of memory"); return; }
    int n = 0;
    for (int i = 0; i < expense_count; i++)
        if (matches(&expenses[i], month, category)) list[n++] = &expenses[i];
    qsort(list, (size_t)n, sizeof *list, compare_newest_first);

    Buffer b;
    buf_init(&b);
    buf_append(&b, "{\"expenses\":[", 13);
    for (int i = 0; i < n; i++) {
        if (i) buf_append(&b, ",", 1);
        buf_expense_json(&b, list[i]);
    }
    buf_printf(&b, "],\"count\":%d}", n);
    send_json(client, 200, "OK", &b);
    free(b.data);
    free(list);
}

/* POST /api/expenses   body: date=...&category=...&amount=...&note=... */
static void handle_add(sock_t client, const char *body) {
    char date[16], category[48], amount[32], note[256];
    get_param(body, "date", date, sizeof date);
    get_param(body, "category", category, sizeof category);
    get_param(body, "amount", amount, sizeof amount);
    get_param(body, "note", note, sizeof note);

    long long paise;
    if (!valid_date(date))
        { send_error(client, 400, "Bad Request", "Please enter a valid date"); return; }
    if (!valid_category(category))
        { send_error(client, 400, "Bad Request", "Please choose a category"); return; }
    if (!parse_amount(amount, &paise))
        { send_error(client, 400, "Bad Request", "Amount must be a positive number with at most 2 decimals"); return; }
    if (expense_count >= MAX_EXPENSES)
        { send_error(client, 507, "Insufficient Storage", "Expense list is full"); return; }

    Expense e;
    memset(&e, 0, sizeof e);
    e.id = next_id++;
    copy_str(e.date, date, sizeof e.date);
    copy_str(e.category, category, sizeof e.category);
    e.amount_paise = paise;
    clean_text(note);
    copy_str(e.note, note, sizeof e.note);

    expenses[expense_count++] = e;
    if (!save_expenses()) {
        expense_count--;                   /* undo, so memory matches the file */
        send_error(client, 500, "Internal Server Error", "Could not save to disk");
        return;
    }

    Buffer b;
    buf_init(&b);
    buf_expense_json(&b, &e);
    send_json(client, 201, "Created", &b);
    free(b.data);
}

/* POST /api/expenses/delete   body: id=7 */
static void handle_delete(sock_t client, const char *body) {
    char id_text[16];
    get_param(body, "id", id_text, sizeof id_text);
    int id = atoi(id_text);

    for (int i = 0; i < expense_count; i++) {
        if (expenses[i].id != id) continue;

        Expense removed = expenses[i];
        /* Shift everything after position i one place to the left. */
        memmove(&expenses[i], &expenses[i + 1], sizeof(Expense) * (size_t)(expense_count - i - 1));
        expense_count--;

        if (!save_expenses()) {
            /* put it back so memory still matches the file */
            memmove(&expenses[i + 1], &expenses[i], sizeof(Expense) * (size_t)(expense_count - i));
            expenses[i] = removed;
            expense_count++;
            send_error(client, 500, "Internal Server Error", "Could not save to disk");
            return;
        }
        Buffer b;
        buf_init(&b);
        buf_printf(&b, "{\"deleted\":%d}", id);
        send_json(client, 200, "OK", &b);
        free(b.data);
        return;
    }
    send_error(client, 404, "Not Found", "No expense with that id");
}

/* GET /api/summary?month=2026-10
 *   -> totals for the chosen month (or all time), split by category,
 *      plus month-by-month totals for the last 12 months that have data. */
typedef struct { char month[8]; long long total; } MonthTotal;

static int compare_month_desc(const void *a, const void *b) {
    return strcmp(((const MonthTotal *)b)->month, ((const MonthTotal *)a)->month);
}

static void handle_summary(sock_t client, const char *query) {
    char month[8], unused[32];
    if (!read_filters(client, query, month, unused)) return;

    long long total = 0;
    long long by_category[NUM_CATEGORIES] = {0};
    int count = 0;

    for (int i = 0; i < expense_count; i++) {
        const Expense *e = &expenses[i];
        if (!matches(e, month, "")) continue;
        total += e->amount_paise;
        count++;
        for (int c = 0; c < NUM_CATEGORIES; c++)
            if (strcmp(e->category, CATEGORIES[c]) == 0) { by_category[c] += e->amount_paise; break; }
    }

    /* Month-by-month totals across ALL expenses (ignores the month filter). */
    MonthTotal months[1300];   /* 1900–2100 is at most 2412 months; we cap at 1300 distinct */
    int month_count = 0;
    for (int i = 0; i < expense_count; i++) {
        char m[8];
        copy_str(m, expenses[i].date, sizeof m);   /* "2026-10-16" -> "2026-10" */
        int k;
        for (k = 0; k < month_count; k++)
            if (strcmp(months[k].month, m) == 0) break;
        if (k == month_count) {
            if (month_count == (int)(sizeof months / sizeof months[0])) continue;
            copy_str(months[k].month, m, sizeof months[k].month);
            months[k].total = 0;
            month_count++;
        }
        months[k].total += expenses[i].amount_paise;
    }
    qsort(months, (size_t)month_count, sizeof months[0], compare_month_desc);
    if (month_count > 12) month_count = 12;

    Buffer b;
    buf_init(&b);
    buf_append(&b, "{\"month\":", 9);
    buf_json_string(&b, month);
    buf_append(&b, ",\"total\":", 9);
    buf_money(&b, total);
    buf_printf(&b, ",\"count\":%d,\"byCategory\":[", count);
    for (int c = 0; c < NUM_CATEGORIES; c++) {
        if (c) buf_append(&b, ",", 1);
        buf_append(&b, "{\"category\":", 12);
        buf_json_string(&b, CATEGORIES[c]);
        buf_append(&b, ",\"total\":", 9);
        buf_money(&b, by_category[c]);
        buf_append(&b, "}", 1);
    }
    buf_append(&b, "],\"byMonth\":[", 13);
    for (int k = 0; k < month_count; k++) {
        if (k) buf_append(&b, ",", 1);
        buf_append(&b, "{\"month\":", 9);
        buf_json_string(&b, months[k].month);
        buf_append(&b, ",\"total\":", 9);
        buf_money(&b, months[k].total);
        buf_append(&b, "}", 1);
    }
    buf_append(&b, "]}", 2);
    send_json(client, 200, "OK", &b);
    free(b.data);
}

/* ---------------------------------------------------------------------------
 * PART 10 — Reading one HTTP request and routing it
 *
 * A request from the browser looks like:
 *
 *   POST /api/expenses HTTP/1.1          <- request line: METHOD PATH VERSION
 *   Host: localhost:8080                 <- headers
 *   Content-Type: application/x-www-form-urlencoded
 *   Content-Length: 52
 *                                        <- blank line = end of headers
 *   date=2026-10-16&category=Food&amount=250&note=Lunch   <- body
 * ------------------------------------------------------------------------- */
static void handle_client(sock_t client) {
    char *req = malloc(REQUEST_MAX + 1);
    if (!req) return;
    size_t got = 0;
    char *body = NULL;
    long content_length = 0;

    /* Keep reading until we have all the headers AND the whole body. */
    while (got < REQUEST_MAX) {
        int n = recv(client, req + got, (int)(REQUEST_MAX - got), 0);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = '\0';

        char *header_end = strstr(req, "\r\n\r\n");
        if (!header_end) continue;
        body = header_end + 4;

        /* Find the Content-Length header (how many bytes the body has). */
        content_length = 0;
        for (char *line = strstr(req, "\r\n"); line && line < header_end; line = strstr(line + 2, "\r\n")) {
            if (starts_with_nocase(line + 2, "Content-Length:")) {
                content_length = atol(line + 2 + 15);
                break;
            }
        }
        if (content_length < 0 || content_length > REQUEST_MAX) content_length = 0;
        if ((long)(got - (size_t)(body - req)) >= content_length) break;
    }

    if (!body) { free(req); return; }                 /* incomplete / timed out */
    if (body + content_length <= req + got) body[content_length] = '\0';

    /* Parse the request line: "GET /api/expenses?month=2026-10 HTTP/1.1" */
    char method[8] = "", target[1024] = "";
    if (sscanf(req, "%7s %1023s", method, target) != 2) {
        send_error(client, 400, "Bad Request", "Malformed request");
        free(req);
        return;
    }
    printf("%s %s\n", method, target);

    /* Split "/api/expenses?month=2026-10" into path and query string. */
    char *query = strchr(target, '?');
    if (query) *query++ = '\0'; else query = "";
    const char *path = target;

    /* The router: decide which function handles this request. */
    int is_get  = strcmp(method, "GET") == 0;
    int is_post = strcmp(method, "POST") == 0;

    if      (is_get  && strcmp(path, "/api/categories") == 0)     handle_categories(client);
    else if (is_get  && strcmp(path, "/api/expenses") == 0)       handle_list(client, query);
    else if (is_post && strcmp(path, "/api/expenses") == 0)       handle_add(client, body);
    else if (is_post && strcmp(path, "/api/expenses/delete") == 0) handle_delete(client, body);
    else if (is_get  && strcmp(path, "/api/summary") == 0)        handle_summary(client, query);
    else if (is_get  && strncmp(path, "/api/", 5) != 0)           serve_static(client, path);
    else send_error(client, 404, "Not Found", "Unknown API route");

    free(req);
}

/* ---------------------------------------------------------------------------
 * PART 11 — main(): open a socket and serve requests forever
 *
 * The life of a server, in five system calls:
 *   socket()  create a network endpoint
 *   bind()    attach it to a port (8080) on this computer
 *   listen()  start accepting connections
 *   accept()  wait for a browser to connect (loops forever)
 *   close()   hang up after answering
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port: %s\n", argv[1]); return 1; }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);   /* don't crash if a browser disconnects mid-reply */
#endif
    setvbuf(stdout, NULL, _IONBF, 0);   /* print log lines immediately */

    load_expenses();

    sock_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) { perror("socket"); return 1; }

    /* Lets you restart the server immediately without "address in use". */
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /* 127.0.0.1 = only this computer can connect. Use INADDR_ANY to allow
     * other devices on your Wi-Fi (e.g. your phone) — see README. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind (is another program already using this port?)");
        return 1;
    }
    if (listen(server, 16) != 0) { perror("listen"); return 1; }

    printf("\n  Expense Tracker is running.\n  Open http://localhost:%d in your browser.\n  Press Ctrl+C to stop.\n\n", port);

    for (;;) {
        sock_t client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        /* If a client connects but sends nothing, give up after 5 seconds
         * so one stuck connection can't freeze the whole server. */
#ifdef _WIN32
        DWORD timeout_ms = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof timeout_ms);
#else
        struct timeval tv = { 5, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        handle_client(client);
        CLOSE_SOCKET(client);
    }
    /* not reached */
}
