; ModuleID = 'kernels.c'
source_filename = "kernels.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nofree norecurse nosync nounwind memory(argmem: read) uwtable
define dso_local i32 @sum_squares(ptr nocapture noundef readonly %a, i32 noundef %n) local_unnamed_addr #0 !dbg !7 {
entry:
  %cmp4 = icmp sgt i32 %n, 0, !dbg !10
  br i1 %cmp4, label %for.body.preheader, label %for.cond.cleanup, !dbg !11

for.body.preheader:                               ; preds = %entry
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !10
  %min.iters.check = icmp ult i32 %n, 8, !dbg !11
  br i1 %min.iters.check, label %for.body.preheader10, label %vector.ph, !dbg !11

for.body.preheader10:                             ; preds = %middle.block, %for.body.preheader
  %indvars.iv.ph = phi i64 [ 0, %for.body.preheader ], [ %n.vec, %middle.block ]
  %s.05.ph = phi i32 [ 0, %for.body.preheader ], [ %7, %middle.block ]
  br label %for.body, !dbg !11

vector.ph:                                        ; preds = %for.body.preheader
  %n.vec = and i64 %wide.trip.count, 2147483640, !dbg !11
  br label %vector.body, !dbg !11

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ], !dbg !12
  %vec.phi = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %4, %vector.body ]
  %vec.phi8 = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %5, %vector.body ]
  %0 = getelementptr inbounds i32, ptr %a, i64 %index, !dbg !13
  %1 = getelementptr inbounds i8, ptr %0, i64 16, !dbg !13
  %wide.load = load <4 x i32>, ptr %0, align 4, !dbg !13, !tbaa !14
  %wide.load9 = load <4 x i32>, ptr %1, align 4, !dbg !13, !tbaa !14
  %2 = mul nsw <4 x i32> %wide.load, %wide.load, !dbg !18
  %3 = mul nsw <4 x i32> %wide.load9, %wide.load9, !dbg !18
  %4 = add <4 x i32> %2, %vec.phi, !dbg !21
  %5 = add <4 x i32> %3, %vec.phi8, !dbg !21
  %index.next = add nuw i64 %index, 8, !dbg !12
  %6 = icmp eq i64 %index.next, %n.vec, !dbg !12
  br i1 %6, label %middle.block, label %vector.body, !dbg !12, !llvm.loop !22

middle.block:                                     ; preds = %vector.body
  %bin.rdx = add <4 x i32> %5, %4, !dbg !11
  %7 = tail call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %bin.rdx), !dbg !11
  %cmp.n = icmp eq i64 %n.vec, %wide.trip.count, !dbg !11
  br i1 %cmp.n, label %for.cond.cleanup, label %for.body.preheader10, !dbg !11

for.cond.cleanup:                                 ; preds = %for.body, %middle.block, %entry
  %s.0.lcssa = phi i32 [ 0, %entry ], [ %7, %middle.block ], [ %add, %for.body ], !dbg !27
  ret i32 %s.0.lcssa, !dbg !28

for.body:                                         ; preds = %for.body.preheader10, %for.body
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body ], [ %indvars.iv.ph, %for.body.preheader10 ]
  %s.05 = phi i32 [ %add, %for.body ], [ %s.05.ph, %for.body.preheader10 ]
  %arrayidx = getelementptr inbounds i32, ptr %a, i64 %indvars.iv, !dbg !13
  %8 = load i32, ptr %arrayidx, align 4, !dbg !13, !tbaa !14
  %mul.i = mul nsw i32 %8, %8, !dbg !18
  %add = add nuw nsw i32 %mul.i, %s.05, !dbg !21
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !12
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !10
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !11, !llvm.loop !29
}

; Function Attrs: nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @scale(ptr nocapture noundef writeonly %dst, ptr nocapture noundef readonly %src, ptr nocapture noundef readonly %k, i32 noundef %n) local_unnamed_addr #1 !dbg !30 {
entry:
  %cmp6 = icmp sgt i32 %n, 0, !dbg !31
  br i1 %cmp6, label %for.body.preheader, label %for.cond.cleanup, !dbg !32

for.body.preheader:                               ; preds = %entry
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !31
  %min.iters.check = icmp ult i32 %n, 12, !dbg !32
  br i1 %min.iters.check, label %for.body.preheader17, label %vector.memcheck, !dbg !32

