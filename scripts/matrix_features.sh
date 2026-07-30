#!/bin/sh
# Feature-axis matrix: the axes scripts/matrix.sh does not cover.
#
# matrix.sh enumerates TYPES through contexts. This one enumerates FEATURES
# through contexts: trait objects, function pointers, generic functions with
# bounds, structs used as a generic argument, pointers, and control flow that
# crosses match arms. Every program checks itself and returns a code naming the
# check that failed, so there are no goldens to drift.
#
# Usage: sh scripts/matrix_features.sh [elf64|arm64|win64]
set -u
MVS=${MVS:-./mvs}
MODE=${1:-elf64}
case "$MODE" in
    arm64) TARGET="--target arm64"; CC=${CROSS:-aarch64-linux-gnu-gcc}; LINK="-static -lm -lpthread"; RUN=${QEMU:-qemu-aarch64} ;;
    win64) TARGET=""; CC=""; LINK=""; RUN="" ;;
    *)     TARGET="--target elf64"; CC=${CC:-gcc}; LINK="-no-pie -lm -lpthread"; RUN="" ;;
esac
TMP=$(mktemp -d)
fail=0
pass=0

check() {   # $1 = name, the program is already at $TMP/$1.mvs, $2 = expected stdout
    name=$1; want=$2
    f=$TMP/$name.mvs
    base=${f%.mvs}
    if ! "$MVS" "$f" $TARGET > "$TMP/b.txt" 2>&1; then
        echo "FAIL  $name (compile)"; grep -E 'error' "$TMP/b.txt" | head -3; fail=$((fail+1)); return
    fi
    if [ "$MODE" = "win64" ]; then exe="$base.exe"; else
        exe=$TMP/run_$name
        if ! $CC "$base.o" -o "$exe" $LINK 2>"$TMP/l.txt"; then
            echo "FAIL  $name (link)"; head -3 "$TMP/l.txt"; fail=$((fail+1)); return
        fi
    fi
    out=$($RUN "$exe" 2>&1); rc=$?
    rm -f "$base.o" "$base.asm" "$base.s" "$base.obj" "$base.exe"
    if [ $rc -ne 0 ]; then echo "FAIL  $name (self-check $rc)"; fail=$((fail+1)); return; fi
    if [ "$out" != "$want" ]; then echo "FAIL  $name (printed '$out', wanted '$want')"; fail=$((fail+1)); return; fi
    pass=$((pass+1)); echo "ok    $name"
}

# --- 1. trait objects through every context -----------------------------
cat > "$TMP/dyn.mvs" <<'EOF'
import { io } from "std";
import { Vec } from "std/vec";

trait Shape {
    func area(self: *Self) -> i64;
    func name(self: *Self) -> str { return "shape"; }     // default method
}
struct Sq { side: i64; }
struct Rect { w: i64; h: i64; }
impl Shape for Sq {
    func area(self: *Sq) -> i64 { return self.side * self.side; }
    func name(self: *Sq) -> str { return "square"; }
}
impl Shape for Rect {
    func area(self: *Rect) -> i64 { return self.w * self.h; }
}
struct Holder { s: dyn Shape; }
struct Cell { s: dyn Shape; }

func by_param(s: dyn Shape) -> i64 { return s.area(); }
func pick(flag: bool, a: dyn Shape, b: dyn Shape) -> dyn Shape {
    if (flag) { return a; }
    return b;
}
func total(items: ...dyn Shape) -> i64 {
    let sum: i64 = 0;
    let i: usize = 0;
    while (i < items_len) { sum = sum + items[i].area(); i = i + 1; }
    return sum;
}

