//===- Ops.cpp - Implement the RTL operations -----------------------------===//
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/RTL/Ops.h"
#include "circt/Dialect/RTL/Visitors.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/FunctionSupport.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"

using namespace circt;
using namespace rtl;

//===----------------------------------------------------------------------===//
// RTLModuleOp
//===----------------------------------------------------------------------===/

static void buildModule(OpBuilder &builder, OperationState &result,
                        StringAttr name, ArrayRef<RTLModulePortInfo> ports) {
  using namespace mlir::impl;

  // Add an attribute for the name.
  result.addAttribute(::mlir::SymbolTable::getSymbolAttrName(), name);

  SmallVector<Type, 4> argTypes;
  SmallVector<Type, 4> resultTypes;
  for (auto elt : ports) {
    if (elt.isOutput())
      resultTypes.push_back(elt.type);
    else
      argTypes.push_back(elt.type);
  }

  // Record the argument and result types as an attribute.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // Record the names of the arguments if present.
  SmallString<8> attrNameBuf;
  SmallString<8> attrDirBuf;
  for (const RTLModulePortInfo &port : ports) {
    SmallVector<NamedAttribute, 2> argAttrs;
    if (!port.name.getValue().empty())
      argAttrs.push_back(
          NamedAttribute(builder.getIdentifier("rtl.name"), port.name));

    if (port.direction == PortDirection::INOUT)
      argAttrs.push_back(NamedAttribute(builder.getIdentifier("rtl.inout"),
                                        builder.getUnitAttr()));

    StringRef attrName = port.isOutput()
                             ? getResultAttrName(port.argNum, attrNameBuf)
                             : getArgAttrName(port.argNum, attrNameBuf);
    result.addAttribute(attrName, builder.getDictionaryAttr(argAttrs));
  }
  result.addRegion();
}

void rtl::RTLModuleOp::build(OpBuilder &builder, OperationState &result,
                             StringAttr name,
                             ArrayRef<RTLModulePortInfo> ports) {
  buildModule(builder, result, name, ports);

  // Create a region and a block for the body.
  auto *bodyRegion = result.regions[0].get();
  Block *body = new Block();
  bodyRegion->push_back(body);

  // Add arguments to the body block.
  for (auto elt : ports)
    if (!elt.isOutput())
      body->addArgument(elt.type);

  rtl::RTLModuleOp::ensureTerminator(*bodyRegion, builder, result.location);
}

void rtl::RTLExternModuleOp::build(OpBuilder &builder, OperationState &result,
                                   StringAttr name,
                                   ArrayRef<RTLModulePortInfo> ports) {
  buildModule(builder, result, name, ports);
}