for.body.preheader17:                             ; preds = %middle.block, %vector.memcheck, %for.body.preheader
  %indvars.iv.ph = phi i64 [ 0, %vector.memcheck ], [ 0, %for.body.preheader ], [ %n.vec, %middle.block ]
  %xtraiter = and i64 %wide.trip.count, 3, !dbg !32
  %lcmp.mod.not = icmp eq i64 %xtraiter, 0, !dbg !32
  br i1 %lcmp.mod.not, label %for.body.prol.loopexit, label %for.body.prol, !dbg !32

for.body.prol:                                    ; preds = %for.body.preheader17, %for.body.prol
  %indvars.iv.prol = phi i64 [ %indvars.iv.next.prol, %for.body.prol ], [ %indvars.iv.ph, %for.body.preheader17 ]
  %prol.iter = phi i64 [ %prol.iter.next, %for.body.prol ], [ 0, %for.body.preheader17 ]
  %arrayidx.prol = getelementptr inbounds float, ptr %src, i64 %indvars.iv.prol, !dbg !33
  %0 = load float, ptr %arrayidx.prol, align 4, !dbg !33, !tbaa !34
  %1 = load float, ptr %k, align 4, !dbg !36, !tbaa !34
  %mul.prol = fmul float %0, %1, !dbg !37
  %arrayidx2.prol = getelementptr inbounds float, ptr %dst, i64 %indvars.iv.prol, !dbg !38
  store float %mul.prol, ptr %arrayidx2.prol, align 4, !dbg !39, !tbaa !34
  %indvars.iv.next.prol = add nuw nsw i64 %indvars.iv.prol, 1, !dbg !40
  %prol.iter.next = add i64 %prol.iter, 1, !dbg !32
  %prol.iter.cmp.not = icmp eq i64 %prol.iter.next, %xtraiter, !dbg !32
  br i1 %prol.iter.cmp.not, label %for.body.prol.loopexit, label %for.body.prol, !dbg !32, !llvm.loop !41

for.body.prol.loopexit:                           ; preds = %for.body.prol, %for.body.preheader17
  %indvars.iv.unr = phi i64 [ %indvars.iv.ph, %for.body.preheader17 ], [ %indvars.iv.next.prol, %for.body.prol ]
  %2 = sub nsw i64 %indvars.iv.ph, %wide.trip.count, !dbg !32
  %3 = icmp ugt i64 %2, -4, !dbg !32
  br i1 %3, label %for.cond.cleanup, label %for.body, !dbg !32

vector.memcheck:                                  ; preds = %for.body.preheader
  %4 = shl nuw nsw i64 %wide.trip.count, 2, !dbg !32
  %scevgep = getelementptr i8, ptr %dst, i64 %4, !dbg !32
  %scevgep9 = getelementptr i8, ptr %src, i64 %4, !dbg !32
  %scevgep10 = getelementptr i8, ptr %k, i64 4, !dbg !32
  %bound0 = icmp ugt ptr %scevgep9, %dst, !dbg !32
  %bound1 = icmp ugt ptr %scevgep, %src, !dbg !32
  %found.conflict = and i1 %bound0, %bound1, !dbg !32
  %bound011 = icmp ugt ptr %scevgep10, %dst, !dbg !32
  %bound112 = icmp ugt ptr %scevgep, %k, !dbg !32
  %found.conflict13 = and i1 %bound011, %bound112, !dbg !32
  %conflict.rdx = or i1 %found.conflict, %found.conflict13, !dbg !32
  br i1 %conflict.rdx, label %for.body.preheader17, label %vector.ph, !dbg !32

