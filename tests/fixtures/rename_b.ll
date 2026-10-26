define i32 @sum(ptr %ptr, i32 %count) {
0:
  %1 = icmp sgt i32 %count, 0
  br i1 %1, label %body, label %out

out:
  %res = phi i32 [ %s, %body ], [ 0, %0 ]
  ret i32 %res

body:
  %idx = phi i32 [ %next, %body ], [ 0, %0 ]
  %total = phi i32 [ 0, %0 ], [ %s, %body ]
  %addr = getelementptr inbounds i32, ptr %ptr, i32 %idx
  %x = load i32, ptr %addr, align 4
  %s = add nsw i32 %x, %total
  %next = add nuw nsw i32 %idx, 1
  %stop = icmp eq i32 %next, %count
  br i1 %stop, label %out, label %body
}
