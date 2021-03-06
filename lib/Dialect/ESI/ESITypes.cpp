//===- ESITypes.cpp - ESI types code defs -----------------------*- C++ -*-===//
//
// Definitions for esi data types. Anything which doesn't have to be public
// should go in here.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/ESI/ESITypes.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace circt::esi;

Type ChannelPort::parse(mlir::MLIRContext *ctxt, mlir::DialectAsmParser &p) {
  Type inner;
  if (p.parseLess() || p.parseType(inner) || p.parseGreater())
    return Type();
  return get(ctxt, inner);
}

void ChannelPort::print(mlir::DialectAsmPrinter &p) const {
  p << "channel<";
  p.printType(getInner());
  p << ">";
}

#define GET_TYPEDEF_CLASSES
#include "circt/Dialect/ESI/ESITypes.cpp.inc"