vector.ph:                                        ; preds = %vector.memcheck
  %n.vec = and i64 %wide.trip.count, 2147483640, !dbg !32
  %5 = load float, ptr %k, align 4, !tbaa !34, !alias.scope !43
  %broadcast.splatinsert15 = insertelement <4 x float> poison, float %5, i64 0
  %broadcast.splat16 = shufflevector <4 x float> %broadcast.splatinsert15, <4 x float> poison, <4 x i32> zeroinitializer
  br label %vector.body, !dbg !32

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ], !dbg !40
  %6 = getelementptr inbounds float, ptr %src, i64 %index, !dbg !33
  %7 = getelementptr inbounds i8, ptr %6, i64 16, !dbg !33
  %wide.load = load <4 x float>, ptr %6, align 4, !dbg !33, !tbaa !34, !alias.scope !46
  %wide.load14 = load <4 x float>, ptr %7, align 4, !dbg !33, !tbaa !34, !alias.scope !46
  %8 = fmul <4 x float> %wide.load, %broadcast.splat16, !dbg !32
  %9 = fmul <4 x float> %wide.load14, %broadcast.splat16, !dbg !32
  %10 = getelementptr inbounds float, ptr %dst, i64 %index, !dbg !38
  %11 = getelementptr inbounds i8, ptr %10, i64 16, !dbg !39
  store <4 x float> %8, ptr %10, align 4, !dbg !39, !tbaa !34, !alias.scope !48, !noalias !50
  store <4 x float> %9, ptr %11, align 4, !dbg !39, !tbaa !34, !alias.scope !48, !noalias !50
  %index.next = add nuw i64 %index, 8, !dbg !40
  %12 = icmp eq i64 %index.next, %n.vec, !dbg !40
  br i1 %12, label %middle.block, label %vector.body, !dbg !40, !llvm.loop !51

middle.block:                                     ; preds = %vector.body
  %cmp.n = icmp eq i64 %n.vec, %wide.trip.count, !dbg !32
  br i1 %cmp.n, label %for.cond.cleanup, label %for.body.preheader17, !dbg !32

for.cond.cleanup:                                 ; preds = %for.body.prol.loopexit, %for.body, %middle.block, %entry
  ret void, !dbg !53

for.body:                                         ; preds = %for.body.prol.loopexit, %for.body
  %indvars.iv = phi i64 [ %indvars.iv.next.3, %for.body ], [ %indvars.iv.unr, %for.body.prol.loopexit ]
  %arrayidx = getelementptr inbounds float, ptr %src, i64 %indvars.iv, !dbg !33
  %13 = load float, ptr %arrayidx, align 4, !dbg !33, !tbaa !34
  %14 = load float, ptr %k, align 4, !dbg !36, !tbaa !34
  %mul = fmul float %13, %14, !dbg !37
  %arrayidx2 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv, !dbg !38
  store float %mul, ptr %arrayidx2, align 4, !dbg !39, !tbaa !34
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !40
  %arrayidx.1 = getelementptr inbounds float, ptr %src, i64 %indvars.iv.next, !dbg !33
  %15 = load float, ptr %arrayidx.1, align 4, !dbg !33, !tbaa !34
  %16 = load float, ptr %k, align 4, !dbg !36, !tbaa !34
  %mul.1 = fmul float %15, %16, !dbg !37
  %arrayidx2.1 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv.next, !dbg !38
  store float %mul.1, ptr %arrayidx2.1, align 4, !dbg !39, !tbaa !34
  %indvars.iv.next.1 = add nuw nsw i64 %indvars.iv, 2, !dbg !40
  %arrayidx.2 = getelementptr inbounds float, ptr %src, i64 %indvars.iv.next.1, !dbg !33
  %17 = load float, ptr %arrayidx.2, align 4, !dbg !33, !tbaa !34
  %18 = load float, ptr %k, align 4, !dbg !36, !tbaa !34
  %mul.2 = fmul float %17, %18, !dbg !37
  %arrayidx2.2 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv.next.1, !dbg !38
  store float %mul.2, ptr %arrayidx2.2, align 4, !dbg !39, !tbaa !34
  %indvars.iv.next.2 = add nuw nsw i64 %indvars.iv, 3, !dbg !40
  %arrayidx.3 = getelementptr inbounds float, ptr %src, i64 %indvars.iv.next.2, !dbg !33
  %19 = load float, ptr %arrayidx.3, align 4, !dbg !33, !tbaa !34
  %20 = load float, ptr %k, align 4, !dbg !36, !tbaa !34
  %mul.3 = fmul float %19, %20, !dbg !37
  %arrayidx2.3 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv.next.2, !dbg !38
  store float %mul.3, ptr %arrayidx2.3, align 4, !dbg !39, !tbaa !34
  %indvars.iv.next.3 = add nuw nsw i64 %indvars.iv, 4, !dbg !40
  %exitcond.not.3 = icmp eq i64 %indvars.iv.next.3, %wide.trip.count, !dbg !31
  br i1 %exitcond.not.3, label %for.cond.cleanup, label %for.body, !dbg !32, !llvm.loop !54
}

