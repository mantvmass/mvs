#!/bin/sh
# Module axis: every import form crossed with every kind of exported thing, and
# the conditional-compilation filter on top of it.
#
# The module loader is the one pass that rewrites what the rest of the compiler
# sees, so a mistake there shows up as a missing symbol or a duplicate rather
# than as wrong code. This enumerates the combinations instead of trusting that
# the examples happen to cover them.
#
# Usage: sh scripts/matrix_modules.sh [elf64|arm64|win64]
set -u
MVS=${MVS:-./mvs}
MODE=${1:-elf64}
case "$MODE" in
    arm64) TARGET="--target arm64"; CC=${CROSS:-aarch64-linux-gnu-gcc}; LINK="-static -lm"; RUN=${QEMU:-qemu-aarch64} ;;
    win64) TARGET=""; CC=""; LINK=""; RUN="" ;;
    *)     TARGET="--target elf64"; CC=${CC:-gcc}; LINK="-no-pie -lm"; RUN="" ;;
esac
TMP=$(mktemp -d)
fail=0
pass=0

run() {     # $1 = name (file $TMP/$1.mvs), $2 = expected stdout
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
    if [ $rc -ne 0 ]; then echo "FAIL  $name (exit $rc)"; fail=$((fail+1)); return; fi
    if [ "$out" != "$want" ]; then echo "FAIL  $name (printed '$out', wanted '$want')"; fail=$((fail+1)); return; fi
    pass=$((pass+1)); echo "ok    $name"
}

expect_error() {   # $1 = name, $2 = text the diagnostic must contain
    name=$1; want=$2
    if "$MVS" "$TMP/$name.mvs" $TARGET > "$TMP/e.txt" 2>&1; then
        echo "FAIL  $name (compiled, but should have been rejected)"; fail=$((fail+1)); return
    fi
    if ! grep -q "$want" "$TMP/e.txt"; then
        echo "FAIL  $name (wrong diagnostic; wanted '$want')"; head -3 "$TMP/e.txt"; fail=$((fail+1)); return
    fi
    pass=$((pass+1)); echo "ok    $name (rejected as it should be)"
}

# a local module exporting one of every kind of thing
cat > "$TMP/lib.mvs" <<'EOF'
struct Point { x: i64; y: i64; }
impl Point {
    func new(x: i64, y: i64) -> Point { return Point { x: x, y: y }; }
    func sum(self: *Point) -> i64 { return self.x + self.y; }
}
trait Named { func name(self: *Self) -> str; }
impl Named for Point { func name(self: *Point) -> str { return "point"; } }
enum Color { Red, Green, Blue(i64) }
func twice(x: i64) -> i64 { return x * 2; }
func pick<T>(a: T, b: T, first: bool) -> T { if (first) { return a; } return b; }
let LIB_BASE: i64 = 100;
EOF

# --- form B: symbol import of a local module, one symbol of each kind --------
cat > "$TMP/form_b.mvs" <<'EOF'
import { io } from "std";
import { Point, Named, Color, twice, pick, LIB_BASE } from "./lib.mvs";

func main() -> i8 {
    let p: Point = Point::new(3, 4);          // struct + associated function
    if (p.sum() != 7) { return 1; }           // method
    if (twice(21) != 42) { return 2; }        // free function
    if (pick(5, 9, true) != 5) { return 3; }  // generic free function
    if (LIB_BASE != 100) { return 4; }        // global
    let d: dyn Named = &p;                    // trait, used as a trait object
    if (d.name() != "point") { return 5; }
    let c: Color = Color::Blue(7);            // enum from another module
    match (c) {
        Color::Blue(v) => { if (v != 7) { return 6; } }
        Color::Red => { return 7; }
        Color::Green => { return 8; }
    }
    io.out("form_b ok");
    return 0;
}
EOF
run form_b "form_b ok"

# --- form C: whole-module alias ---------------------------------------------
cat > "$TMP/form_c.mvs" <<'EOF'
import { io } from "std";
import lib from "./lib.mvs";
import { Point } from "./lib.mvs";       // structs are global either way

func main() -> i8 {
    if (lib.twice(4) != 8) { return 1; }         // free function under the alias
    if (lib.pick(1, 2, false) != 2) { return 2; }// generic under the alias
    let p: Point = Point::new(1, 2);
    if (p.sum() != 3) { return 3; }
    io.out("form_c ok");
    return 0;
}
EOF
run form_c "form_c ok"

# --- form A: package namespace, several submodules ---------------------------
cat > "$TMP/form_a.mvs" <<'EOF'
import { io, math, mem } from "std";

