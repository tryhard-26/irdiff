target triple = "aarch64-unknown-linux-gnu"

define i32 @inc(i32 %x) {
entry:
  %r = add i32 %x, 1
  ret i32 %r
}

define i32 @same(i32 %x) {
entry:
  ret i32 %x
}