; Function Attrs: nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @scale_restrict(ptr noalias nocapture noundef writeonly %dst, ptr noalias nocapture noundef readonly %src, ptr noalias nocapture noundef readonly %k, i32 noundef %n) local_unnamed_addr #1 !dbg !55 {
entry:
  %cmp6 = icmp sgt i32 %n, 0, !dbg !56
  br i1 %cmp6, label %for.body.lr.ph, label %for.cond.cleanup, !dbg !57

for.body.lr.ph:                                   ; preds = %entry
  %0 = load float, ptr %k, align 4, !tbaa !34
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !56
  %min.iters.check = icmp ult i32 %n, 8, !dbg !57
  br i1 %min.iters.check, label %for.body.preheader, label %vector.ph, !dbg !57

for.body.preheader:                               ; preds = %middle.block, %for.body.lr.ph
  %indvars.iv.ph = phi i64 [ 0, %for.body.lr.ph ], [ %n.vec, %middle.block ]
  br label %for.body, !dbg !57

vector.ph:                                        ; preds = %for.body.lr.ph
  %n.vec = and i64 %wide.trip.count, 2147483640, !dbg !57
  %broadcast.splatinsert = insertelement <4 x float> poison, float %0, i64 0, !dbg !57
  %broadcast.splat = shufflevector <4 x float> %broadcast.splatinsert, <4 x float> poison, <4 x i32> zeroinitializer, !dbg !57
  br label %vector.body, !dbg !57

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ], !dbg !58
  %1 = getelementptr inbounds float, ptr %src, i64 %index, !dbg !59
  %2 = getelementptr inbounds i8, ptr %1, i64 16, !dbg !59
  %wide.load = load <4 x float>, ptr %1, align 4, !dbg !59, !tbaa !34
  %wide.load9 = load <4 x float>, ptr %2, align 4, !dbg !59, !tbaa !34
  %3 = fmul <4 x float> %wide.load, %broadcast.splat, !dbg !60
  %4 = fmul <4 x float> %wide.load9, %broadcast.splat, !dbg !60
  %5 = getelementptr inbounds float, ptr %dst, i64 %index, !dbg !61
  %6 = getelementptr inbounds i8, ptr %5, i64 16, !dbg !62
  store <4 x float> %3, ptr %5, align 4, !dbg !62, !tbaa !34
  store <4 x float> %4, ptr %6, align 4, !dbg !62, !tbaa !34
  %index.next = add nuw i64 %index, 8, !dbg !58
  %7 = icmp eq i64 %index.next, %n.vec, !dbg !58
  br i1 %7, label %middle.block, label %vector.body, !dbg !58, !llvm.loop !63

middle.block:                                     ; preds = %vector.body
  %cmp.n = icmp eq i64 %n.vec, %wide.trip.count, !dbg !57
  br i1 %cmp.n, label %for.cond.cleanup, label %for.body.preheader, !dbg !57

for.cond.cleanup:                                 ; preds = %for.body, %middle.block, %entry
  ret void, !dbg !65

for.body:                                         ; preds = %for.body.preheader, %for.body
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body ], [ %indvars.iv.ph, %for.body.preheader ]
  %arrayidx = getelementptr inbounds float, ptr %src, i64 %indvars.iv, !dbg !59
  %8 = load float, ptr %arrayidx, align 4, !dbg !59, !tbaa !34
  %mul = fmul float %8, %0, !dbg !60
  %arrayidx2 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv, !dbg !61
  store float %mul, ptr %arrayidx2, align 4, !dbg !62, !tbaa !34
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !58
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !56
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !57, !llvm.loop !66
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local range(i32 -1, 2) i32 @classify(i32 noundef %x) local_unnamed_addr #2 !dbg !67 {
entry:
  %cmp1 = icmp ne i32 %x, 0, !dbg !68
  %. = zext i1 %cmp1 to i32, !dbg !68
  %cmp.inv = icmp sgt i32 %x, -1, !dbg !68
  %retval.0 = select i1 %cmp.inv, i32 %., i32 -1, !dbg !68
  ret i32 %retval.0, !dbg !69
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>) #3