func main() -> i8 {
    let sq: Sq = Sq { side: 4 };
    let re: Rect = Rect { w: 2, h: 5 };

    let d: dyn Shape = &sq;                       // local
    if (d.area() != 16) { return 1; }
    if (by_param(&re) != 10) { return 2; }        // parameter
    let r: dyn Shape = pick(false, &sq, &re);     // return value
    if (r.area() != 10) { return 3; }

    let h: Holder = Holder { s: &sq };            // struct field
    if (h.s.area() != 16) { return 4; }

    let arr: [dyn Shape; 2] = [&sq, &re];         // array element
    let i: i64 = 1;
    if (arr[i].area() != 10) { return 5; }

    // a trait object cannot be a generic argument (it is a fat pointer): the
    // supported form is a one-field struct, which the diagnostic points at
    let v: Vec<Cell> = Vec<Cell>::new();
    v.push(Cell { s: &sq });
    v.push(Cell { s: &re });
    if (v.get(1).s.area() != 10) { return 6; }
    v.drop();

    if (total(&sq, &re) != 26) { return 7; }      // variadic dyn
    if (!eq(d.name(), "square")) { return 8; }    // overridden method
    if (!eq(pick(false, &sq, &re).name(), "shape")) { return 9; }   // default method
    io.out("dyn ok");
    return 0;
}
extern func strcmp(a: str, b: str) -> i32;
func eq(a: str, b: str) -> bool { return strcmp(a, b) == 0; }
EOF
check dyn "dyn ok"

# --- 2. function pointers through every context --------------------------
cat > "$TMP/funcptr.mvs" <<'EOF'
import { io } from "std";
import { Vec } from "std/vec";

func add(a: i64, b: i64) -> i64 { return a + b; }
func mul(a: i64, b: i64) -> i64 { return a * b; }
func apply(f: func(i64, i64) -> i64, a: i64, b: i64) -> i64 { return f(a, b); }
func chooser(flag: bool) -> func(i64, i64) -> i64 {
    if (flag) { return add; }
    return mul;
}
struct Ops { op: func(i64, i64) -> i64; label: str; }

let g_op: func(i64, i64) -> i64 = mul;

func main() -> i8 {
    let f: func(i64, i64) -> i64 = add;      // local
    if (f(2, 3) != 5) { return 1; }
    if (apply(mul, 2, 3) != 6) { return 2; } // parameter
    if (chooser(true)(4, 5) != 9) { return 3; }   // return value, called directly
    if (g_op(4, 5) != 20) { return 4; }      // global

    let o: Ops = Ops { op: add, label: "plus" };  // struct field
    if (o.op(6, 7) != 13) { return 5; }
    o.op = mul;
    if (o.op(6, 7) != 42) { return 6; }

    let arr: [func(i64, i64) -> i64; 2] = [add, mul];   // array element
    let i: i64 = 1;
    if (arr[i](3, 4) != 12) { return 7; }

    // like trait objects, a function-pointer type is not a generic argument:
    // put it in a struct, which is what the diagnostic tells you to do
    let v: Vec<Ops> = Vec<Ops>::new();
    v.push(Ops { op: add, label: "plus" });
    v.push(Ops { op: mul, label: "times" });
    if (v.get(0).op(8, 9) != 17) { return 8; }
    if (v.get(1).op(8, 9) != 72) { return 9; }
    v.drop();
    io.out("funcptr ok");
    return 0;
}
EOF
check funcptr "funcptr ok"

# --- 3. generic functions with trait bounds over several types ------------
cat > "$TMP/bounds.mvs" <<'EOF'
import { io } from "std";
import { String } from "std/string";

trait Label { func label(self: *Self) -> str; }
struct A { v: i64; }
struct B { v: i64; }
impl Label for A { func label(self: *A) -> str { return "A"; } }
impl Label for B { func label(self: *B) -> str { return "B"; } }
impl Label for i64 { func label(self: *i64) -> str { return "int"; } }

func describe<T: Label>(x: T) -> str { return x.label(); }
func twice<T>(x: T) -> T { return x + x; }          // unbounded, numeric use
func first<T, U>(a: T, b: U) -> T { return a; }     // two parameters

extern func strcmp(a: str, b: str) -> i32;
func eq(a: str, b: str) -> bool { return strcmp(a, b) == 0; }

