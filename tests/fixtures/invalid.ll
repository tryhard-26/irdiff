define i32 @f(i32 %x) {
entry:
  %r = add i32 %y, 1
  %y = add i32 %x, 1
  ret i32 %r
}
