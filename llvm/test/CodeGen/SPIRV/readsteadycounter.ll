; RUN: llc -O0 -mtriple=spirv64-unknown-unknown --spirv-ext=+SPV_KHR_shader_clock %s -o - | FileCheck %s
; RUN: llc -O0 -mtriple=spirv64-intel %s -o - | FileCheck %s
; RUN: %if spirv-tools %{ llc -O0 -mtriple=spirv64-unknown-unknown --spirv-ext=+SPV_KHR_shader_clock %s -o - -filetype=obj | spirv-val %}
; RUN: %if spirv-tools %{ llc -O0 -mtriple=spirv64-intel %s -o - -filetype=obj | spirv-val %}

; CHECK-DAG: OpCapability ShaderClockKHR
; CHECK-DAG: OpExtension "SPV_KHR_shader_clock"

; CHECK-DAG: %[[#TyLong:]] = OpTypeInt 64 0
; CHECK-DAG: %[[#ScopeDevice:]] = OpConstant %[[#]] 1

; CHECK: %[[#]] = OpReadClockKHR %[[#TyLong]] %[[#ScopeDevice]]
; CHECK: %[[#]] = OpReadClockKHR %[[#TyLong]] %[[#ScopeDevice]]

define spir_kernel void @test_readsteadycounter(ptr addrspace(1) %out) {
  %cycle0 = call i64 @llvm.readsteadycounter()
  store volatile i64 %cycle0, ptr addrspace(1) %out

  %cycle1 = call i64 @llvm.readsteadycounter()
  store volatile i64 %cycle1, ptr addrspace(1) %out
  ret void
}

declare i64 @llvm.readsteadycounter()
