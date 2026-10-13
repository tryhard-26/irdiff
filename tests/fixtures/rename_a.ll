define i32 @sum(ptr %p, i32 %n) {
entry:
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %loop, label %exit

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %add, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i32 %i
  %v = load i32, ptr %gep, align 4
  %add = add nsw i32 %acc, %v
  %inc = add nuw nsw i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = phi i32 [ 0, %entry ], [ %add, %loop ]
  ret i32 %r
}
