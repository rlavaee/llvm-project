get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
set(LIBS
  ${dialect_libs}
  MLIRParser
  MLIRPass
  MLIRSPIRV
  MLIRSPIRVSerialization
  MLIRTargetAVX512
  MLIRTargetLLVMIR
  MLIRTargetNVVMIR
  MLIRTargetROCDLIR
  MLIRTranslation
  MLIRSupport
)
add_llvm_tool(mlir-translate
  mlir-translate.cpp
)
llvm_update_compile_flags(mlir-translate)
target_link_libraries(mlir-translate PRIVATE MLIRIR MLIRTranslation ${LIBS} LLVMSupport)