FunctionType rtl::getModuleType(Operation *op) {
  auto typeAttr = op->getAttrOfType<TypeAttr>(RTLModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

StringAttr rtl::getRTLNameAttr(ArrayRef<NamedAttribute> attrs) {
  for (auto &argAttr : attrs) {
    if (argAttr.first != "rtl.name")
      continue;
    return argAttr.second.dyn_cast<StringAttr>();
  }
  return StringAttr();
}

static bool containsInOutAttr(ArrayRef<NamedAttribute> attrs) {
  for (auto &argAttr : attrs) {
    if (argAttr.first == "rtl.inout")
      return true;
  }
  return false;
}

void rtl::getRTLModulePortInfo(Operation *op,
                               SmallVectorImpl<RTLModulePortInfo> &results) {
  auto argTypes = getModuleType(op).getInputs();

  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    auto argAttrs = ::mlir::impl::getArgAttrs(op, i);
    bool isInout = containsInOutAttr(argAttrs);

    results.push_back({getRTLNameAttr(argAttrs),
                       isInout ? PortDirection::INOUT : PortDirection::INPUT,
                       argTypes[i], i});
  }

  auto resultTypes = getModuleType(op).getResults();
  for (unsigned i = 0, e = resultTypes.size(); i < e; ++i) {
    auto argAttrs = ::mlir::impl::getResultAttrs(op, i);
    results.push_back(
        {getRTLNameAttr(argAttrs), PortDirection::OUTPUT, resultTypes[i], i});
  }
}

static ParseResult parseRTLModuleOp(OpAsmParser &parser, OperationState &result,
                                    bool isExtModule = false) {
  using namespace mlir::impl;

  SmallVector<OpAsmParser::OperandType, 4> entryArgs;
  SmallVector<NamedAttrList, 4> argAttrs;
  SmallVector<NamedAttrList, 4> resultAttrs;
  SmallVector<Type, 4> argTypes;
  SmallVector<Type, 4> resultTypes;
  auto &builder = parser.getBuilder();

  // Parse the name as a symbol.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, ::mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  // Parse the function signature.
  bool isVariadic = false;

  if (parseFunctionSignature(parser, /*allowVariadic=*/false, entryArgs,
                             argTypes, argAttrs, isVariadic, resultTypes,
                             resultAttrs))
    return failure();

  // Record the argument and result types as an attribute.  This is necessary
  // for external modules.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // If function attributes are present, parse them.
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  assert(argAttrs.size() == argTypes.size());
  assert(resultAttrs.size() == resultTypes.size());

  auto *context = result.getContext();

  // Postprocess each of the arguments.  If there was no 'rtl.name'
  // attribute, and if the argument name was non-numeric, then add the
  // rtl.name attribute with the textual name from the IR.  The name in the
  // text file is a load-bearing part of the IR, but we don't want the
  // verbosity in dumps of including it explicitly in the attribute
  // dictionary.
  for (size_t i = 0, e = argAttrs.size(); i != e; ++i) {
    auto &attrs = argAttrs[i];

    // If an explicit name attribute was present, don't add the implicit one.
    bool hasNameAttr = false;
    for (auto &elt : attrs)
      if (elt.first.str() == "rtl.name")
        hasNameAttr = true;
    if (hasNameAttr || entryArgs.empty())
      continue;

    auto &arg = entryArgs[i];

    // The name of an argument is of the form "%42" or "%id", and since
    // parsing succeeded, we know it always has one character.
    assert(arg.name.size() > 1 && arg.name[0] == '%' && "Unknown MLIR name");
    if (isdigit(arg.name[1]))
      continue;

    auto nameAttr = StringAttr::get(arg.name.drop_front(), context);
    attrs.push_back({Identifier::get("rtl.name", context), nameAttr});
  }

  // Add the attributes to the function arguments.
  addArgAndResultAttrs(builder, result, argAttrs, resultAttrs);

  // Parse the optional function body.
  auto *body = result.addRegion();
  if (parser.parseOptionalRegion(
          *body, entryArgs, entryArgs.empty() ? ArrayRef<Type>() : argTypes))
    return failure();

  if (!isExtModule)
    RTLModuleOp::ensureTerminator(*body, parser.getBuilder(), result.location);
  return success();
}

static ParseResult parseRTLExternModuleOp(OpAsmParser &parser,
                                          OperationState &result) {
  return parseRTLModuleOp(parser, result, /*isExtModule:*/ true);
}

FunctionType getRTLModuleOpType(Operation *op) {
  auto typeAttr =
      op->getAttrOfType<TypeAttr>(rtl::RTLModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

static void printRTLModuleOp(OpAsmPrinter &p, Operation *op) {
  using namespace mlir::impl;

  FunctionType fnType = getRTLModuleOpType(op);
  auto argTypes = fnType.getInputs();
  auto resultTypes = fnType.getResults();

  // Print the operation and the function name.
  auto funcName =
      op->getAttrOfType<StringAttr>(::mlir::SymbolTable::getSymbolAttrName())
          .getValue();
  p << op->getName() << ' ';
  p.printSymbolName(funcName);

  printFunctionSignature(p, op, argTypes, /*isVariadic=*/false, resultTypes);
  printFunctionAttributes(p, op, argTypes.size(), resultTypes.size());
}

static void print(OpAsmPrinter &p, RTLExternModuleOp op) {
  printRTLModuleOp(p, op);
}

static void print(OpAsmPrinter &p, RTLModuleOp op) {
  printRTLModuleOp(p, op);

  // Print the body if this is not an external function.
  Region &body = op.getBody();
  if (!body.empty())
    p.printRegion(body, /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
}

//===----------------------------------------------------------------------===//
// RTLInstanceOp
//===----------------------------------------------------------------------===/

static LogicalResult verifyRTLInstanceOp(RTLInstanceOp op) {
  auto moduleIR = op.getParentWithTrait<OpTrait::SymbolTable>();
  if (moduleIR == nullptr) {
    op.emitError("Must be contained within a SymbolTable region");
    return failure();
  }
  auto referencedModule =
      mlir::SymbolTable::lookupSymbolIn(moduleIR, op.moduleName());
  if (referencedModule == nullptr) {
    op.emitError("Cannot find module definition '") << op.moduleName() << "'.";
    return failure();
  }
  if (!isa<rtl::RTLModuleOp>(referencedModule) &&
      !isa<rtl::RTLExternModuleOp>(referencedModule)) {
    op.emitError("Symbol resolved to '")
        << referencedModule->getName() << "' which is not a RTL[Ext]ModuleOp.";
    return failure();
  }
  return success();
}

StringAttr RTLInstanceOp::getResultName(size_t idx) {
  if (auto nameAttrList = getAttrOfType<ArrayAttr>("name"))
    if (idx < nameAttrList.size())
      return nameAttrList[idx].dyn_cast<StringAttr>();
  return StringAttr();
}

/// Intercept the `attr-dict` parsing to inject the result names which _may_ be
/// missing.
ParseResult parseResultNames(OpAsmParser &p, NamedAttrList &attrDict) {
  MLIRContext *ctxt = p.getBuilder().getContext();
  if (p.parseOptionalAttrDict(attrDict))
    return failure();

  // Assemble the result names from the asm.
  SmallVector<Attribute, 8> names;
  for (size_t i = 0, e = p.getNumResults(); i < e; ++i) {
    names.push_back(StringAttr::get(p.getResultName(i).first, ctxt));
  }

  // Look for existing result names in the attr-dict and if they exist and are
  // non-empty, replace them in the 'names' vector.
  auto resultNamesID = Identifier::get("name", ctxt);
  if (auto namesAttr = attrDict.getNamed(resultNamesID)) {
    // It must be an ArrayAttr.
    if (auto nameAttrList = namesAttr->second.dyn_cast<ArrayAttr>()) {
      for (size_t i = 0, e = nameAttrList.size(); i < e; ++i) {
        // List of result names must be no longer than number of results.
        if (i >= names.size())
          break;
        // And it must be a string.
        if (auto resultNameStringAttr =
                nameAttrList[i].dyn_cast<StringAttr>()) {
          // Only replace if non-empty.
          if (!resultNameStringAttr.getValue().empty())
            names[i] = resultNameStringAttr;
        }
      }
    }
  }
  attrDict.set("name", ArrayAttr::get(names, ctxt));
  return success();
}

/// Intercept the `attr-dict` printing to determine whether or not we can elide
/// the result names attribute.
void printResultNames(OpAsmPrinter &p, RTLInstanceOp *op) {
  SmallVector<StringRef, 8> elideFields = {"instanceName", "moduleName"};

  // If any names don't match what the printer is going to emit, keep the
  // attributes.
  bool nameDisagreement = false;
  ArrayAttr nameAttrList = op->getAttrOfType<ArrayAttr>("name");
  // Look for result names to possibly elide.
  if (nameAttrList && nameAttrList.size() <= op->getNumResults()) {
    // Check that all the result names have been kept.
    for (size_t i = 0, e = nameAttrList.size(); i < e; ++i) {
      // Name must be a string.
      if (auto expectedName = nameAttrList[i].dyn_cast<StringAttr>()) {
        // Check for disagreement
        SmallString<32> resultNameStr;
        llvm::raw_svector_ostream tmpStream(resultNameStr);
        p.printOperand(op->getResult(i), tmpStream);
        if (tmpStream.str().drop_front() != expectedName.getValue()) {
          nameDisagreement = true;
        }
      }
    }
  }
  if (!nameDisagreement)
    elideFields.push_back("name");

  p.printOptionalAttrDict(op->getAttrs(), elideFields);
}

/// Suggest a name for each result value based on the saved result names
/// attribute.
void RTLInstanceOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
  ArrayAttr nameAttrList = getAttrOfType<ArrayAttr>("name");
  if (nameAttrList && nameAttrList.size() <= getNumResults())
    for (size_t i = 0, e = nameAttrList.size(); i < e; ++i)
      if (auto resultName = nameAttrList[i].dyn_cast<StringAttr>())
        setNameFn(getResult(i), resultName.getValue());
}

//===----------------------------------------------------------------------===//
// RTLOutputOp
//===----------------------------------------------------------------------===/

/// Verify that the num of operands and types fit the declared results.
static LogicalResult verifyOutputOp(OutputOp *op) {
  OperandRange outputValues = op->getOperands();
  auto opParent = op->getParentOp();

  // Check that we are in the correct region. OutputOp should be directly
  // contained by an RTLModuleOp region. We'll loosen this restriction if
  // there's a compelling use case.
  if (!isa<RTLModuleOp>(opParent)) {
    op->emitOpError("operation expected to be in a RTLModuleOp.");
    return failure();
  }

  // Check that the we (rtl.output) have the same number of operands as our
  // region has results.
  FunctionType modType = getModuleType(opParent);
  ArrayRef<Type> modResults = modType.getResults();
  if (modResults.size() != outputValues.size()) {
    op->emitOpError("must have same number of operands as region results.");
    return failure();
  }

  // Check that the types of our operands and the region's results match.
  for (size_t i = 0, e = modResults.size(); i < e; ++i) {
    if (modResults[i] != outputValues[i].getType()) {
      op->emitOpError("output types must match module. In "
                      "operand ")
          << i << ", expected " << modResults[i] << ", but got "
          << outputValues[i].getType() << ".";
      return failure();
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// RTL combinational ops
//===----------------------------------------------------------------------===/

/// Return true if the specified operation is a combinatorial logic op.
bool rtl::isCombinatorial(Operation *op) {
  struct IsCombClassifier
      : public CombinatorialVisitor<IsCombClassifier, bool> {
    bool visitInvalidComb(Operation *op) { return false; }
    bool visitUnhandledComb(Operation *op) { return true; }
  };

  return IsCombClassifier().dispatchCombinatorialVisitor(op);
}

static Attribute getIntAttr(const APInt &value, MLIRContext *context) {
  return IntegerAttr::get(IntegerType::get(value.getBitWidth(), context),
                          value);
}

namespace {
struct ConstantIntMatcher {
  APInt &value;
  ConstantIntMatcher(APInt &value) : value(value) {}
  bool match(Operation *op) {
    if (auto cst = dyn_cast<ConstantOp>(op)) {
      value = cst.value();
      return true;
    }
    return false;
  }
};
} // end anonymous namespace

static inline ConstantIntMatcher m_RConstant(APInt &value) {
  return ConstantIntMatcher(value);
}

//===----------------------------------------------------------------------===//
// WireOp
//===----------------------------------------------------------------------===//

static void printWireOp(OpAsmPrinter &p, WireOp &op) {
  p << op.getOperationName();
  // Note that we only need to print the "name" attribute if the asmprinter
  // result name disagrees with it.  This can happen in strange cases, e.g.
  // when there are conflicts.
  bool namesDisagree = false;

  SmallString<32> resultNameStr;
  llvm::raw_svector_ostream tmpStream(resultNameStr);
  p.printOperand(op.getResult(), tmpStream);
  auto expectedName = op.nameAttr();
  if (!expectedName ||
      tmpStream.str().drop_front() != expectedName.getValue()) {
    namesDisagree = true;
  }

  if (namesDisagree)
    p.printOptionalAttrDict(op.getAttrs());
  else
    p.printOptionalAttrDict(op.getAttrs(), {"name"});

  p << " : " << op.getType();
}

static ParseResult parseWireOp(OpAsmParser &parser, OperationState &result) {
  Type resultType;

  if (parser.parseOptionalAttrDict(result.attributes) || parser.parseColon() ||
      parser.parseType(resultType))
    return failure();

  result.addTypes(resultType);

  // If the attribute dictionary contains no 'name' attribute, infer it from
  // the SSA name (if specified).
  bool hadName = llvm::any_of(result.attributes, [](NamedAttribute attr) {
    return attr.first == "name";
  });

  // If there was no name specified, check to see if there was a useful name
  // specified in the asm file.
  if (hadName)
    return success();

  auto resultName = parser.getResultName(0);
  if (!resultName.first.empty() && !isdigit(resultName.first[0])) {
    StringRef name = resultName.first;
    auto *context = result.getContext();
    auto nameAttr = parser.getBuilder().getStringAttr(name);
    result.attributes.push_back({Identifier::get("name", context), nameAttr});
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

static LogicalResult verifyConstantOp(ConstantOp constant) {
  // If the result type has a bitwidth, then the attribute must match its width.
  auto intType = constant.getType().cast<IntegerType>();
  if (constant.value().getBitWidth() != intType.getWidth()) {
    constant.emitError(
        "firrtl.constant attribute bitwidth doesn't match return type");
    return failure();
  }

  return success();
}

OpFoldResult ConstantOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.empty() && "constant has no operands");
  return valueAttr();
}

/// Build a ConstantOp from an APInt, infering the result type from the
/// width of the APInt.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       const APInt &value) {

  auto type = IntegerType::get(value.getBitWidth(), IntegerType::Signless,
                               builder.getContext());
  auto attr = builder.getIntegerAttr(type, value);
  return build(builder, result, type, attr);
}

/// This builder allows construction of small signed integers like 0, 1, -1
/// matching a specified MLIR IntegerType.  This shouldn't be used for general
/// constant folding because it only works with values that can be expressed in
/// an int64_t.  Use APInt's instead.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       int64_t value, IntegerType type) {
  auto numBits = type.getWidth();
  build(builder, result, APInt(numBits, (uint64_t)value, /*isSigned=*/true));
}

/// Flattens a single input in `op` if `hasOneUse` is true and it can be defined
/// as an Op. Returns true if successful, and false otherwise.
/// Example: op(1, 2, op(3, 4), 5) -> op(1, 2, 3, 4, 5)  // returns true
template <typename Op>
static bool tryFlatteningOperands(Op op, PatternRewriter &rewriter) {
  auto inputs = op.inputs();

  for (size_t i = 0, size = inputs.size(); i != size; ++i) {
    if (!inputs[i].hasOneUse())
      continue;
    auto flattenOp = inputs[i].template getDefiningOp<Op>();
    if (!flattenOp)
      continue;
    auto flattenOpInputs = flattenOp.inputs();

    SmallVector<Value, 4> newOperands;
    newOperands.reserve(size + flattenOpInputs.size());

    auto flattenOpIndex = inputs.begin() + i;
    newOperands.append(inputs.begin(), flattenOpIndex);
    newOperands.append(flattenOpInputs.begin(), flattenOpInputs.end());
    newOperands.append(flattenOpIndex + 1, inputs.end());

    rewriter.replaceOpWithNewOp<Op>(op, op.getType(), newOperands);
    return true;
  }
  return false;
}

void ConstantOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  auto intTy = getType().cast<IntegerType>();
  auto intCst = getValue();

  // Sugar i1 constants with 'true' and 'false'.
  if (intTy.getWidth() == 1)
    return setNameFn(getResult(), intCst.isNullValue() ? "false" : "true");

  // Otherwise, build a complex name with the value and type.
  SmallVector<char, 32> specialNameBuffer;
  llvm::raw_svector_ostream specialName(specialNameBuffer);
  specialName << 'c' << intCst << '_' << intTy;
  setNameFn(getResult(), specialName.str());
}

//===----------------------------------------------------------------------===//
// Unary Operations
//===----------------------------------------------------------------------===//

// Verify SExtOp and ZExtOp.
static LogicalResult verifyExtOp(Operation *op) {
  // The source must be smaller than the dest type.  Both are already known to
  // be signless integers.
  auto srcType = op->getOperand(0).getType().cast<IntegerType>();
  auto dstType = op->getResult(0).getType().cast<IntegerType>();
  if (srcType.getWidth() >= dstType.getWidth()) {
    op->emitOpError("extension must increase bitwidth of operand");
    return failure();
  }

  return success();
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

void ConcatOp::build(OpBuilder &builder, OperationState &result,
                     ValueRange inputs) {
  unsigned resultWidth = 0;
  for (auto input : inputs) {
    resultWidth += input.getType().cast<IntegerType>().getWidth();
  }
  build(builder, result, builder.getIntegerType(resultWidth), inputs);
}

static LogicalResult verifyExtractOp(ExtractOp op) {
  unsigned srcWidth = op.input().getType().cast<IntegerType>().getWidth();
  unsigned dstWidth = op.getType().cast<IntegerType>().getWidth();
  if (op.lowBit() >= srcWidth || srcWidth - op.lowBit() < dstWidth)
    return op.emitOpError("from bit too large for input"), failure();

  return success();
}

OpFoldResult ExtractOp::fold(ArrayRef<Attribute> operands) {
  // If we are extracting the entire input, then return it.
  if (input().getType() == getType())
    return input();

  // Constant fold.
  APInt value;
  if (mlir::matchPattern(input(), m_RConstant(value))) {
    unsigned dstWidth = getType().cast<IntegerType>().getWidth();
    return getIntAttr(value.lshr(lowBit()).trunc(dstWidth), getContext());
  }
  return {};
}

//===----------------------------------------------------------------------===//
// Variadic operations
//===----------------------------------------------------------------------===//

static LogicalResult verifyUTVariadicRTLOp(Operation *op) {
  auto size = op->getOperands().size();
  if (size < 1)
    return op->emitOpError("requires 1 or more args");

  return success();
}

OpFoldResult AndOp::fold(ArrayRef<Attribute> operands) {
  auto size = inputs().size();

  // and(x) -> x -- noop
  if (size == 1u)
    return inputs()[0];

  APInt value;

  // and(..., 0) -> 0 -- annulment
  if (matchPattern(inputs().back(), m_RConstant(value)) && value.isNullValue())
    return inputs().back();

  return {};
}

void AndOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  struct Folder final : public OpRewritePattern<AndOp> {
    using OpRewritePattern::OpRewritePattern;
    LogicalResult matchAndRewrite(AndOp op,
                                  PatternRewriter &rewriter) const override {
      auto inputs = op.inputs();
      auto size = inputs.size();
      assert(size > 1 && "expected 2 or more operands");

      APInt value, value2;

      // and(..., '1) -> and(...) -- identity
      if (matchPattern(inputs.back(), m_RConstant(value)) &&
          value.isAllOnesValue()) {
        rewriter.replaceOpWithNewOp<AndOp>(op, op.getType(),
                                           inputs.drop_back());
        return success();
      }

      // and(..., x, x) -> and(..., x) -- idempotent
      if (inputs[size - 1] == inputs[size - 2]) {
        rewriter.replaceOpWithNewOp<AndOp>(op, op.getType(),
                                           inputs.drop_back());
        return success();
      }

      // and(..., c1, c2) -> and(..., c3) where c3 = c1 & c2 -- constant folding
      if (matchPattern(inputs[size - 1], m_RConstant(value)) &&
          matchPattern(inputs[size - 2], m_RConstant(value2))) {
        auto cst = rewriter.create<ConstantOp>(op.getLoc(), value & value2);
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(cst);
        rewriter.replaceOpWithNewOp<AndOp>(op, op.getType(), newOperands);
        return success();
      }

      // and(x, and(...)) -> and(x, ...) -- flatten
      if (tryFlatteningOperands(op, rewriter))
        return success();

      /// TODO: and(..., x, not(x)) -> and(..., 0) -- complement
      return failure();
    }
  };
  results.insert<Folder>(context);
}

OpFoldResult OrOp::fold(ArrayRef<Attribute> operands) {
  auto size = inputs().size();

  // or(x) -> x -- noop
  if (size == 1u)
    return inputs()[0];

  APInt value;

  // or(..., '1) -> '1 -- annulment
  if (matchPattern(inputs().back(), m_RConstant(value)) &&
      value.isAllOnesValue())
    return inputs().back();
  return {};
}

void OrOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                       MLIRContext *context) {
  struct Folder final : public OpRewritePattern<OrOp> {
    using OpRewritePattern::OpRewritePattern;
    LogicalResult matchAndRewrite(OrOp op,
                                  PatternRewriter &rewriter) const override {
      auto inputs = op.inputs();
      auto size = inputs.size();
      assert(size > 1 && "expected 2 or more operands");

      APInt value, value2;

      // or(..., 0) -> or(...) -- identity
      if (matchPattern(inputs.back(), m_RConstant(value)) &&
          value.isNullValue()) {

        rewriter.replaceOpWithNewOp<OrOp>(op, op.getType(), inputs.drop_back());
        return success();
      }

      // or(..., x, x) -> or(..., x) -- idempotent
      if (inputs[size - 1] == inputs[size - 2]) {
        rewriter.replaceOpWithNewOp<OrOp>(op, op.getType(), inputs.drop_back());
        return success();
      }

      // or(..., c1, c2) -> or(..., c3) where c3 = c1 | c2 -- constant folding
      if (matchPattern(inputs[size - 1], m_RConstant(value)) &&
          matchPattern(inputs[size - 2], m_RConstant(value2))) {
        auto cst = rewriter.create<ConstantOp>(op.getLoc(), value | value2);
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(cst);
        rewriter.replaceOpWithNewOp<OrOp>(op, op.getType(), newOperands);
        return success();
      }

      // or(x, or(...)) -> or(x, ...) -- flatten
      if (tryFlatteningOperands(op, rewriter))
        return success();

      /// TODO: or(..., x, not(x)) -> or(..., '1) -- complement
      return failure();
    }
  };
  results.insert<Folder>(context);
}

OpFoldResult XorOp::fold(ArrayRef<Attribute> operands) {
  auto size = inputs().size();

  // xor(x) -> x -- noop
  if (size == 1u)
    return inputs()[0];

  // xor(x, x) -> 0 -- idempotent
  if (size == 2u && inputs()[0] == inputs()[1])
    return IntegerAttr::get(getType(), 0);

  return {};
}

void XorOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  struct Folder final : public OpRewritePattern<XorOp> {
    using OpRewritePattern::OpRewritePattern;
    LogicalResult matchAndRewrite(XorOp op,
                                  PatternRewriter &rewriter) const override {
      auto inputs = op.inputs();
      auto size = inputs.size();
      assert(size > 1 && "expected 2 or more operands");

      APInt value, value2;

      // xor(..., 0) -> xor(...) -- identity
      if (matchPattern(inputs.back(), m_RConstant(value)) &&
          value.isNullValue()) {

        rewriter.replaceOpWithNewOp<XorOp>(op, op.getType(),
                                           inputs.drop_back());
        return success();
      }

      if (inputs[size - 1] == inputs[size - 2]) {
        assert(size > 2 &&
               "expected idempotent case for 2 elements handled already.");
        // xor(..., x, x) -> xor (...) -- idempotent
        rewriter.replaceOpWithNewOp<XorOp>(op, op.getType(),
                                           inputs.drop_back(/*n=*/2));
        return success();
      }

      // xor(..., c1, c2) -> xor(..., c3) where c3 = c1 ^ c2 -- constant folding
      if (matchPattern(inputs[size - 1], m_RConstant(value)) &&
          matchPattern(inputs[size - 2], m_RConstant(value2))) {
        auto cst = rewriter.create<ConstantOp>(op.getLoc(), value ^ value2);
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(cst);
        rewriter.replaceOpWithNewOp<XorOp>(op, op.getType(), newOperands);
        return success();
      }

      // xor(x, xor(...)) -> xor(x, ...) -- flatten
      if (tryFlatteningOperands(op, rewriter))
        return success();

      /// TODO: xor(..., '1) -> not(xor(...))
      /// TODO: xor(..., x, not(x)) -> xor(..., '1)
      return failure();
    }
  };
  results.insert<Folder>(context);
}

OpFoldResult AddOp::fold(ArrayRef<Attribute> operands) {
  auto size = inputs().size();

  // add(x) -> x -- noop
  if (size == 1u)
    return inputs()[0];

  return {};
}

void AddOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  struct Folder final : public OpRewritePattern<AddOp> {
    using OpRewritePattern::OpRewritePattern;
    LogicalResult matchAndRewrite(AddOp op,
                                  PatternRewriter &rewriter) const override {
      auto inputs = op.inputs();
      auto size = inputs.size();
      assert(size > 1 && "expected 2 or more operands");

      APInt value, value2;

      // add(..., 0) -> add(...) -- identity
      if (matchPattern(inputs.back(), m_RConstant(value)) &&
          value.isNullValue()) {
        rewriter.replaceOpWithNewOp<AddOp>(op, op.getType(),
                                           inputs.drop_back());
        return success();
      }

      // add(..., c1, c2) -> add(..., c3) where c3 = c1 + c2 -- constant folding
      if (matchPattern(inputs[size - 1], m_RConstant(value)) &&
          matchPattern(inputs[size - 2], m_RConstant(value2))) {
        auto cst = rewriter.create<ConstantOp>(op.getLoc(), value + value2);
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(cst);
        rewriter.replaceOpWithNewOp<AddOp>(op, op.getType(), newOperands);
        return success();
      }

      // add(..., x, x) -> add(..., shl(x, 1))
      if (inputs[size - 1] == inputs[size - 2]) {
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));

        auto one = rewriter.create<ConstantOp>(
            op.getLoc(), 1, op.getType().cast<IntegerType>());
        auto shiftLeftOp =
            rewriter.create<rtl::ShlOp>(op.getLoc(), inputs.back(), one);

        newOperands.push_back(shiftLeftOp);
        rewriter.replaceOpWithNewOp<AddOp>(op, op.getType(), newOperands);
        return success();
      }

      auto shlOp = inputs[size - 1].getDefiningOp<rtl::ShlOp>();
      // add(..., x, shl(x, c)) -> add(..., mul(x, (1 << c) + 1))
      if (shlOp && shlOp.lhs() == inputs[size - 2] &&
          matchPattern(shlOp.rhs(), m_RConstant(value))) {

        APInt one(/*numBits=*/value.getBitWidth(), 1, /*isSigned=*/false);
        auto rhs =
            rewriter.create<ConstantOp>(op.getLoc(), (one << value) + one);

        std::array<Value, 2> factors = {shlOp.lhs(), rhs};
        auto mulOp = rewriter.create<rtl::MulOp>(op.getLoc(), factors);

        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(mulOp);
        rewriter.replaceOpWithNewOp<AddOp>(op, op.getType(), newOperands);
        return success();
      }

      auto mulOp = inputs[size - 1].getDefiningOp<rtl::MulOp>();
      // add(..., x, mul(x, c)) -> add(..., mul(x, c + 1))
      if (mulOp && mulOp.inputs().size() == 2 &&
          mulOp.inputs()[0] == inputs[size - 2] &&
          matchPattern(mulOp.inputs()[1], m_RConstant(value))) {

        APInt one(/*numBits=*/value.getBitWidth(), 1, /*isSigned=*/false);
        auto rhs = rewriter.create<ConstantOp>(op.getLoc(), value + one);
        std::array<Value, 2> factors = {mulOp.inputs()[0], rhs};
        auto newMulOp = rewriter.create<rtl::MulOp>(op.getLoc(), factors);

        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(newMulOp);
        rewriter.replaceOpWithNewOp<AddOp>(op, op.getType(), newOperands);
        return success();
      }

      // add(x, add(...)) -> add(x, ...) -- flatten
      if (tryFlatteningOperands(op, rewriter))
        return success();

      return failure();
    }
  };
  results.insert<Folder>(context);
}

