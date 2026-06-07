# vmass — เฟรมเวิร์ก HTTP เขียนด้วย MVS

มินิเฟรมเวิร์กเว็บที่ใช้งานได้จริงระดับหนึ่ง — เป็น **ตัวสาธิต/ทดสอบ** ความสามารถของคอมไพเลอร์ MVS
(โดยเฉพาะ **function pointer** + โมดูล `net`) **ไม่ใช่ส่วนหนึ่งของตัวคอมไพเลอร์**

ออกแบบด้วย **builder pattern** + **function-pointer routing** (`app.get(path, handler)`) จัดเป็นโครงสร้าง modular

## โครงสร้าง
```
vmass/
├── core.mvs        ยูทิลิตีระดับล่าง — str_len / str_eq / substr / alloc / dealloc   (เทียบ zephyr-core)
├── http.mvs        ชั้น HTTP — Request (matcher + query), Response (builder), parse   (เทียบ zephyr-http + router)
├── vmass.mvs       entry library — App (builder + route table + runner)   (เทียบ zephyr-server + umbrella)
└── examples/
    ├── hello.mvs   เซิร์ฟเวอร์เส้นทางเดียว
    └── api.mvs     หลายเส้นทาง + query + redirect + PUT/DELETE + custom 404
```

> **นโยบายโค้ด:** เขียนด้วย **MVS ล้วน** ให้มากที่สุด — `str_len`/`str_eq` เขียนด้วย MVS ·
> **extern C เฉพาะที่ MVS ทำเองไม่ได้** คือการจัดการฮีป (`malloc`/`free`/`memcpy` ใน `core.mvs` เท่านั้น)

## ใช้งาน — function-pointer routing
```rust
import { App } from "../vmass.mvs";   // โหลดทั้งเฟรมเวิร์ก → Request/Response เป็น struct global ใช้ได้เลย
import { String } from "std/string";

// handler คือ function pointer: func(*Request) -> String (คืนค่าจาก Response builder ด้วย .render())
func home(req: *Request) -> String { return Response::ok().html("<h1>Home</h1>").render(); }
func save(req: *Request) -> String { return Response::created().text("saved").render(); }
func my404(req: *Request) -> String { return Response::not_found().json("{\"err\":\"nope\"}").render(); }

func main() -> i8 {
    App::new()
        .host("0.0.0.0").port(8080).name("myapp")
        .get("/", home)
        .post("/save", save)
        .fallback(my404)        // (ไม่บังคับ) handler 404 เอง — เก็บเป็น function pointer
        .run();
    return 0;
}
```
> หัวใจคือ handler ถูกเก็บใน **ตารางเส้นทาง** (`struct Route { method; path; handler: func(*Request)->String }`)
> บนฮีป แล้ว `dispatch()` จับคู่ method+path และ **เรียก handler แบบ indirect ผ่าน function pointer**

## API

### `App` — config + route table + runner (`vmass.mvs`) · chain คืน `*self`
| method | ความหมาย |
|--------|----------|
| `App::new()` | ค่าเริ่มต้น (0.0.0.0:8080, name "vmass", ตาราง 64 เส้นทาง) |
| `.host(str)` `.port(u16)` `.name(str)` | ตั้งค่า |
| `.get/.post/.put/.delete(path, handler)` | ลงทะเบียนเส้นทาง (handler = `func(*Request) -> String`) |
| `.route(method, path, handler)` | ลงทะเบียนเส้นทางแบบระบุ method เอง |
| `.fallback(handler)` | ตั้ง handler เมื่อไม่ตรงเส้นทางใด (404 เริ่มต้นถ้าไม่ตั้ง) |
| `.run()` | เริ่มรับคำขอ — วนจับคู่ตารางแล้วเรียก handler |

### `Request` — จับคู่เส้นทาง + query (`http.mvs`)
| member / method | ความหมาย |
|-----------------|----------|
| `req.method` `req.path` `req.query` | method · path (ไม่รวม query) · query (ส่วนหลัง `?`) |
| `.matches(m, p)` | ตรงทั้ง method และ path ไหม |
| `.is_get/.is_post/.is_put/.is_delete(p)` | ทางลัดของ matches |

### `Response` — builder (`http.mvs`) · chain คืน `*self`, ปิดด้วย `.render()`
| method | ความหมาย |
|--------|----------|
| `Response::ok()` `::created()` `::bad_request()` `::not_found()` `::server_error()` `::new()` | constructor |
| `.status(i32)` | กำหนด status code |
| `.html(s)` `.json(s)` `.text(s)` | ตั้ง Content-Type + body |
| `.ctype(s)` `.body(s)` | ตั้งเอง |
| `.header(line)` | ใส่ header เสริมหนึ่งบรรทัด (เช่น `"Cache-Control: no-cache"`) |
| `.redirect(url)` | 302 + `Location: url` |
| `.render()` | สร้าง HTTP response เต็มรูปเป็น `String` (terminal step) |

## build / run
```powershell
mvs.exe vmass/examples/api.mvs -o vmass/api.exe   # หรือ:  make framework
vmass\api.exe                                      # ฟังที่ http://0.0.0.0:8080
```
อีกเทอร์มินัล:
```powershell
curl http://127.0.0.1:8080/api               # {"framework":"vmass",...}
curl "http://127.0.0.1:8080/echo?msg=hi"     # อ่าน query string -> "msg=hi"
curl -i http://127.0.0.1:8080/old            # 302 Found + Location: /api
curl -i -X PUT http://127.0.0.1:8080/item    # 201 Created
curl -i http://127.0.0.1:8080/nope           # 404 (custom fallback)
```

## ข้อจำกัด / แนวทางต่อยอด
- parse เฉพาะ **บรรทัดแรก** (method + path + query) — ยังไม่ parse headers/body — ขยายใน `parse_request` (`http.mvs`) ได้
- `query` ให้มาเป็นสตริงดิบ (ยังไม่แยกเป็น key/value) · ยังไม่มี path parameter (`/users/:id`)
- `run()` รับทีละ connection แบบ blocking (single-thread) — เหมาะกับตัวอย่าง/งานเบา
- คืนหน่วยความจำต่อ connection แล้ว (request strings + บัฟเฟอร์ recv) — ตารางเส้นทางจองครั้งเดียวตอนเริ่ม
