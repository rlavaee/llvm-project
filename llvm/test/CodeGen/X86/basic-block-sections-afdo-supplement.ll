; RUN: echo 'v1' > %t.propeller
; RUN: echo 'f foo' >> %t.propeller
; RUN: echo 'g 0:100,1:100,2:0 1:100,3:100 2:0,3:0 3:100' >> %t.propeller
; RUN: echo 'f shared_loc' >> %t.propeller
; RUN: echo 'g 0:100,1:100,2:0 1:100,3:100 2:0,3:0 3:100' >> %t.propeller
; RUN: echo 'foo:200:100' > %t.afdo
; RUN: echo ' 5: 50' >> %t.afdo
; RUN: echo 'shared_loc:200:100' >> %t.afdo
; RUN: echo ' 1: inlined_func:100' >> %t.afdo
; RUN: echo '  0: 50' >> %t.afdo

; Case 1: Without AFDO profile supplement, block 2 has 0 Propeller samples and is split into cold section.
; RUN: llc < %s -O0 -mtriple=x86_64-pc-linux -function-sections -basic-block-sections=%t.propeller -basic-block-section-match-infer -propeller-match-bbid | \
; RUN: FileCheck %s --check-prefixes=CHECK,WITHOUT-AFDO

; Case 2: With AFDO profile supplement:
; - In foo, block 2 has a distinct debug location from block 1, so it gets AFDO samples and is kept in the main section.
; - In shared_loc, block 2 shares the same inline callstack with block 1 (which has nonzero Propeller weight),
;   so it is ambiguous and NOT supplemented by AFDO, remaining split into the cold section.
; RUN: llc < %s -O0 -mtriple=x86_64-pc-linux -function-sections -basic-block-sections=%t.propeller -basic-block-section-afdo-profile=%t.afdo -propeller-match-bbid | \
; RUN: FileCheck %s --check-prefixes=CHECK,WITH-AFDO

; Case 3: A missing AFDO profile is diagnosed.
; RUN: not llc < %s -O0 -mtriple=x86_64-pc-linux -function-sections -basic-block-sections=%t.propeller -basic-block-section-afdo-profile=%t.missing 2>&1 | \
; RUN: FileCheck %s --check-prefix=MISSING
; MISSING: error: {{.*}}.missing: Could not open AFDO profile:

define void @foo(i1 zeroext %0) nounwind !dbg !4 {
  %2 = alloca i8, align 1
  %3 = zext i1 %0 to i8
  store i8 %3, ptr %2, align 1, !dbg !10
  %4 = load i8, ptr %2, align 1, !dbg !11
  %5 = trunc i8 %4 to i1, !dbg !11
  br i1 %5, label %6, label %8, !dbg !12

6:                                                ; preds = %1
  %7 = call i32 @bar(), !dbg !13
  br label %10, !dbg !14

8:                                                ; preds = %1
  %9 = call i32 @baz(), !dbg !15
  br label %10, !dbg !16

10:                                               ; preds = %8, %6
  ret void, !dbg !17
}

define void @shared_loc(i1 zeroext %0) nounwind !dbg !20 {
  %2 = alloca i8, align 1
  %3 = zext i1 %0 to i8
  store i8 %3, ptr %2, align 1, !dbg !24
  %4 = load i8, ptr %2, align 1, !dbg !24
  %5 = trunc i8 %4 to i1, !dbg !24
  br i1 %5, label %6, label %8, !dbg !24

6:                                                ; preds = %1
  %7 = call i32 @bar(), !dbg !25
  br label %10, !dbg !24

8:                                                ; preds = %1
  %9 = call i32 @baz(), !dbg !25
  br label %10, !dbg !24

10:                                               ; preds = %8, %6
  ret void, !dbg !24
}

declare i32 @bar()
declare i32 @baz()

; CHECK:                    .section        .text.foo,"ax",@progbits
; CHECK-LABEL:              foo:
; CHECK-LABEL:              # %bb.0:
; CHECK-LABEL:              # %bb.1:
; CHECK:                    callq   bar@PLT

; WITHOUT-AFDO-LABEL:       .LBB0_3:
; WITHOUT-AFDO:             retq
; WITHOUT-AFDO:             .section        .text.split.foo,"ax",@progbits
; WITHOUT-AFDO-LABEL:       foo.cold:
; WITHOUT-AFDO:             callq   baz@PLT

; WITH-AFDO-LABEL:          .LBB0_2:
; WITH-AFDO:                callq   baz@PLT
; WITH-AFDO-NOT:            .section        .text.split.foo

; For shared_loc, block 2 shares !dbg !25 with block 1 (which has nonzero Propeller weight).
; It must be conservatively excluded from AFDO supplement, and thus split into .text.split.shared_loc.
; CHECK:                    .section        .text.shared_loc,"ax",@progbits
; CHECK-LABEL:              shared_loc:
; WITH-AFDO:                .section        .text.split.shared_loc,"ax",@progbits
; WITH-AFDO-LABEL:          shared_loc.cold:
; WITH-AFDO:                callq   baz@PLT

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: LineTablesOnly)
!1 = !DIFile(filename: "test.cc", directory: "/tmp")
!2 = !{i32 2, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "foo", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{null}
!10 = !DILocation(line: 1, column: 1, scope: !4)
!11 = !DILocation(line: 2, column: 1, scope: !4)
!12 = !DILocation(line: 3, column: 1, scope: !4)
!13 = !DILocation(line: 4, column: 1, scope: !4)
!14 = !DILocation(line: 5, column: 1, scope: !4)
!15 = !DILocation(line: 6, column: 1, scope: !4)
!16 = !DILocation(line: 7, column: 1, scope: !4)
!17 = !DILocation(line: 8, column: 1, scope: !4)

!20 = distinct !DISubprogram(name: "shared_loc", scope: !1, file: !1, line: 20, type: !5, scopeLine: 20, spFlags: DISPFlagDefinition, unit: !0)
!21 = distinct !DISubprogram(name: "inlined_func", scope: !1, file: !1, line: 30, type: !5, scopeLine: 30, spFlags: DISPFlagDefinition, unit: !0)
!22 = !DILocation(line: 21, column: 1, scope: !20)
!24 = !DILocation(line: 20, column: 1, scope: !20)
!25 = !DILocation(line: 30, column: 1, scope: !21, inlinedAt: !22)