attributes #0 = { nofree norecurse nosync nounwind memory(argmem: read) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "kernels.c", directory: "/tmp")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"PIE Level", i32 2}
!6 = !{i32 7, !"uwtable", i32 2}
!7 = distinct !DISubprogram(name: "sum_squares", scope: !1, file: !1, line: 3, type: !8, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!8 = !DISubroutineType(types: !9)
!9 = !{}
!10 = !DILocation(line: 5, column: 21, scope: !7)
!11 = !DILocation(line: 5, column: 3, scope: !7)
!12 = !DILocation(line: 5, column: 26, scope: !7)
!13 = !DILocation(line: 6, column: 17, scope: !7)
!14 = !{!15, !15, i64 0}
!15 = !{!"int", !16, i64 0}
!16 = !{!"omnipotent char", !17, i64 0}
!17 = !{!"Simple C/C++ TBAA"}
!18 = !DILocation(line: 1, column: 37, scope: !19, inlinedAt: !20)
!19 = distinct !DISubprogram(name: "square", scope: !1, file: !1, line: 1, type: !8, scopeLine: 1, flags: DIFlagPrototyped, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!20 = distinct !DILocation(line: 6, column: 10, scope: !7)
!21 = !DILocation(line: 6, column: 7, scope: !7)
!22 = distinct !{!22, !11, !23, !24, !25, !26}
!23 = !DILocation(line: 6, column: 21, scope: !7)
!24 = !{!"llvm.loop.mustprogress"}
!25 = !{!"llvm.loop.isvectorized", i32 1}
!26 = !{!"llvm.loop.unroll.runtime.disable"}
!27 = !DILocation(line: 0, scope: !7)
!28 = !DILocation(line: 7, column: 3, scope: !7)
!29 = distinct !{!29, !11, !23, !24, !26, !25}
!30 = distinct !DISubprogram(name: "scale", scope: !1, file: !1, line: 10, type: !8, scopeLine: 10, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!31 = !DILocation(line: 11, column: 21, scope: !30)
!32 = !DILocation(line: 11, column: 3, scope: !30)
!33 = !DILocation(line: 12, column: 14, scope: !30)
!34 = !{!35, !35, i64 0}
!35 = !{!"float", !16, i64 0}
!36 = !DILocation(line: 12, column: 23, scope: !30)
!37 = !DILocation(line: 12, column: 21, scope: !30)
!38 = !DILocation(line: 12, column: 5, scope: !30)
!39 = !DILocation(line: 12, column: 12, scope: !30)
!40 = !DILocation(line: 11, column: 26, scope: !30)
!41 = distinct !{!41, !42}
!42 = !{!"llvm.loop.unroll.disable"}
!43 = !{!44}
!44 = distinct !{!44, !45}
!45 = distinct !{!45, !"LVerDomain"}
!46 = !{!47}
!47 = distinct !{!47, !45}
!48 = !{!49}
!49 = distinct !{!49, !45}
!50 = !{!47, !44}
!51 = distinct !{!51, !32, !52, !24, !25, !26}
!52 = !DILocation(line: 12, column: 24, scope: !30)
!53 = !DILocation(line: 13, column: 1, scope: !30)
!54 = distinct !{!54, !32, !52, !24, !25}
!55 = distinct !DISubprogram(name: "scale_restrict", scope: !1, file: !1, line: 15, type: !8, scopeLine: 16, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!56 = !DILocation(line: 17, column: 21, scope: !55)
!57 = !DILocation(line: 17, column: 3, scope: !55)
!58 = !DILocation(line: 17, column: 26, scope: !55)
!59 = !DILocation(line: 18, column: 14, scope: !55)
!60 = !DILocation(line: 18, column: 21, scope: !55)
!61 = !DILocation(line: 18, column: 5, scope: !55)
!62 = !DILocation(line: 18, column: 12, scope: !55)
!63 = distinct !{!63, !57, !64, !24, !25, !26}
!64 = !DILocation(line: 18, column: 24, scope: !55)
!65 = !DILocation(line: 19, column: 1, scope: !55)
!66 = distinct !{!66, !57, !64, !24, !26, !25}
!67 = distinct !DISubprogram(name: "classify", scope: !1, file: !1, line: 21, type: !8, scopeLine: 21, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!68 = !DILocation(line: 22, column: 7, scope: !67)
!69 = !DILocation(line: 27, column: 1, scope: !67)