func main() -> i8 {
    if (math.abs(0 - 5) != 5) { return 1; }
    if (math.max(2.5, 1.5) != 2.5) { return 2; }   // overload inside a namespace
    let p: *u8 = mem.alloc(4);
    mem.set(p, 7, 4);
    if (*p != 7) { return 3; }
    mem.dealloc(p);
    io.out("form_a ok");
    return 0;
}
EOF
run form_a "form_a ok"

# --- forms mixed in one file, plus a std module reached three ways ------------
cat > "$TMP/mixed.mvs" <<'EOF'
import { io } from "std";                 // A
import { String } from "std/string";      // B
import { Vec } from "std/vec";            // B
import { cstr } from "core";              // B
import lib from "./lib.mvs";              // C

func main() -> i8 {
    let s: String = String::from("ab");
    s.push_str("cd");
    if (s.len() != 4) { return 1; }
    if (!cstr.eq(s.as_str(), "abcd")) { return 2; }   // str compares by address
    s.drop();
    let v: Vec<i64> = Vec<i64>::new();
    v.push(lib.twice(3));
    if (v.get(0) != 6) { return 3; }
    v.drop();
    io.out("mixed ok");
    return 0;
}
EOF
run mixed "mixed ok"

# --- transitive imports: a module that imports another module ----------------
cat > "$TMP/mid.mvs" <<'EOF'
import { twice } from "./lib.mvs";
func quad(x: i64) -> i64 { return twice(twice(x)); }
EOF
cat > "$TMP/transitive.mvs" <<'EOF'
import { io } from "std";
import { quad } from "./mid.mvs";
import { twice } from "./lib.mvs";        // the same module again, one level up

func main() -> i8 {
    if (quad(3) != 12) { return 1; }
    if (twice(3) != 6) { return 2; }      // must not be duplicated by the two paths
    io.out("transitive ok");
    return 0;
}
EOF
run transitive "transitive ok"

# --- @compile: the filter must leave exactly one definition standing ---------
cat > "$TMP/cond.mvs" <<'EOF'
import { io } from "std";

@compile(target_os = "windows")
func plat() -> str { return "windows"; }
@compile(target_os = "linux")
func plat() -> str { return "linux"; }

@compile(target_arch = "x86_64")
func arch_name() -> str { return "x86_64"; }
@compile(target_arch = "aarch64")
func arch_name() -> str { return "aarch64"; }

@compile(target_os = "linux", target_arch = "x86_64")
func both() -> i64 { return 1; }
@compile(target_os = "linux", target_arch = "aarch64")
func both() -> i64 { return 2; }
@compile(target_os = "windows")
func both() -> i64 { return 3; }

func main() -> i8 {
    io.out("{} {} {}", plat(), arch_name(), both());
    return 0;
}
EOF
case "$MODE" in
    arm64) run cond "linux aarch64 2" ;;
    win64) run cond "windows x86_64 3" ;;
    *)     run cond "linux x86_64 1" ;;
esac

# --- @compile inside an imported module (filtered by the loader, not codegen) -
cat > "$TMP/platlib.mvs" <<'EOF'
@compile(target_os = "windows")
func tag() -> str { return "win"; }
@compile(target_os = "linux")
func tag() -> str { return "nix"; }
EOF
cat > "$TMP/cond_import.mvs" <<'EOF'
import { io } from "std";
import { tag } from "./platlib.mvs";
import plat from "./platlib.mvs";

func main() -> i8 {
    io.out("{} {}", tag(), plat.tag());
    return 0;
}
EOF
case "$MODE" in
    win64) run cond_import "win win" ;;
    *)     run cond_import "nix nix" ;;
esac

# --- the import errors the guide promises ------------------------------------
cat > "$TMP/err_missing.mvs" <<'EOF'
import { nosuchthing } from "./lib.mvs";
func main() -> i8 { return 0; }
EOF
expect_error err_missing "no exported symbol"

cat > "$TMP/err_cycle_a.mvs" <<'EOF'
import { b_fn } from "./err_cycle_b.mvs";
func a_fn() -> i64 { return b_fn(); }
func main() -> i8 { return a_fn() as i8; }
EOF
cat > "$TMP/err_cycle_b.mvs" <<'EOF'
import { a_fn } from "./err_cycle_a.mvs";
func b_fn() -> i64 { return 0; }
EOF
expect_error err_cycle_a "circular import"

cat > "$TMP/err_two_ns.mvs" <<'EOF'
import one from "./lib.mvs";
import two from "./lib.mvs";
func main() -> i8 { return one.twice(1) as i8; }
EOF
expect_error err_two_ns "namespace"

echo
if [ "$fail" -gt 0 ]; then echo "MODULE MATRIX FAILED: $fail case(s), $pass passed"; rm -rf "$TMP"; exit 1; fi
echo "MODULE MATRIX OK: $pass module/import combination(s) verified"
rm -rf "$TMP"