func main() -> i8 {
    let a: A = A { v: 1 };
    let b: B = B { v: 2 };
    if (!eq(describe(a), "A")) { return 1; }
    if (!eq(describe(b), "B")) { return 2; }
    let n: i64 = 7;
    if (!eq(describe(n), "int")) { return 3; }      // impl on a primitive
    if (twice(21) != 42) { return 4; }
    if (twice(1.5) != 3.0) { return 5; }            // second instantiation
    if (first(9, "x") != 9) { return 6; }
    io.out("bounds ok");
    return 0;
}
EOF
check bounds "bounds ok"

# --- 4. a STRUCT used as the generic argument everywhere ------------------
cat > "$TMP/structarg.mvs" <<'EOF'
import { io } from "std";
import { Vec } from "std/vec";
import { HashMap } from "std/map";
import { Option, Some, None } from "std/option";

struct P { x: i64; y: i64; }
struct Wrap<T> { inner: T; }
enum Slot { Full(P), Empty }

func main() -> i8 {
    let p: P = P { x: 3, y: 4 };
    let q: P = P { x: 5, y: 6 };

    let v: Vec<P> = Vec<P>::new();          // struct as a Vec element
    v.push(p);
    v.push(q);
    let got: P = v.get(1);
    if (got.x != 5 || got.y != 6) { return 1; }
    v.drop();

    let o: Option<P> = Some(p);             // struct as an Option payload
    match (o) {
        Some(r) => { if (r.x != 3) { return 2; } }
        None => { return 3; }
    }

    let w: Wrap<P> = Wrap<P> { inner: q };  // struct inside a generic struct
    if (w.inner.y != 6) { return 4; }

    let s: Slot = Slot::Full(p);            // struct as an enum payload
    match (s) {
        Slot::Full(r) => { if (r.x + r.y != 7) { return 5; } }
        Slot::Empty => { return 6; }
    }

    let m: HashMap<str, P> = HashMap<str, P>::new();   // struct as a map value
    m.insert("a", p);
    match (m.get("a")) {
        Some(r) => { if (r.y != 4) { return 7; } }
        None => { return 8; }
    }
    m.drop();

    let nested: Wrap<Wrap<P>> = Wrap<Wrap<P>> { inner: Wrap<P> { inner: q } };
    if (nested.inner.inner.x != 5) { return 9; }
    io.out("structarg ok");
    return 0;
}
EOF
check structarg "structarg ok"

# --- 5. pointers through every context ------------------------------------
cat > "$TMP/pointers.mvs" <<'EOF'
import { io, mem } from "std";

struct Node { value: i64; next: *Node; }
struct Pair { a: *i64; b: **i64; }

func bump(p: *i64) -> void { *p = *p + 1; }
func pick(a: *i64, b: *i64, first: bool) -> *i64 {
    if (first) { return a; }
    return b;
}

let g_arr: [i64; 3] = [10, 20, 30];

func main() -> i8 {
    let x: i64 = 1;
    let p: *i64 = &x;                 // local pointer
    bump(p);                          // pointer parameter
    if (x != 2) { return 1; }
    let y: i64 = 100;
    if (*pick(&x, &y, false) != 100) { return 2; }   // pointer return

    let pp: **i64 = &p;               // pointer to pointer
    **pp = 42;
    if (x != 42) { return 3; }

    let pr: Pair = Pair { a: &x, b: &p };            // pointers in a struct
    *pr.a = 7;
    if (x != 7) { return 4; }
    if (**pr.b != 7) { return 5; }

    // a linked list on the heap: pointer as a struct field, walked
    let n2: *Node = mem.alloc(16) as *Node;
    let n1: *Node = mem.alloc(16) as *Node;
    n2.value = 2; n2.next = 0;
    n1.value = 1; n1.next = n2;
    let sum: i64 = 0;
    let cur: *Node = n1;
    while (cur != 0) {
        sum = sum + cur.value;
        cur = cur.next;
    }
    if (sum != 3) { return 6; }
    mem.dealloc(n1 as *u8);
    mem.dealloc(n2 as *u8);

    // array decay and pointer arithmetic
    let ap: *i64 = g_arr;
    if (*(ap + 2) != 30) { return 7; }
    if (ap[1] != 20) { return 8; }
    let diff: isize = (ap + 2) as isize - ap as isize;
    if (diff != 16) { return 9; }
    io.out("pointers ok");
    return 0;
}
EOF
check pointers "pointers ok"

