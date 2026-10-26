define i32 @f(ptr noalias %a, ptr noalias %b) {
entry:
  store i32 1, ptr %a, align 4
  %v = load i32, ptr %b, align 4
  ret i32 %v
}
