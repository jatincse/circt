def RTLModuleOp : RTLOp<"module",
      [IsolatedFromAbove, FunctionLike, Symbol, RegionKindInterface,
       SingleBlockImplicitTerminator<"OutputOp">]> {
  let summary = "RTL Module";
  let description = [{
    The "rtl.module" operation represents a Verilog module, including a given
    name, a list of ports, and a body that represents the connections within
    the module.
  }];
  let arguments = (ins);
  let results = (outs);
  let regions = (region SizedRegion<1>:$body);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<"StringAttr name, ArrayRef<RTLModulePortInfo> ports">
  ];

  let extraClassDeclaration = [{
    using FunctionLike::getBody;

    // Implement RegionKindInterface.
    static RegionKind getRegionKind(unsigned index) { return RegionKind::Graph;}

    // Decode information about the input and output ports on this module.
    void getRTLPortInfo(SmallVectorImpl<RTLModulePortInfo> &results) {
      getRTLModulePortInfo(*this, results);
    }

    // TODO(mlir): FunctionLike shouldn't produce a getBody() helper, it is
    // squatting on the name.
    Block *getBodyBlock() { return &body().front(); }

  private:
    // This trait needs access to the hooks defined below.
    friend class OpTrait::FunctionLike<RTLModuleOp>;

    /// Returns the number of arguments, implementing OpTrait::FunctionLike.
    unsigned getNumFuncArguments() { return getType().getInputs().size(); }
    /// Returns the number of results, implementing OpTrait::FunctionLike.
    unsigned getNumFuncResults() { return getType().getResults().size(); }

    /// Hook for OpTrait::FunctionLike, called after verifying that the 'type'
    /// attribute is present and checks if it holds a function type.  Ensures
    /// getType, getNumFuncArguments, and getNumFuncResults can be called
    ///  safely.
    LogicalResult verifyType() {
      auto type = getTypeAttr().getValue();
      if (!type.isa<FunctionType>())
        return emitOpError("requires '" + getTypeAttrName() +
                           "' attribute of function type");
      return success();
    }
  public:
  }];

  let printer = [{ return ::print(p, *this); }];
  let parser = [{ return ::parse$cppClass(parser, result); }];
}

def RTLExternModuleOp : RTLOp<"externmodule",
      [IsolatedFromAbove, FunctionLike, Symbol]> {
  let summary = "RTL external Module";
  let description = [{
    The "rtl.externmodule" operation represents an external reference to a
    Verilog module, including a given name and a list of ports.
  }];
  let arguments = (ins OptionalAttr<DictionaryAttr>:$parameters);
  let results = (outs);
  let regions = (region AnyRegion:$body);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<"StringAttr name, ArrayRef<RTLModulePortInfo> ports">
  ];

  let extraClassDeclaration = [{   
    // Decode information about the input and output ports on this module.
    void getRTLPortInfo(SmallVectorImpl<RTLModulePortInfo> &results) {
      getRTLModulePortInfo(*this, results);
    }

  private:
    // This trait needs access to the hooks defined below.
    friend class OpTrait::FunctionLike<RTLExternModuleOp>;

    /// Returns the number of arguments, implementing OpTrait::FunctionLike.
    unsigned getNumFuncArguments() { return getType().getInputs().size(); }
    /// Returns the number of results, implementing OpTrait::FunctionLike.
    unsigned getNumFuncResults() { return getType().getResults().size(); }

    /// Hook for OpTrait::FunctionLike, called after verifying that the 'type'
    /// attribute is present and checks if it holds a function type.  Ensures
    /// getType, getNumFuncArguments, and getNumFuncResults can be called
    ///  safely.
    LogicalResult verifyType() {
      auto type = getTypeAttr().getValue();
      if (!type.isa<FunctionType>())
        return emitOpError("requires '" + getTypeAttrName() +
                           "' attribute of function type");
      return success();
    }
  public:
  }];

  let printer = [{ return ::print(p, *this); }];
  let parser = [{ return ::parse$cppClass(parser, result); }];
}


def RTLInstanceOp : RTLOp<"instance",
                          [DeclareOpInterfaceMethods<OpAsmOpInterface>]> {
  let summary = "Create an instance of a module";
  let description = [{
    This represents an instance of a module. The inputs and results are 
    the referenced module's inputs and outputs. 
  }];

  let arguments = (ins StrAttr:$instanceName,
                       FlatSymbolRefAttr:$moduleName,
                       Variadic<AnyType>:$inputs);
  let results = (outs Variadic<AnyType>);

  let extraClassDeclaration = [{   
    StringAttr getResultName(size_t i);
  }];
  
  let assemblyFormat = [{
    $instanceName $moduleName `(` $inputs `)` custom<ResultNames>( attr-dict )
      `:` functional-type($inputs, results)
  }];

  let verifier = [{ return ::verifyRTLInstanceOp(*this); }];
}

def OutputOp : RTLOp<"output", [Terminator, HasParent<"RTLModuleOp">,
                                NoSideEffect, ReturnLike]> {
  let summary = "RTL termination operation";
  let description = [{
    "rtl.output" marks the end of a region in the RTL dialect and the values
    to put on the output ports.
  }];

  let arguments = (ins Variadic<AnyType>:$operands);

  let builders = [OpBuilder<"", [{ build($_builder, $_state, llvm::None); }]>];

  let assemblyFormat = "attr-dict ($operands^ `:` type($operands))?";

  let verifier = [{ return ::verifyOutputOp(this); }];
}