# --- 6. control flow crossing match, loops and functions ------------------
cat > "$TMP/flow.mvs" <<'EOF'
import { io } from "std";

enum Step { Go(i64), Skip, Stop }

func classify(i: i64) -> Step {
    if (i == 3) { return Step::Skip; }
    if (i == 7) { return Step::Stop; }
    return Step::Go(i);
}

// every arm returns: the function must typecheck without a trailing return
func code(s: Step) -> i64 {
    match (s) {
        Step::Go(v) => { return v; }
        Step::Skip => { return 0 - 1; }
        Step::Stop => { return 0 - 2; }
    }
}

func main() -> i8 {
    let sum: i64 = 0;
    let skipped: i64 = 0;
    let i: i64 = 0;
    while (i < 20) {
        match (classify(i)) {
            Step::Go(v) => { sum = sum + v; }
            Step::Skip => { skipped = skipped + 1; i = i + 1; continue; }   // continue from an arm
            Step::Stop => { i = 100; break; }                              // break from an arm
        }
        i = i + 1;
    }
    if (sum != 18) { return 1; }          // 0+1+2+4+5+6
    if (skipped != 1) { return 2; }
    if (i != 100) { return 3; }

    // nested loops with a match inside the inner one
    let hits: i64 = 0;
    let a: i64 = 0;
    while (a < 3) {
        let b: i64 = 0;
        while (b < 3) {
            match (classify(b)) {
                Step::Go(v) => { hits = hits + 1; }
                _ => { b = b + 1; continue; }
            }
            b = b + 1;
        }
        a = a + 1;
    }
    if (hits != 9) { return 4; }

    // match as a value in every accepted position
    let v1: i64 = match (classify(5)) { Step::Go(v) => v, _ => 0 };
    let v2: i64 = 0;
    v2 = match (classify(3)) { Step::Skip => 99, _ => 0 };
    if (v1 != 5 || v2 != 99) { return 5; }
    if (code(classify(7)) != 0 - 2) { return 6; }

    // for and do-while with early exits
    let f: i64 = 0;
    for (let k: i64 = 0; k < 10; k++) {
        if (k == 5) { break; }
        if (k % 2 == 0) { continue; }
        f = f + k;
    }
    if (f != 4) { return 7; }             // 1 + 3
    let d: i64 = 0;
    do { d = d + 1; if (d > 3) { break; } } while (true);
    if (d != 4) { return 8; }
    io.out("flow ok");
    return 0;
}
EOF
check flow "flow ok"

# --- 7. pointers as generic arguments -------------------------------------
cat > "$TMP/ptrgen.mvs" <<'EOF'
import { io, mem } from "std";
import { Vec } from "std/vec";
import { HashMap } from "std/map";
import { Option, Some, None } from "std/option";

struct Node { value: i64; }
struct Wrap<T> { inner: T; }
enum Cell { Held(*Node), Nothing }

func main() -> i8 {
    let a: *Node = mem.alloc(8) as *Node;
    let b: *Node = mem.alloc(8) as *Node;
    a.value = 11;
    b.value = 22;

    let v: Vec<*Node> = Vec<*Node>::new();          // pointer as a Vec element
    v.push(a);
    v.push(b);
    if (v.get(1).value != 22) { return 1; }
    v.drop();

    let o: Option<*Node> = Some(a);                 // pointer as an enum payload
    match (o) {
        Some(p) => { if (p.value != 11) { return 2; } }
        None => { return 3; }
    }

    let w: Wrap<*Node> = Wrap<*Node> { inner: b };  // pointer in a generic struct
    if (w.inner.value != 22) { return 4; }

    let m: HashMap<str, *Node> = HashMap<str, *Node>::new();
    m.insert("a", a);
    match (m.get("a")) {
        Some(p) => { if (p.value != 11) { return 5; } }
        None => { return 6; }
    }
    m.drop();

    let vp: Vec<*u8> = Vec<*u8>::new();             // pointer to a primitive
    vp.push(mem.alloc(4));
    let raw: *u8 = vp.get(0);
    *raw = 9;
    if (*vp.get(0) != 9) { return 7; }
    mem.dealloc(raw);
    vp.drop();

    let vv: Vec<Vec<*Node>> = Vec<Vec<*Node>>::new();   // nested, pointer at the leaf
    let inner: Vec<*Node> = Vec<*Node>::new();
    inner.push(b);
    vv.push(inner);
    if (vv.get(0).get(0).value != 22) { return 8; }
    inner.drop();
    vv.drop();

    let c: Cell = Cell::Held(a);
    match (c) {
        Cell::Held(p) => { if (p.value != 11) { return 9; } }
        Cell::Nothing => { return 10; }
    }
    mem.dealloc(a as *u8);
    mem.dealloc(b as *u8);
    io.out("ptrgen ok");
    return 0;
}
EOF
check ptrgen "ptrgen ok"

