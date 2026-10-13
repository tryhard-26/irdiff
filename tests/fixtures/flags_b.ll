define i32 @inc(i32 %x) {
entry:
  %r = add nsw i32 %x, 1
  ret i32 %r
}

define i32 @same(i32 %x) {
entry:
  ret i32 %x
}