OpFoldResult MulOp::fold(ArrayRef<Attribute> operands) {
  auto size = inputs().size();

  // mul(x) -> x -- noop
  if (size == 1u)
    return inputs()[0];

  APInt value;

  // mul(..., 0) -> 0 -- annulment
  if (matchPattern(inputs().back(), m_RConstant(value)) && value.isNullValue())
    return inputs().back();

  return {};
}

void MulOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  struct Folder final : public OpRewritePattern<MulOp> {
    using OpRewritePattern::OpRewritePattern;
    LogicalResult matchAndRewrite(MulOp op,
                                  PatternRewriter &rewriter) const override {
      auto inputs = op.inputs();
      auto size = inputs.size();
      assert(size > 1 && "expected 2 or more operands");

      APInt value, value2;

      // mul(x, c) -> shl(x, log2(c)), where c is a power of two.
      if (size == 2 && matchPattern(inputs.back(), m_RConstant(value)) &&
          value.isPowerOf2()) {
        auto shift =
            rewriter.create<ConstantOp>(op.getLoc(), value.exactLogBase2(),
                                        op.getType().cast<IntegerType>());
        auto shlOp = rewriter.create<rtl::ShlOp>(op.getLoc(), inputs[0], shift);

        rewriter.replaceOpWithNewOp<MulOp>(op, op.getType(),
                                           ArrayRef<Value>(shlOp));
        return success();
      }

      // mul(..., 1) -> mul(...) -- identity
      if (matchPattern(inputs.back(), m_RConstant(value)) && (value == 1u)) {
        rewriter.replaceOpWithNewOp<MulOp>(op, op.getType(),
                                           inputs.drop_back());
        return success();
      }

      // mul(..., c1, c2) -> mul(..., c3) where c3 = c1 * c2 -- constant folding
      if (matchPattern(inputs[size - 1], m_RConstant(value)) &&
          matchPattern(inputs[size - 2], m_RConstant(value2))) {
        auto cst = rewriter.create<ConstantOp>(op.getLoc(), value * value2);
        SmallVector<Value, 4> newOperands(inputs.drop_back(/*n=*/2));
        newOperands.push_back(cst);
        rewriter.replaceOpWithNewOp<MulOp>(op, op.getType(), newOperands);
        return success();
      }

      // mul(a, mul(...)) -> mul(a, ...) -- flatten
      if (tryFlatteningOperands(op, rewriter))
        return success();

      return failure();
    }
  };
  results.insert<Folder>(context);
}

//===----------------------------------------------------------------------===//
// TableGen generated logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation guts for the Op classes.
#define GET_OP_CLASSES
#include "circt/Dialect/RTL/RTL.cpp.inc"