# --- 8. inferring a generic parameter THROUGH a composite parameter --------
cat > "$TMP/infer.mvs" <<'EOF'
import { io } from "std";
import { Vec } from "std/vec";
import { HashMap } from "std/map";
import { Option, Some, None } from "std/option";

struct Node { value: i64; }
struct Wrap<T> { inner: T; }

func head<T>(v: Vec<T>) -> T { return v.get(0); }               // T from Vec<T>
func size<K, V>(m: HashMap<K, V>) -> usize { return m.len(); }  // two parameters
func unwrap_or<T>(o: Option<T>, dflt: T) -> T {                 // T from Option<T>
    match (o) {
        Some(v) => { return v; }
        None => { return dflt; }
    }
}
func inner_of<T>(w: Wrap<T>) -> T { return w.inner; }
func first_of<T>(v: Vec<Wrap<T>>) -> T { return v.get(0).inner; }   // two levels deep
func deref<T>(p: *T) -> T { return *p; }                            // T behind a pointer

extern func strcmp(a: str, b: str) -> i32;
func eq(a: str, b: str) -> bool { return strcmp(a, b) == 0; }

func main() -> i8 {
    let vi: Vec<i64> = Vec<i64>::new();
    vi.push(7);
    if (head(vi) != 7) { return 1; }

    let vs: Vec<str> = Vec<str>::new();         // the case that used to become i64
    vs.push("hi");
    if (!eq(head(vs), "hi")) { return 2; }

    let vf: Vec<f64> = Vec<f64>::new();
    vf.push(2.5);
    if (head(vf) != 2.5) { return 3; }

    let vn: Vec<Node> = Vec<Node>::new();       // a struct element
    vn.push(Node { value: 5 });
    if (head(vn).value != 5) { return 4; }

    let m: HashMap<str, i64> = HashMap<str, i64>::new();
    m.insert("k", 1);
    if (size(m) != 1) { return 5; }
    m.drop();

    if (!eq(unwrap_or(Some("a"), "b"), "a")) { return 6; }
    if (!eq(unwrap_or(None(), "b"), "b")) { return 7; }     // T comes from the other argument
    if (unwrap_or(Some(3.5), 0.0) != 3.5) { return 8; }

    if (!eq(inner_of(Wrap<str> { inner: "in" }), "in")) { return 9; }

    let vw: Vec<Wrap<str>> = Vec<Wrap<str>>::new();
    vw.push(Wrap<str> { inner: "deep" });
    if (!eq(first_of(vw), "deep")) { return 10; }
    vw.drop();

    let x: f32 = 1.25;
    if (deref(&x) != 1.25) { return 11; }
    vi.drop(); vs.drop(); vf.drop(); vn.drop();
    io.out("infer ok");
    return 0;
}
EOF
check infer "infer ok"

echo
if [ "$fail" -gt 0 ]; then echo "FEATURE MATRIX FAILED: $fail case(s), $pass passed"; rm -rf "$TMP"; exit 1; fi
echo "FEATURE MATRIX OK: $pass feature area(s) verified"
rm -rf "$TMP"
