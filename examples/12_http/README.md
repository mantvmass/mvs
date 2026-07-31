# A web application on std/http

`std/http` is the HTTP framework built into the standard library, written in
MVS on top of `std/net`. The shape is axum's: a `Router` built from method
routers, handlers that take the request and return a `Response`, and a listener
the server loop runs on.

```txt
import { Router, Listener, Request, Response } from "std/http";
import { get, post, Text, Html, Json, Status, serve } from "std/http";

func home(_req: *Request) -> Response {
    return Html("<h1>Home</h1>");
}

func show(req: *Request) -> Response {
    match (req.param("id")) {
        Some(id) => { return Text(id); }
        None => { return Status(400); }
    }
}

func create(req: *Request) -> Response {
    return Json(req.body).status(201).header("Location", "/users/new");
}

func main() -> i8 {
    let app: Router = Router::new()
        .route("/", get(home))
        .route("/users/:id", get(show).post(create))
        .fallback(my404);

    let l: Listener = Listener::bind("0.0.0.0:8080");
    serve(&l, &app);
    l.close();
    app.drop();
    return 0;
}
```

## Run this example

```sh
mvs examples/12_http/main.mvs
./examples/12_http/main
```

It runs the client and the server in one process, which is what makes the
output identical on every run and on every target: each request is sent first,
then `serve_once` accepts it. Output is byte-identical on x86-64 Windows,
x86-64 Linux and AArch64 Linux, and the program is clean under
AddressSanitizer with leak detection ON.

To serve a real port instead, replace the driver with `serve(&l, &app)` and use
curl:

```sh
curl http://127.0.0.1:8080/
curl "http://127.0.0.1:8080/search?q=hello+world"
curl -i -X POST -d 'hi' http://127.0.0.1:8080/echo
curl -i http://127.0.0.1:8080/old        # 302 with a Location header
curl -i http://127.0.0.1:8080/nope       # the custom 404
```

## What the framework gives you

| Piece | What it does |
|-------|--------------|
| `Router::new()` | an empty router with the built-in 404 |
| `.route(path, method_router)` | one path with everything it answers |
| `get/post/put/delete/patch/head/options/any(handler)` | the method routers, chained for one path: `get(show).post(create)` |
| `method_router(method, h)` and `.on(method, h)` | any other method name |
| `.fallback(handler)` `.log(bool)` | what answers when nothing matched, and the access log |
| `.handle(req)` | dispatch with no sockets, which is how the tests work |
| `Listener::bind("ip:port")` `serve` `serve_n` `serve_once` `.close()` | the socket side |
| `Request` | `method` `path` `query_string` `version` `body`, plus `.param()` `.query()` `.header()` `.content_length()` `.is_method()` |
| `Response` | `Text` `Html` `Json` `Status` `Redirect`, then `.status()` `.header()` `.content_type()` `.write()` and `.render()` |
| client | `send_request` `read_response` `fetch` `response_status` `response_body` `response_header` |

Path patterns: `/users/:id` captures a segment into `req.param("id")`, and a
segment that is just `*` (`/assets/*`) matches the rest of the path.

## What it does not do

No TLS, no chunked transfer encoding, no keep-alive (every response closes the
connection), no multipart bodies, and one connection at a time. A request over
64 KB is refused rather than growing a buffer forever. Those are the honest
limits of a framework that fits in one std module; `std/thread` is there if you
want to hand each connection to a worker.
