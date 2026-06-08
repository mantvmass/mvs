# vmass — an HTTP framework written in MVS

A small but genuinely working web framework, here as a **demo/test** of the MVS compiler
(especially **function pointers** and the `net` module). It is **not part of the compiler**.

It's built around a **builder pattern** plus **function-pointer routing** (`app.get(path, handler)`),
organized into a few modular files.

## Structure

```
vmass/
├── core.mvs        low-level utilities — str_len / str_eq / substr / alloc / dealloc
├── http.mvs        the HTTP layer — Request (matcher + query), Response (builder), parse
├── vmass.mvs       entry library — App (builder + route table + runner)
└── examples/
    ├── hello.mvs   a single-route server
    └── api.mvs     multiple routes + query + redirect + PUT/DELETE + custom 404
```

> **Code policy:** write as much as possible in pure MVS — `str_len`/`str_eq` are MVS. Use `extern` C
> only where MVS can't do it itself, namely heap management (`malloc`/`free`/`memcpy`, in `core.mvs`).

## Usage — function-pointer routing

```rust
import { App } from "../vmass.mvs";   // loads the whole framework; Request/Response are global structs
import { String } from "std/string";

// a handler is a function pointer: func(*Request) -> String (built via a Response builder + .render())
func home(req: *Request) -> String { return Response::ok().html("<h1>Home</h1>").render(); }
func save(req: *Request) -> String { return Response::created().text("saved").render(); }
func my404(req: *Request) -> String { return Response::not_found().json("{\"err\":\"nope\"}").render(); }

func main() -> i8 {
    App::new()
        .host("0.0.0.0").port(8080).name("myapp")
        .get("/", home)
        .post("/save", save)
        .fallback(my404)        // optional custom 404 handler — stored as a function pointer
        .run();
    return 0;
}
```

Handlers are stored in a **route table** (`struct Route { method; path; handler: func(*Request)->String }`)
on the heap; `dispatch()` matches method+path and **calls the handler indirectly through the function pointer**.

## API

### `App` — config + route table + runner (`vmass.mvs`), chain returns `*self`

| method | meaning |
|--------|---------|
| `App::new()` | defaults (0.0.0.0:8080, name "vmass", a 64-entry route table) |
| `.host(str)` `.port(u16)` `.name(str)` | configuration |
| `.get/.post/.put/.delete(path, handler)` | register a route (handler = `func(*Request) -> String`) |
| `.route(method, path, handler)` | register a route with an explicit method |
| `.fallback(handler)` | handler for no match (a default 404 if unset) |
| `.run()` | start serving — loop, match the table, call the handler |

### `Request` — route matching + query (`http.mvs`)

| member / method | meaning |
|-----------------|---------|
| `req.method` `req.path` `req.query` | method · path (without query) · query (after `?`) |
| `.matches(m, p)` | matches both method and path? |
| `.is_get/.is_post/.is_put/.is_delete(p)` | shorthands for matches |

### `Response` — builder (`http.mvs`), chain returns `*self`, finish with `.render()`

| method | meaning |
|--------|---------|
| `Response::ok()` `::created()` `::bad_request()` `::not_found()` `::server_error()` `::new()` | constructors |
| `.status(i32)` | set the status code |
| `.html(s)` `.json(s)` `.text(s)` | set Content-Type + body |
| `.ctype(s)` `.body(s)` | set them manually |
| `.header(line)` | add one extra header line (e.g. `"Cache-Control: no-cache"`) |
| `.redirect(url)` | 302 + `Location: url` |
| `.render()` | build the full HTTP response as a `String` (the terminal step) |

## Build / run

```powershell
mvs.exe vmass/examples/api.mvs -o vmass/api.exe   # or:  make framework
vmass\api.exe                                      # listens on http://0.0.0.0:8080
```

Another terminal:

```powershell
curl http://127.0.0.1:8080/api               # {"framework":"vmass",...}
curl "http://127.0.0.1:8080/echo?msg=hi"     # reads the query string -> "msg=hi"
curl -i http://127.0.0.1:8080/old            # 302 Found + Location: /api
curl -i -X PUT http://127.0.0.1:8080/item    # 201 Created
curl -i http://127.0.0.1:8080/nope           # 404 (custom fallback)
```

## Limitations / where to extend

- Parses only the **first line** (method + path + query) — no headers/body yet; extend `parse_request` (`http.mvs`).
- `query` comes back as a raw string (not split into key/value); no path parameters (`/users/:id`) yet.
- `run()` handles one connection at a time, blocking (single-threaded) — fine for demos / light load.
- Per-connection memory is freed (request strings + recv buffer); the route table is allocated once at startup.
