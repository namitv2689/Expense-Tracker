# Expense Tracker — a website built in C, from the basics

A personal expense tracker you use in your browser, where **the web server is a single C file you can read top to bottom**. No frameworks, no database engine, no libraries beyond the C standard library and the operating system's sockets.

```
expense-tracker-c/
├── server.c            ← the whole back end (≈870 lines, heavily commented)
├── public/
│   ├── index.html      ← page structure
│   ├── style.css       ← page looks (light + dark, phone-friendly)
│   └── app.js          ← page behaviour: talks to server.c
├── Makefile            ← "make run" on Linux / macOS / MSYS2
├── build_and_run.bat   ← double-click build on Windows
└── expenses.csv        ← created automatically; your data lives here
```

**Features:** add expenses (date, category, amount in ₹, note) · delete them · filter by month or category · totals, transaction count, average and top category · spending by category and month-by-month bars · data saved to a CSV file you can open in Excel · works on phone-width screens and in dark mode.

---

## 1. Run it in 2 minutes

You need a C compiler (`gcc`).

| Your computer | Install gcc | Build & run |
|---|---|---|
| **Windows** | Install [MSYS2](https://www.msys2.org), then in the *MSYS2 UCRT64* terminal: `pacman -S mingw-w64-ucrt-x86_64-gcc`. Add `C:\msys64\ucrt64\bin` to your PATH. | Double-click `build_and_run.bat` — or in a terminal: `gcc -o server.exe server.c -lws2_32` then `server.exe` |
| **macOS** | `xcode-select --install` | `make run` |
| **Linux** | `sudo apt install build-essential` | `make run` |

Then open **http://localhost:8080**. Stop the server with **Ctrl + C**.

> Run the server **from inside the project folder** — it looks for `public/` and `expenses.csv` relative to where you start it.
> Port 8080 busy? Start it on another one: `./server 9000`.

---

## 2. The big idea: what *is* a website?

A website is just two programs talking over a network:

```
   ┌──────────────┐   1. "GET /"  (please send the page)      ┌──────────────────┐
   │              │ ────────────────────────────────────────▶ │                  │
   │   BROWSER    │ ◀──────────────────────────────────────── │   server.c       │
   │  (Chrome)    │   2. index.html, style.css, app.js        │  (your program)  │
   │              │                                           │                  │
   │  app.js runs │   3. "POST /api/expenses  amount=250"     │  validates,      │
   │  in the page │ ────────────────────────────────────────▶ │  saves to        │──▶ expenses.csv
   │              │ ◀──────────────────────────────────────── │  expenses.csv    │
   └──────────────┘   4. {"id":7,"amount":250.00,...}  (JSON) └──────────────────┘
```

* The **browser** asks for things using **HTTP** — a plain-text format.
* The **server** (our C program) reads the request, decides what to do, and writes back a plain-text response.
* **Steps 1–2** deliver the page. **Steps 3–4** happen every time you add, delete or filter — the page asks the server for data and redraws itself. That second part is called an **API**.

Because HTTP is just text, you can see it yourself. With the server running, try:

```bash
curl -v http://localhost:8080/api/categories
```

You'll see the request lines starting with `>` and the response starting with `<` — exactly what `server.c` reads and writes.

---

## 3. Tour of `server.c` (read along with the file)

The file is split into numbered **PARTS**, ordered from simplest to the `main()` that ties them together.

### PART 0 — Platform differences
Windows calls its socket library *Winsock* and needs different headers and `closesocket()` instead of `close()`. A few `#ifdef _WIN32` lines hide that so everything else is written once.

### PART 1 — The data model
```c
typedef struct {
    int       id;
    char      date[11];        /* "2026-10-16" */
    char      category[32];
    long long amount_paise;    /* ₹250.50 -> 25050 */
    char      note[121];
} Expense;

static Expense expenses[MAX_EXPENSES];
```
All expenses sit in a plain array in memory. **Money is stored as whole paise, never as `float`/`double`** — floating point can't store 0.10 exactly, so totals drift. Try `printf("%.20f", 0.1 + 0.2);` to see why.

### PART 2–3 — Helpers and validation
Every value from the browser is untrusted. `valid_date()` rejects `2026-02-30`; `parse_amount()` turns `"250.5"` into `25050` paise digit by digit and rejects `-5`, `1.234`, `abc`; `valid_category()` only allows the fixed list; `clean_text()` strips commas and newlines so a note can't break the CSV file.

### PART 4 — Saving to a file
`expenses.csv` is plain text, one expense per line. Saving writes to `expenses.csv.tmp` first and then **renames** it over the real file — if the program crashes mid-write, your old data is still safe. `load_expenses()` reads it back at start-up and skips any corrupted rows.

### PART 5 — Building JSON
JSON is the text format the browser understands (`{"id":1,"amount":250.50}`). Since we don't know how long a response will be, `Buffer` is a small **growable string** using `realloc()`. `buf_json_string()` escapes `"` and `\` so a note like `He said "hi"` doesn't break the JSON.

### PART 6 — Sending HTTP responses
Every response is a status line, headers, a blank line, then the body:
```
HTTP/1.1 201 Created
Content-Type: application/json; charset=utf-8
Content-Length: 76

{"id":7,"date":"2026-10-16","category":"Food","amount":250.50,"note":"Lunch"}
```
`send_all()` loops because `send()` is allowed to send only part of the data in one go.

### PART 7 — Reading form data
The browser sends forms *URL-encoded*: `amount=250&note=Chai+%26+samosa`. `+` is a space and `%26` is hex 26 = `&`. `url_decode()` reverses that; `get_param()` finds one field by name.

### PART 8 — Serving the page files
Only four URLs map to files (`/`, `/index.html`, `/style.css`, `/app.js`). Anything else — including sneaky requests like `/../server.c` — gets a 404. **Never** build a file path straight from the URL.

### PART 9 — The API ("business logic")

| Method & path | Body / query | What it does |
|---|---|---|
| `GET /api/categories` | — | The category list (single source of truth) |
| `GET /api/expenses` | `?month=2026-10&category=Food` (both optional) | Matching expenses, newest first |
| `POST /api/expenses` | `date, category, amount, note` | Validates, adds, saves → `201 Created` |
| `POST /api/expenses/delete` | `id` | Removes and saves → `404` if no such id |
| `GET /api/summary` | `?month=2026-10` (optional) | Total, count, totals per category, last 12 months |

Errors always come back as `{"error":"…"}` with a 4xx/5xx status, and the page shows the message.

### PART 10 — Reading a request and routing it
`handle_client()` keeps calling `recv()` until it has all the headers (ending in a blank line, `\r\n\r\n`) **and** as many body bytes as `Content-Length` says. It reads the first line with `sscanf("%7s %1023s")` to get the method and path, splits off the `?query`, then a chain of `if`s — the **router** — calls the right handler.

### PART 11 — `main()`: the life of a server in five calls
```c
socket()   // make a network endpoint
bind()     // attach it to port 8080 on this computer
listen()   // start accepting connections
accept()   // wait for a browser… (loops forever)
close()    // hang up after answering
```
A 5-second receive timeout stops one silent connection from freezing everything.

---

## 4. The browser side (`public/app.js`)

The page never touches the CSV file — it only calls the API with `fetch()`:

```js
// add an expense
await fetch('/api/expenses', {
  method: 'POST',
  body: new URLSearchParams({ date, category, amount, note })  // → "date=…&category=…"
});
```

After every change it calls `refresh()`, which fetches `/api/summary` and `/api/expenses` and redraws the cards, bars and table. Text from the server is inserted with `textContent` (never `innerHTML`), so a note containing `<script>` is shown as text, not run.

---

## 5. Try it by hand with `curl`

```bash
curl http://localhost:8080/api/categories
curl -d "date=2026-10-16&category=Food&amount=120&note=Lunch" http://localhost:8080/api/expenses
curl "http://localhost:8080/api/expenses?month=2026-10"
curl "http://localhost:8080/api/summary?month=2026-10"
curl -d "id=1" http://localhost:8080/api/expenses/delete
curl -d "date=2026-02-30&category=Food&amount=5" http://localhost:8080/api/expenses   # → 400 error
```
(On Windows PowerShell use `curl.exe` instead of `curl`.)

---

## 6. Things to know (limits by design)

* **One request at a time.** The server handles connections one after another. That's plenty for one person, but a slow client can delay others by up to 5 s.
* **Local only.** It listens on `127.0.0.1`, so only your own computer can open it. To use it from your phone on the same Wi-Fi, change `INADDR_LOOPBACK` to `INADDR_ANY` in `main()` and open `http://<your-PC's-IP>:8080` — but there's **no login**, so anyone on that network could see and edit your data.
* **Up to 10,000 expenses** in memory (`MAX_EXPENSES`). Each save rewrites the whole file — fine at this size.

---

## 7. Exercises — grow it yourself

Roughly easiest → hardest:

1. **New category.** Add `"Rent"` to `CATEGORIES[]`. Rebuild. Notice the page picks it up without touching HTML — why?
2. **Edit an expense.** Add `POST /api/expenses/update` (body: `id` + fields). Reuse the validation from `handle_add()`.
3. **Monthly budget.** Store a budget per category in a second file and return `"budget"` in `/api/summary`; show the bars turning red when you go over.
4. **Export.** Add `GET /api/export.csv` that sends `expenses.csv` with `Content-Type: text/csv` and `Content-Disposition: attachment`.
5. **Search.** Add `?q=chai` to `/api/expenses` that matches the note (case-insensitive — you already have `starts_with_nocase` as inspiration).
6. **Concurrency.** Handle each client in its own thread (`pthread_create` on Linux/macOS, `CreateThread` on Windows). You'll then need a mutex around the `expenses` array — find out why.
7. **SQLite.** Replace PART 4 with the SQLite C library (`sqlite3_open`, `sqlite3_prepare_v2`, …), the same database your Python version used.
