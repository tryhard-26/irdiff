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
  br label %for.body, !dbg !11

for.cond.cleanup:                                 ; preds = %for.body, %entry
  %s.0.lcssa = phi i32 [ 0, %entry ], [ %add, %for.body ], !dbg !12
  ret i32 %s.0.lcssa, !dbg !13

for.body:                                         ; preds = %for.body.preheader, %for.body
  %indvars.iv = phi i64 [ 0, %for.body.preheader ], [ %indvars.iv.next, %for.body ]
  %s.05 = phi i32 [ 0, %for.body.preheader ], [ %add, %for.body ]
  %arrayidx = getelementptr inbounds i32, ptr %a, i64 %indvars.iv, !dbg !14
  %0 = load i32, ptr %arrayidx, align 4, !dbg !14, !tbaa !15
  %mul.i = mul nsw i32 %0, %0, !dbg !19
  %add = add nuw nsw i32 %mul.i, %s.05, !dbg !22
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !23
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !10
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !11, !llvm.loop !24
}

; Function Attrs: nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @scale(ptr nocapture noundef writeonly %dst, ptr nocapture noundef readonly %src, ptr nocapture noundef readonly %k, i32 noundef %n) local_unnamed_addr #1 !dbg !28 {
entry:
  %cmp6 = icmp sgt i32 %n, 0, !dbg !29
  br i1 %cmp6, label %for.body.preheader, label %for.cond.cleanup, !dbg !30

for.body.preheader:                               ; preds = %entry
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !29
  br label %for.body, !dbg !30

for.cond.cleanup:                                 ; preds = %for.body, %entry
  ret void, !dbg !31

for.body:                                         ; preds = %for.body.preheader, %for.body
  %indvars.iv = phi i64 [ 0, %for.body.preheader ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds float, ptr %src, i64 %indvars.iv, !dbg !32
  %0 = load float, ptr %arrayidx, align 4, !dbg !32, !tbaa !33
  %1 = load float, ptr %k, align 4, !dbg !35, !tbaa !33
  %mul = fmul float %0, %1, !dbg !36
  %arrayidx2 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv, !dbg !37
  store float %mul, ptr %arrayidx2, align 4, !dbg !38, !tbaa !33
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !39
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !29
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !30, !llvm.loop !40
}

; Function Attrs: nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @scale_restrict(ptr noalias nocapture noundef writeonly %dst, ptr noalias nocapture noundef readonly %src, ptr noalias nocapture noundef readonly %k, i32 noundef %n) local_unnamed_addr #1 !dbg !42 {
entry:
  %cmp6 = icmp sgt i32 %n, 0, !dbg !43
  br i1 %cmp6, label %for.body.lr.ph, label %for.cond.cleanup, !dbg !44

for.body.lr.ph:                                   ; preds = %entry
  %0 = load float, ptr %k, align 4, !tbaa !33
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !43
  br label %for.body, !dbg !44

for.cond.cleanup:                                 ; preds = %for.body, %entry
  ret void, !dbg !45

for.body:                                         ; preds = %for.body.lr.ph, %for.body
  %indvars.iv = phi i64 [ 0, %for.body.lr.ph ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds float, ptr %src, i64 %indvars.iv, !dbg !46
  %1 = load float, ptr %arrayidx, align 4, !dbg !46, !tbaa !33
  %mul = fmul float %1, %0, !dbg !47
  %arrayidx2 = getelementptr inbounds float, ptr %dst, i64 %indvars.iv, !dbg !48
  store float %mul, ptr %arrayidx2, align 4, !dbg !49, !tbaa !33
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !50
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !43
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !44, !llvm.loop !51
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local range(i32 -1, 2) i32 @classify(i32 noundef %x) local_unnamed_addr #2 !dbg !53 {
entry:
  %cmp1 = icmp ne i32 %x, 0, !dbg !54
  %. = zext i1 %cmp1 to i32, !dbg !54
  %cmp.inv = icmp sgt i32 %x, -1, !dbg !54
  %retval.0 = select i1 %cmp.inv, i32 %., i32 -1, !dbg !54
  ret i32 %retval.0, !dbg !55
}

attributes #0 = { nofree norecurse nosync nounwind memory(argmem: read) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

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
!12 = !DILocation(line: 0, scope: !7)
!13 = !DILocation(line: 7, column: 3, scope: !7)
!14 = !DILocation(line: 6, column: 17, scope: !7)
!15 = !{!16, !16, i64 0}
!16 = !{!"int", !17, i64 0}
!17 = !{!"omnipotent char", !18, i64 0}
!18 = !{!"Simple C/C++ TBAA"}
!19 = !DILocation(line: 1, column: 37, scope: !20, inlinedAt: !21)
!20 = distinct !DISubprogram(name: "square", scope: !1, file: !1, line: 1, type: !8, scopeLine: 1, flags: DIFlagPrototyped, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!21 = distinct !DILocation(line: 6, column: 10, scope: !7)
!22 = !DILocation(line: 6, column: 7, scope: !7)
!23 = !DILocation(line: 5, column: 26, scope: !7)
!24 = distinct !{!24, !11, !25, !26, !27}
!25 = !DILocation(line: 6, column: 21, scope: !7)
!26 = !{!"llvm.loop.mustprogress"}
!27 = !{!"llvm.loop.unroll.disable"}
!28 = distinct !DISubprogram(name: "scale", scope: !1, file: !1, line: 10, type: !8, scopeLine: 10, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!29 = !DILocation(line: 11, column: 21, scope: !28)
!30 = !DILocation(line: 11, column: 3, scope: !28)
!31 = !DILocation(line: 13, column: 1, scope: !28)
!32 = !DILocation(line: 12, column: 14, scope: !28)
!33 = !{!34, !34, i64 0}
!34 = !{!"float", !17, i64 0}
!35 = !DILocation(line: 12, column: 23, scope: !28)
!36 = !DILocation(line: 12, column: 21, scope: !28)
!37 = !DILocation(line: 12, column: 5, scope: !28)
!38 = !DILocation(line: 12, column: 12, scope: !28)
!39 = !DILocation(line: 11, column: 26, scope: !28)
!40 = distinct !{!40, !30, !41, !26, !27}
!41 = !DILocation(line: 12, column: 24, scope: !28)
!42 = distinct !DISubprogram(name: "scale_restrict", scope: !1, file: !1, line: 15, type: !8, scopeLine: 16, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!43 = !DILocation(line: 17, column: 21, scope: !42)
!44 = !DILocation(line: 17, column: 3, scope: !42)
!45 = !DILocation(line: 19, column: 1, scope: !42)
!46 = !DILocation(line: 18, column: 14, scope: !42)
!47 = !DILocation(line: 18, column: 21, scope: !42)
!48 = !DILocation(line: 18, column: 5, scope: !42)
!49 = !DILocation(line: 18, column: 12, scope: !42)
!50 = !DILocation(line: 17, column: 26, scope: !42)
!51 = distinct !{!51, !44, !52, !26, !27}
!52 = !DILocation(line: 18, column: 24, scope: !42)
!53 = distinct !DISubprogram(name: "classify", scope: !1, file: !1, line: 21, type: !8, scopeLine: 21, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!54 = !DILocation(line: 22, column: 7, scope: !53)
!55 = !DILocation(line: 27, column: 1, scope: !53)
