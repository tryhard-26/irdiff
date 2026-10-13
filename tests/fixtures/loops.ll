define i32 @count() {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add nuw nsw i32 %i, 1
  %c = icmp ult i32 %inc, 100
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %inc
}

define void @copy(ptr %dst, ptr %src, i32 %n) {
entry:
  %g = icmp sgt i32 %n, 0
  br i1 %g, label %body, label %exit

body:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]
  %s = getelementptr inbounds i32, ptr %src, i32 %i
  %v = load i32, ptr %s, align 4
  %d = getelementptr inbounds i32, ptr %dst, i32 %i
  store i32 %v, ptr %d, align 4
  %inc = add nuw nsw i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %body, label %exit

exit:
  ret void
}
