//===- SVFIRBuilder.cpp -- SVFIR builder-----------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2017>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * SVFIRBuilder.cpp
 *
 *  Created on: Nov 1, 2013
 *      Author: Yulei Sui
 */

#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFModule.h"
#include "Util/SVFUtil.h"
#include "SVF-LLVM/BasicTypes.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "Util/CppUtil.h"
#include "SVFIR/SVFValue.h"
#include "SVFIR/PAGBuilderFromFile.h"
#include "SVF-LLVM/LLVMLoopAnalysis.h"
#include "Util/Options.h"
#include "SVF-LLVM/CHGBuilder.h"

using namespace std;
using namespace SVF;
using namespace SVFUtil;
using namespace LLVMUtil;


/*!
 * Start building SVFIR here
 */
SVFIR* SVFIRBuilder::build()
{
    double startTime = SVFStat::getClk(true);

    DBOUT(DGENERAL, outs() << pasMsg("\t Building SVFIR ...\n"));

    // Set SVFModule from SVFIRBuilder
    pag->setModule(svfModule);

    // Build ICFG
    ICFG* icfg = new ICFG();
    ICFGBuilder icfgbuilder(icfg);
    icfgbuilder.build(svfModule);
    pag->setICFG(icfg);

    CHGraph *chg = new CHGraph(pag->getModule());
    CHGBuilder chgbuilder(chg);
    chgbuilder.buildCHG();
    pag->setCHG(chg);

    // We read SVFIR from a user-defined txt instead of parsing SVFIR from LLVM IR
    if (SVFModule::pagReadFromTXT())
    {
        PAGBuilderFromFile fileBuilder(SVFModule::pagFileName());
        return fileBuilder.build();
    }

    // If the SVFIR has been built before, then we return the unique SVFIR of the program
    if(pag->getNodeNumAfterPAGBuild() > 1)
        return pag;

    /// initial external library information
    /// initial SVFIR nodes
    initialiseNodes();
    /// initial SVFIR edges:
    ///// handle globals
    visitGlobal(svfModule);
    ///// collect exception vals in the program

    /// handle functions
    for (Module& M : LLVMModuleSet::getLLVMModuleSet()->getLLVMModules())
    {
        for (Module::const_iterator F = M.begin(), E = M.end(); F != E; ++F)
        {
            const Function& fun = *F;
            const SVFFunction* svffun = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(&fun);
            /// collect return node of function fun
            if(!fun.isDeclaration())
            {
                /// Return SVFIR node will not be created for function which can not
                /// reach the return instruction due to call to abort(), exit(),
                /// etc. In 176.gcc of SPEC 2000, function build_objc_string() from
                /// c-lang.c shows an example when fun.doesNotReturn() evaluates
                /// to TRUE because of abort().
                if(fun.doesNotReturn() == false && fun.getReturnType()->isVoidTy() == false)
                    pag->addFunRet(svffun,pag->getGNode(pag->getReturnNode(svffun)));

                /// To be noted, we do not record arguments which are in declared function without body
                /// TODO: what about external functions with SVFIR imported by commandline?
                for (Function::const_arg_iterator I = fun.arg_begin(), E = fun.arg_end();
                        I != E; ++I)
                {
                    setCurrentLocation(&*I,&fun.getEntryBlock());
                    NodeID argValNodeId = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&*I));
                    // if this is the function does not have caller (e.g. main)
                    // or a dead function, shall we create a black hole address edge for it?
                    // it is (1) too conservative, and (2) make FormalParmVFGNode defined at blackhole address PAGEdge.
                    // if(SVFUtil::ArgInNoCallerFunction(&*I)) {
                    //    if(I->getType()->isPointerTy())
                    //        addBlackHoleAddrEdge(argValNodeId);
                    //}
                    pag->addFunArgs(svffun,pag->getGNode(argValNodeId));
                }
            }
            for (Function::const_iterator bit = fun.begin(), ebit = fun.end();
                    bit != ebit; ++bit)
            {
                const BasicBlock& bb = *bit;
                for (BasicBlock::const_iterator it = bb.begin(), eit = bb.end();
                        it != eit; ++it)
                {
                    const Instruction& inst = *it;
                    setCurrentLocation(&inst,&bb);
                    visit(const_cast<Instruction&>(inst));
                }
            }
        }
    }

    sanityCheck();

    pag->initialiseCandidatePointers();

    pag->setNodeNumAfterPAGBuild(pag->getTotalNodeNum());

    // dump SVFIR
    if (Options::PAGDotGraph())
        pag->dump("svfir_initial");

    // print to command line of the SVFIR graph
    if (Options::PAGPrint())
        pag->print();

    // dump ICFG
    if (Options::DumpICFG())
        pag->getICFG()->dump("icfg_initial");

    if (Options::LoopAnalysis())
    {
        LLVMLoopAnalysis loopAnalysis;
        loopAnalysis.build(pag->getICFG());
    }

    double endTime = SVFStat::getClk(true);
    SVFStat::timeOfBuildingSVFIR = (endTime - startTime)/TIMEINTERVAL;

    return pag;
}

/*
 * Initial all the nodes from symbol table
 */
void SVFIRBuilder::initialiseNodes()
{
    DBOUT(DPAGBuild, outs() << "Initialise SVFIR Nodes ...\n");

    SymbolTableInfo* symTable = pag->getSymbolInfo();

    pag->addBlackholeObjNode();
    pag->addConstantObjNode();
    pag->addBlackholePtrNode();
    addNullPtrNode();

    for (SymbolTableInfo::ValueToIDMapTy::iterator iter =
                symTable->valSyms().begin(); iter != symTable->valSyms().end();
            ++iter)
    {
        DBOUT(DPAGBuild, outs() << "add val node " << iter->second << "\n");
        if(iter->second == symTable->blkPtrSymID() || iter->second == symTable->nullPtrSymID())
            continue;
        pag->addValNode(iter->first, iter->second);
    }

    for (SymbolTableInfo::ValueToIDMapTy::iterator iter =
                symTable->objSyms().begin(); iter != symTable->objSyms().end();
            ++iter)
    {
        DBOUT(DPAGBuild, outs() << "add obj node " << iter->second << "\n");
        if(iter->second == symTable->blackholeSymID() || iter->second == symTable->constantSymID())
            continue;
        pag->addObjNode(iter->first, iter->second);
    }

    for (SymbolTableInfo::FunToIDMapTy::iterator iter =
                symTable->retSyms().begin(); iter != symTable->retSyms().end();
            ++iter)
    {
        DBOUT(DPAGBuild, outs() << "add ret node " << iter->second << "\n");
        pag->addRetNode(iter->first, iter->second);
    }

    for (SymbolTableInfo::FunToIDMapTy::iterator iter =
                symTable->varargSyms().begin();
            iter != symTable->varargSyms().end(); ++iter)
    {
        DBOUT(DPAGBuild, outs() << "add vararg node " << iter->second << "\n");
        pag->addVarargNode(iter->first, iter->second);
    }

    /// add address edges for constant nodes.
    for (SymbolTableInfo::ValueToIDMapTy::iterator iter =
                symTable->objSyms().begin(); iter != symTable->objSyms().end(); ++iter)
    {
        DBOUT(DPAGBuild, outs() << "add address edges for constant node " << iter->second << "\n");
        const SVFValue* val = iter->first;
        if (isConstantObjSym(val))
        {
            NodeID ptr = pag->getValueNode(val);
            if(ptr!= pag->getBlkPtr() && ptr!= pag->getNullPtr())
            {
                setCurrentLocation(val, nullptr);
                addAddrEdge(iter->second, ptr);
            }
        }
    }

    assert(pag->getTotalNodeNum() >= symTable->getTotalSymNum()
           && "not all node been inititalize!!!");

}

/*
    https://github.com/SVF-tools/SVF/issues/524
    Handling single value types, for constant index, including pointer, integer, etc
    e.g. field_idx = getelementptr i8, %i8* %p, i64 -4
    We can obtain the field index by inferring the byteoffset if %p is casted from a pointer to a struct
    For another example, the following can be an array access.
    e.g. field_idx = getelementptr i8, %struct_type %p, i64 1

*/
u32_t SVFIRBuilder::inferFieldIdxFromByteOffset(const llvm::GEPOperator* gepOp, DataLayout *dl, LocationSet& ls, s32_t idx)
{
    return 0;
}

/*!
 * Return the object node offset according to GEP insn (V).
 * Given a gep edge p = q + i, if "i" is a constant then we return its offset size
 * otherwise if "i" is a variable determined by runtime, then it is a variant offset
 * Return TRUE if the offset of this GEP insn is a constant.
 */
bool SVFIRBuilder::computeGepOffset(const User *V, LocationSet& ls)
{
    assert(V);

    const llvm::GEPOperator *gepOp = SVFUtil::dyn_cast<const llvm::GEPOperator>(V);
    DataLayout * dataLayout = getDataLayout(LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule());
    llvm::APInt byteOffset(dataLayout->getIndexSizeInBits(gepOp->getPointerAddressSpace()),0,true);
    if(gepOp && dataLayout && gepOp->accumulateConstantOffset(*dataLayout,byteOffset))
    {
        //s32_t bo = byteOffset.getSExtValue();
    }

    for (bridge_gep_iterator gi = bridge_gep_begin(*V), ge = bridge_gep_end(*V);
            gi != ge; ++gi)
    {
        const Type* gepTy = *gi;
        const Value* offsetVal = gi.getOperand();
        ls.addOffsetValue(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(offsetVal), LLVMModuleSet::getLLVMModuleSet()->getSVFType(gepTy));

        //The int value of the current index operand
        const ConstantInt* op = SVFUtil::dyn_cast<ConstantInt>(offsetVal);

        // if Options::ModelConsts() is disabled. We will treat whole array as one,
        // but we can distinguish different field of an array of struct, e.g. s[1].f1 is differet from s[0].f2
        if(const ArrayType* arrTy = SVFUtil::dyn_cast<ArrayType>(gepTy))
        {
            if(!op || (arrTy->getArrayNumElements() <= (u32_t)op->getSExtValue()))
                continue;
            s32_t idx = op->getSExtValue();
            u32_t offset = pag->getSymbolInfo()->getFlattenedElemIdx(LLVMModuleSet::getLLVMModuleSet()->getSVFType(arrTy), idx);
            ls.setFldIdx(ls.accumulateConstantFieldIdx() + offset);
        }
        else if (const StructType *ST = SVFUtil::dyn_cast<StructType>(gepTy))
        {
            assert(op && "non-const offset accessing a struct");
            //The actual index
            s32_t idx = op->getSExtValue();
            u32_t offset = pag->getSymbolInfo()->getFlattenedElemIdx(LLVMModuleSet::getLLVMModuleSet()->getSVFType(ST), idx);
            ls.setFldIdx(ls.accumulateConstantFieldIdx() + offset);
        }
        else if (gepTy->isSingleValueType())
        {
            // If it's a non-constant offset access
            // If its point-to target is struct or array, it's likely an array accessing (%result = gep %struct.A* %a, i32 %non-const-index)
            // If its point-to target is single value (pointer arithmetic), then it's a variant gep (%result = gep i8* %p, i32 %non-const-index)
            if(!op && gepTy->isPointerTy() && getPtrElementType(SVFUtil::dyn_cast<PointerType>(gepTy))->isSingleValueType())
                return false;

            // The actual index
            //s32_t idx = op->getSExtValue();

            // For pointer arithmetic we ignore the byte offset
            // consider using inferFieldIdxFromByteOffset(geopOp,dataLayout,ls,idx)?
            // ls.setFldIdx(ls.accumulateConstantFieldIdx() + inferFieldIdxFromByteOffset(geopOp,idx));
        }
    }
    return true;
}

/*!
 * Handle constant expression, and connect the gep edge
 */
void SVFIRBuilder::processCE(const Value* val)
{
    if (const Constant* ref = SVFUtil::dyn_cast<Constant>(val))
    {
        if (const ConstantExpr* gepce = isGepConstantExpr(ref))
        {
            DBOUT(DPAGBuild, outs() << "handle gep constant expression " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref)->toString() << "\n");
            const Constant* opnd = gepce->getOperand(0);
            // handle recursive constant express case (gep (bitcast (gep X 1)) 1)
            processCE(opnd);
            LocationSet ls;
            bool constGep = computeGepOffset(gepce, ls);
            // must invoke pag methods here, otherwise it will be a dead recursion cycle
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(gepce, nullptr);
            /*
             * The gep edge created are like constexpr (same edge may appear at multiple callsites)
             * so bb/inst of this edge may be rewritten several times, we treat it as global here.
             */
            addGepEdge(pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(opnd)), pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(gepce)), ls, constGep);
            setCurrentLocation(cval, cbb);
        }
        else if (const ConstantExpr* castce = isCastConstantExpr(ref))
        {
            DBOUT(DPAGBuild, outs() << "handle cast constant expression " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref)->toString() << "\n");
            const Constant* opnd = castce->getOperand(0);
            processCE(opnd);
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(castce, nullptr);
            addCopyEdge(pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(opnd)), pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(castce)));
            setCurrentLocation(cval, cbb);
        }
        else if (const ConstantExpr* selectce = isSelectConstantExpr(ref))
        {
            DBOUT(DPAGBuild, outs() << "handle select constant expression " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref)->toString() << "\n");
            const Constant* src1 = selectce->getOperand(1);
            const Constant* src2 = selectce->getOperand(2);
            processCE(src1);
            processCE(src2);
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(selectce, nullptr);
            NodeID cond = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(selectce->getOperand(0)));
            NodeID nsrc1 = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(src1));
            NodeID nsrc2 = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(src2));
            NodeID nres = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(selectce));
            addSelectStmt(nres,nsrc1, nsrc2, cond);
            setCurrentLocation(cval, cbb);
        }
        // if we meet a int2ptr, then it points-to black hole
        else if (const ConstantExpr* int2Ptrce = isInt2PtrConstantExpr(ref))
        {
            addGlobalBlackHoleAddrEdge(pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(int2Ptrce)), int2Ptrce);
        }
        else if (const ConstantExpr* ptr2Intce = isPtr2IntConstantExpr(ref))
        {
            const Constant* opnd = ptr2Intce->getOperand(0);
            processCE(opnd);
            const SVFBasicBlock* cbb = getCurrentBB();
            const SVFValue* cval = getCurrentValue();
            setCurrentLocation(ptr2Intce, nullptr);
            addCopyEdge(pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(opnd)), pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ptr2Intce)));
            setCurrentLocation(cval, cbb);
        }
        else if(isTruncConstantExpr(ref) || isCmpConstantExpr(ref))
        {
            // we don't handle trunc and cmp instruction for now
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(ref, nullptr);
            NodeID dst = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref));
            addBlackHoleAddrEdge(dst);
            setCurrentLocation(cval, cbb);
        }
        else if (isBinaryConstantExpr(ref))
        {
            // we don't handle binary constant expression like add(x,y) now
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(ref, nullptr);
            NodeID dst = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref));
            addBlackHoleAddrEdge(dst);
            setCurrentLocation(cval, cbb);
        }
        else if (isUnaryConstantExpr(ref))
        {
            // we don't handle unary constant expression like fneg(x) now
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(ref, nullptr);
            NodeID dst = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref));
            addBlackHoleAddrEdge(dst);
            setCurrentLocation(cval, cbb);
        }
        else if (SVFUtil::isa<ConstantAggregate>(ref))
        {
            // we don't handle constant agrgregate like constant vectors
        }
        else if (SVFUtil::isa<BlockAddress>(ref))
        {
            // blockaddress instruction (e.g. i8* blockaddress(@run_vm, %182))
            // is treated as constant data object for now, see LLVMUtil.h:397, SymbolTableInfo.cpp:674 and SVFIRBuilder.cpp:183-194
            const SVFValue* cval = getCurrentValue();
            const SVFBasicBlock* cbb = getCurrentBB();
            setCurrentLocation(ref, nullptr);
            NodeID dst = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(ref));
            addAddrEdge(pag->getConstantNode(), dst);
            setCurrentLocation(cval, cbb);
        }
        else
        {
            if(SVFUtil::isa<ConstantExpr>(val))
                assert(false && "we don't handle all other constant expression for now!");
        }
    }
}
/*!
 * Get the field of the global variable node
 * FIXME:Here we only get the field that actually used in the program
 * We ignore the initialization of global variable field that not used in the program
 */
NodeID SVFIRBuilder::getGlobalVarField(const GlobalVariable *gvar, u32_t offset, SVFType* tpy)
{

    // if the global variable do not have any field needs to be initialized
    if (offset == 0 && gvar->getInitializer()->getType()->isSingleValueType())
    {
        return getValueNode(gvar);
    }
    /// if we did not find the constant expression in the program,
    /// then we need to create a gep node for this field
    else
    {
        return getGepValVar(gvar, LocationSet(offset), tpy);
    }
}

/*For global variable initialization
 * Give a simple global variable
 * int x = 10;     // store 10 x  (constant, non pointer)                                      |
 * int *y = &x;    // store x y   (pointer type)
 * Given a struct
 * struct Z { int s; int *t;};
 * Global initialization:
 * struct Z z = {10,&x}; // store x z.t  (struct type)
 * struct Z *m = &z;       // store z m  (pointer type)
 * struct Z n = {10,&z.s}; // store z.s n ,  &z.s constant expression (constant expression)
 */
void SVFIRBuilder::InitialGlobal(const GlobalVariable *gvar, Constant *C,
                                 u32_t offset)
{
    DBOUT(DPAGBuild, outs() << "global " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(gvar)->toString() << " constant initializer: " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(C)->toString() << "\n");
    if (C->getType()->isSingleValueType())
    {
        NodeID src = getValueNode(C);
        // get the field value if it is avaiable, otherwise we create a dummy field node.
        setCurrentLocation(gvar, nullptr);
        NodeID field = getGlobalVarField(gvar, offset, LLVMModuleSet::getLLVMModuleSet()->getSVFType(C->getType()));

        if (SVFUtil::isa<GlobalVariable, Function>(C))
        {
            setCurrentLocation(C, nullptr);
            addStoreEdge(src, field);
        }
        else if (SVFUtil::isa<ConstantExpr>(C))
        {
            // add gep edge of C1 itself is a constant expression
            processCE(C);
            setCurrentLocation(C, nullptr);
            addStoreEdge(src, field);
        }
        else if (SVFUtil::isa<BlockAddress>(C))
        {
            // blockaddress instruction (e.g. i8* blockaddress(@run_vm, %182))
            // is treated as constant data object for now, see LLVMUtil.h:397, SymbolTableInfo.cpp:674 and SVFIRBuilder.cpp:183-194
            processCE(C);
            setCurrentLocation(C, nullptr);
            addAddrEdge(pag->getConstantNode(), src);
        }
        else
        {
            setCurrentLocation(C, nullptr);
            addStoreEdge(src, field);
            /// src should not point to anything yet
            if (C->getType()->isPtrOrPtrVectorTy() && src != pag->getNullPtr())
                addCopyEdge(pag->getNullPtr(), src);
        }
    }
    else if (SVFUtil::isa<ConstantArray, ConstantStruct>(C))
    {
        if(LLVMUtil::isValVtbl(gvar) && !Options::VtableInSVFIR())
            return;
        for (u32_t i = 0, e = C->getNumOperands(); i != e; i++)
        {
            u32_t off = pag->getSymbolInfo()->getFlattenedElemIdx(LLVMModuleSet::getLLVMModuleSet()->getSVFType(C->getType()), i);
            InitialGlobal(gvar, SVFUtil::cast<Constant>(C->getOperand(i)), offset + off);
        }
    }
    else if(ConstantData* data = SVFUtil::dyn_cast<ConstantData>(C))
    {
        if(Options::ModelConsts())
        {
            if(ConstantDataSequential* seq = SVFUtil::dyn_cast<ConstantDataSequential>(data))
            {
                for(u32_t i = 0; i < seq->getNumElements(); i++)
                {
                    u32_t off = pag->getSymbolInfo()->getFlattenedElemIdx(LLVMModuleSet::getLLVMModuleSet()->getSVFType(C->getType()), i);
                    Constant* ct = seq->getElementAsConstant(i);
                    InitialGlobal(gvar, ct, offset + off);
                }
            }
            else
            {
                assert((SVFUtil::isa<ConstantAggregateZero, UndefValue>(data)) && "Single value type data should have been handled!");
            }
        }
    }
    else
    {
        //TODO:assert(SVFUtil::isa<ConstantVector>(C),"what else do we have");
    }
}

/*!
 *  Visit global variables for building SVFIR
 */
void SVFIRBuilder::visitGlobal(SVFModule* svfModule)
{

    /// initialize global variable
    for (Module &M : LLVMModuleSet::getLLVMModuleSet()->getLLVMModules())
    {
        for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I)
        {
            GlobalVariable *gvar = &*I;
            NodeID idx = getValueNode(gvar);
            NodeID obj = getObjectNode(gvar);

            setCurrentLocation(gvar, nullptr);
            addAddrEdge(obj, idx);

            if (gvar->hasInitializer())
            {
                Constant *C = gvar->getInitializer();
                DBOUT(DPAGBuild, outs() << "add global var node " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(gvar)->toString() << "\n");
                InitialGlobal(gvar, C, 0);
            }
        }


        /// initialize global functions
        for (Module::const_iterator I = M.begin(), E = M.end(); I != E; ++I)
        {
            const Function* fun = &*I;
            NodeID idx = getValueNode(fun);
            NodeID obj = getObjectNode(fun);

            DBOUT(DPAGBuild, outs() << "add global function node " << fun->getName().str() << "\n");
            setCurrentLocation(fun, nullptr);
            addAddrEdge(obj, idx);
        }

        // Handle global aliases (due to linkage of multiple bc files), e.g., @x = internal alias @y. We need to add a copy from y to x.
        for (Module::alias_iterator I = M.alias_begin(), E = M.alias_end(); I != E; I++)
        {
            const GlobalAlias* alias = &*I;
            NodeID dst = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(alias));
            NodeID src = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(alias->getAliasee()));
            processCE(alias->getAliasee());
            setCurrentLocation(alias, nullptr);
            addCopyEdge(src,dst);
        }
    }
}

/*!
 * Visit alloca instructions
 * Add edge V (dst) <-- O (src), V here is a value node on SVFIR, O is object node on SVFIR
 */
void SVFIRBuilder::visitAllocaInst(AllocaInst &inst)
{

    // AllocaInst should always be a pointer type
    assert(SVFUtil::isa<PointerType>(inst.getType()));

    DBOUT(DPAGBuild, outs() << "process alloca  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");
    NodeID dst = getValueNode(&inst);

    NodeID src = getObjectNode(&inst);

    addAddrEdge(src, dst);

}

/*!
 * Visit phi instructions
 */
void SVFIRBuilder::visitPHINode(PHINode &inst)
{

    DBOUT(DPAGBuild, outs() << "process phi " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << "  \n");

    NodeID dst = getValueNode(&inst);

    for (u32_t i = 0; i < inst.getNumIncomingValues(); ++i)
    {
        const Value* val = inst.getIncomingValue(i);
        const Instruction* incomingInst = SVFUtil::dyn_cast<Instruction>(val);
        bool matched = (incomingInst == nullptr ||
                        incomingInst->getFunction() == inst.getFunction());
        (void) matched; // Suppress warning of unused variable under release build
        assert(matched && "incomingInst's Function incorrect");
        const Instruction* predInst = &inst.getIncomingBlock(i)->back();
        const SVFInstruction* svfPrevInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(predInst);
        const ICFGNode* icfgNode = pag->getICFG()->getICFGNode(svfPrevInst);
        NodeID src = getValueNode(val);
        addPhiStmt(dst,src,icfgNode);
    }
}

/*
 * Visit load instructions
 */
void SVFIRBuilder::visitLoadInst(LoadInst &inst)
{
    DBOUT(DPAGBuild, outs() << "process load  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");

    NodeID dst = getValueNode(&inst);

    NodeID src = getValueNode(inst.getPointerOperand());

    addLoadEdge(src, dst);
}

/*!
 * Visit store instructions
 */
void SVFIRBuilder::visitStoreInst(StoreInst &inst)
{
    // StoreInst itself should always not be a pointer type
    assert(!SVFUtil::isa<PointerType>(inst.getType()));

    DBOUT(DPAGBuild, outs() << "process store " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");

    NodeID dst = getValueNode(inst.getPointerOperand());

    NodeID src = getValueNode(inst.getValueOperand());

    addStoreEdge(src, dst);

}

/*!
 * Visit getelementptr instructions
 */
void SVFIRBuilder::visitGetElementPtrInst(GetElementPtrInst &inst)
{

    NodeID dst = getValueNode(&inst);
    // GetElementPtrInst should always be a pointer or a vector contains pointers
    // for now we don't handle vector type here
    if(SVFUtil::isa<VectorType>(inst.getType()))
    {
        addBlackHoleAddrEdge(dst);
        return;
    }

    assert(SVFUtil::isa<PointerType>(inst.getType()));

    DBOUT(DPAGBuild, outs() << "process gep  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");

    NodeID src = getValueNode(inst.getPointerOperand());

    LocationSet ls;
    bool constGep = computeGepOffset(&inst, ls);
    addGepEdge(src, dst, ls, constGep);
}

/*
 * Visit cast instructions
 */
void SVFIRBuilder::visitCastInst(CastInst &inst)
{

    DBOUT(DPAGBuild, outs() << "process cast  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");
    NodeID dst = getValueNode(&inst);

    if (SVFUtil::isa<IntToPtrInst>(&inst))
    {
        addBlackHoleAddrEdge(dst);
    }
    else
    {
        const Value*  opnd = inst.getOperand(0);
        if (!SVFUtil::isa<PointerType>(opnd->getType()))
            opnd = stripAllCasts(opnd);

        NodeID src = getValueNode(opnd);
        addCopyEdge(src, dst);
    }
}

/*!
 * Visit Binary Operator
 */
void SVFIRBuilder::visitBinaryOperator(BinaryOperator &inst)
{
    NodeID dst = getValueNode(&inst);
    assert(inst.getNumOperands() == 2 && "not two operands for BinaryOperator?");
    Value* op1 = inst.getOperand(0);
    NodeID op1Node = getValueNode(op1);
    Value* op2 = inst.getOperand(1);
    NodeID op2Node = getValueNode(op2);
    u32_t opcode = inst.getOpcode();
    addBinaryOPEdge(op1Node, op2Node, dst, opcode);
}

/*!
 * Visit Unary Operator
 */
void SVFIRBuilder::visitUnaryOperator(UnaryOperator &inst)
{
    NodeID dst = getValueNode(&inst);
    assert(inst.getNumOperands() == 1 && "not one operand for Unary instruction?");
    Value* opnd = inst.getOperand(0);
    NodeID src = getValueNode(opnd);
    u32_t opcode = inst.getOpcode();
    addUnaryOPEdge(src, dst, opcode);
}

/*!
 * Visit compare instruction
 */
void SVFIRBuilder::visitCmpInst(CmpInst &inst)
{
    NodeID dst = getValueNode(&inst);
    assert(inst.getNumOperands() == 2 && "not two operands for compare instruction?");
    Value* op1 = inst.getOperand(0);
    NodeID op1Node = getValueNode(op1);
    Value* op2 = inst.getOperand(1);
    NodeID op2Node = getValueNode(op2);
    u32_t predicate = inst.getPredicate();
    addCmpEdge(op1Node, op2Node, dst, predicate);
}


/*!
 * Visit select instructions
 */
void SVFIRBuilder::visitSelectInst(SelectInst &inst)
{

    DBOUT(DPAGBuild, outs() << "process select  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");

    NodeID dst = getValueNode(&inst);
    NodeID src1 = getValueNode(inst.getTrueValue());
    NodeID src2 = getValueNode(inst.getFalseValue());
    NodeID cond = getValueNode(inst.getCondition());
    /// Two operands have same incoming basic block, both are the current BB
    addSelectStmt(dst,src1,src2, cond);
}

void SVFIRBuilder::visitCallInst(CallInst &i)
{
    visitCallSite(&i);
}

void SVFIRBuilder::visitInvokeInst(InvokeInst &i)
{
    visitCallSite(&i);
}

void SVFIRBuilder::visitCallBrInst(CallBrInst &i)
{
    visitCallSite(&i);
}

/*
 * Visit callsites
 */
void SVFIRBuilder::visitCallSite(CallBase* cs)
{

    // skip llvm intrinsics
    if(isIntrinsicInst(cs))
        return;

    const SVFInstruction* svfcall = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(cs);

    DBOUT(DPAGBuild,
          outs() << "process callsite " << svfcall->toString() << "\n");

    CallICFGNode* callBlockNode = pag->getICFG()->getCallICFGNode(svfcall);
    RetICFGNode* retBlockNode = pag->getICFG()->getRetICFGNode(svfcall);

    pag->addCallSite(callBlockNode);

    /// Collect callsite arguments and returns
    for (u32_t i = 0; i < cs->arg_size(); i++)
        pag->addCallSiteArgs(callBlockNode,pag->getGNode(getValueNode(cs->getArgOperand(i))));

    if(!cs->getType()->isVoidTy())
        pag->addCallSiteRets(retBlockNode,pag->getGNode(getValueNode(cs)));

    if (const Function *callee = LLVMUtil::getCallee(cs))
    {
        const SVFFunction* svfcallee = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(callee);
        if (isExtCall(svfcallee))
        {
            // There is no extpag for the function, use the old method.
            handleExtCall(cs, callee);
        }
        else
        {
            handleDirectCall(cs, callee);
        }
    }
    else
    {
        //If the callee was not identified as a function (null F), this is indirect.
        handleIndCall(cs);
    }
}

/*!
 * Visit return instructions of a function
 */
void SVFIRBuilder::visitReturnInst(ReturnInst &inst)
{

    // ReturnInst itself should always not be a pointer type
    assert(!SVFUtil::isa<PointerType>(inst.getType()));

    DBOUT(DPAGBuild, outs() << "process return  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(&inst)->toString() << " \n");

    if(Value* src = inst.getReturnValue())
    {
        const SVFFunction *F = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(inst.getParent()->getParent());

        NodeID rnF = getReturnNode(F);
        NodeID vnS = getValueNode(src);
        const SVFInstruction* svfInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(&inst);
        const ICFGNode* icfgNode = pag->getICFG()->getICFGNode(svfInst);
        //vnS may be null if src is a null ptr
        addPhiStmt(rnF,vnS,icfgNode);
    }
}


/*!
 * visit extract value instructions for structures in registers
 * TODO: for now we just assume the pointer after extraction points to blackhole
 * for example %24 = extractvalue { i32, %struct.s_hash* } %call34, 0
 * %24 is a pointer points to first field of a register value %call34
 * however we can not create %call34 as an memory object, as it is register value.
 * Is that necessary treat extract value as getelementptr instruction later to get more precise results?
 */
void SVFIRBuilder::visitExtractValueInst(ExtractValueInst  &inst)
{
    NodeID dst = getValueNode(&inst);
    addBlackHoleAddrEdge(dst);
}

/*!
 * The �extractelement� instruction extracts a single scalar element from a vector at a specified index.
 * TODO: for now we just assume the pointer after extraction points to blackhole
 * The first operand of an �extractelement� instruction is a value of vector type.
 * The second operand is an index indicating the position from which to extract the element.
 *
 * <result> = extractelement <4 x i32> %vec, i32 0    ; yields i32
 */
void SVFIRBuilder::visitExtractElementInst(ExtractElementInst &inst)
{
    NodeID dst = getValueNode(&inst);
    addBlackHoleAddrEdge(dst);
}

/*!
 * Branch and switch instructions are treated as UnaryOP
 * br %cmp label %if.then, label %if.else
 */
void SVFIRBuilder::visitBranchInst(BranchInst &inst)
{
    NodeID brinst = getValueNode(&inst);
    NodeID cond;
    if (inst.isConditional())
        cond = getValueNode(inst.getCondition());
    else
        cond = pag->getNullPtr();

    assert(inst.getNumSuccessors() <= 2 && "if/else has more than two branches?");

    BranchStmt::SuccAndCondPairVec successors;
    for (u32_t i = 0; i < inst.getNumSuccessors(); ++i)
    {
        const Instruction* succInst = &inst.getSuccessor(i)->front();
        const SVFInstruction* svfSuccInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(succInst);
        const ICFGNode* icfgNode = pag->getICFG()->getICFGNode(svfSuccInst);
        successors.push_back(std::make_pair(icfgNode, 1-i));
    }
    addBranchStmt(brinst, cond,successors);
}

void SVFIRBuilder::visitSwitchInst(SwitchInst &inst)
{
    NodeID brinst = getValueNode(&inst);
    NodeID cond = getValueNode(inst.getCondition());

    BranchStmt::SuccAndCondPairVec successors;
    for (u32_t i = 0; i < inst.getNumSuccessors(); ++i)
    {
        const Instruction* succInst = &inst.getSuccessor(i)->front();
        const ConstantInt* condVal = inst.findCaseDest(inst.getSuccessor(i));
        /// default case is set to -1;
        s64_t val = -1;
        if (condVal && condVal->getBitWidth() <= 64)
            val = condVal->getSExtValue();
        const SVFInstruction* svfSuccInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(succInst);
        const ICFGNode* icfgNode = pag->getICFG()->getICFGNode(svfSuccInst);
        successors.push_back(std::make_pair(icfgNode,val));
    }
    addBranchStmt(brinst, cond,successors);
}

///   %ap = alloca %struct.va_list
///  %ap2 = bitcast %struct.va_list* %ap to i8*
/// ; Read a single integer argument from %ap2
/// %tmp = va_arg i8* %ap2, i32 (VAArgInst)
/// TODO: for now, create a copy edge from %ap2 to %tmp, we assume here %tmp should point to the n-th argument of the var_args
void SVFIRBuilder::visitVAArgInst(VAArgInst &inst)
{
    NodeID dst = getValueNode(&inst);
    Value* opnd = inst.getPointerOperand();
    NodeID src = getValueNode(opnd);
    addCopyEdge(src,dst);
}

/// <result> = freeze ty <val>
/// If <val> is undef or poison, ‘freeze’ returns an arbitrary, but fixed value of type `ty`
/// Otherwise, this instruction is a no-op and returns the input <val>
/// For now, we assume <val> is never a posion or undef.
void SVFIRBuilder::visitFreezeInst(FreezeInst &inst)
{
    NodeID dst = getValueNode(&inst);
    for (u32_t i = 0; i < inst.getNumOperands(); i++)
    {
        Value* opnd = inst.getOperand(i);
        NodeID src = getValueNode(opnd);
        addCopyEdge(src,dst);
    }
}


/*!
 * Add the constraints for a direct, non-external call.
 */
void SVFIRBuilder::handleDirectCall(CallBase* cs, const Function *F)
{

    assert(F);
    const SVFInstruction* svfcall = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(cs);
    const SVFFunction* svffun = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(F);
    DBOUT(DPAGBuild,
          outs() << "handle direct call " << svfcall->toString() << " callee " << F->getName().str() << "\n");

    //Only handle the ret.val. if it's used as a ptr.
    NodeID dstrec = getValueNode(cs);
    //Does it actually return a ptr?
    if (!cs->getType()->isVoidTy())
    {
        NodeID srcret = getReturnNode(svffun);
        CallICFGNode* callICFGNode = pag->getICFG()->getCallICFGNode(svfcall);
        FunExitICFGNode* exitICFGNode = pag->getICFG()->getFunExitICFGNode(svffun);
        addRetEdge(srcret, dstrec,callICFGNode, exitICFGNode);
    }
    //Iterators for the actual and formal parameters
    u32_t itA = 0, ieA = cs->arg_size();
    Function::const_arg_iterator itF = F->arg_begin(), ieF = F->arg_end();
    //Go through the fixed parameters.
    DBOUT(DPAGBuild, outs() << "      args:");
    for (; itF != ieF; ++itA, ++itF)
    {
        //Some programs (e.g. Linux kernel) leave unneeded parameters empty.
        if (itA == ieA)
        {
            DBOUT(DPAGBuild, outs() << " !! not enough args\n");
            break;
        }
        const Value* AA = cs->getArgOperand(itA), *FA = &*itF; //current actual/formal arg

        DBOUT(DPAGBuild, outs() << "process actual parm  " << LLVMModuleSet::getLLVMModuleSet()->getSVFValue(AA)->toString() << " \n");

        NodeID dstFA = getValueNode(FA);
        NodeID srcAA = getValueNode(AA);
        CallICFGNode* icfgNode = pag->getICFG()->getCallICFGNode(svfcall);
        FunEntryICFGNode* entry = pag->getICFG()->getFunEntryICFGNode(svffun);
        addCallEdge(srcAA, dstFA, icfgNode, entry);
    }
    //Any remaining actual args must be varargs.
    if (F->isVarArg())
    {
        NodeID vaF = getVarargNode(svffun);
        DBOUT(DPAGBuild, outs() << "\n      varargs:");
        for (; itA != ieA; ++itA)
        {
            const Value* AA = cs->getArgOperand(itA);
            NodeID vnAA = getValueNode(AA);
            CallICFGNode* icfgNode = pag->getICFG()->getCallICFGNode(svfcall);
            FunEntryICFGNode* entry = pag->getICFG()->getFunEntryICFGNode(svffun);
            addCallEdge(vnAA,vaF, icfgNode,entry);
        }
    }
    if(itA != ieA)
    {
        /// FIXME: this assertion should be placed for correct checking except
        /// bug program like 188.ammp, 300.twolf
        writeWrnMsg("too many args to non-vararg func.");
        writeWrnMsg("(" + svfcall->getSourceLoc() + ")");

    }
}

const Value* SVFIRBuilder::getBaseValueForExtArg(const Value* V)
{
    const Value*  value = stripAllCasts(V);
    assert(value && "null ptr?");
    if(const GetElementPtrInst* gep = SVFUtil::dyn_cast<GetElementPtrInst>(value))
    {
        s32_t totalidx = 0;
        for (bridge_gep_iterator gi = bridge_gep_begin(gep), ge = bridge_gep_end(gep); gi != ge; ++gi)
        {
            if(const ConstantInt* op = SVFUtil::dyn_cast<ConstantInt>(gi.getOperand()))
                totalidx += op->getSExtValue();
        }
        if(totalidx == 0 && !SVFUtil::isa<StructType>(value->getType()))
            value = gep->getPointerOperand();
    }

    // if the argument of memcpy is the result of an allocation (1) or a casted load instruction (2),
    // further steps are necessary to find the correct base value
    //
    // (1)
    // %call   = malloc 80
    // %0      = bitcast i8* %call to %struct.A*
    // %1      = bitcast %struct.B* %param to i8*
    // call void memcpy(%call, %1, 80)
    //
    // (2)
    // %0 = bitcast %struct.A* %param to i8*
    // %2 = bitcast %struct.B** %arrayidx to i8**
    // %3 = load i8*, i8** %2
    // call void @memcpy(%0, %3, 80)
    LLVMContext &cxt = LLVMModuleSet::getLLVMModuleSet()->getContext();
    if (value->getType() == PointerType::getInt8PtrTy(cxt))
    {
        // (1)
        if (const CallBase* cb = SVFUtil::dyn_cast<CallBase>(value))
        {
            const SVFInstruction* svfInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(cb);
            if (SVFUtil::isHeapAllocExtCallViaRet(svfInst))
            {
                if (const Value* bitCast = getUniqueUseViaCastInst(cb))
                    return bitCast;
            }
        }
        // (2)
        else if (const LoadInst* load = SVFUtil::dyn_cast<LoadInst>(value))
        {
            if (const BitCastInst* bitCast = SVFUtil::dyn_cast<BitCastInst>(load->getPointerOperand()))
                return bitCast->getOperand(0);
        }
    }

    return value;
}

/*!
 * Find the base type and the max possible offset of an object pointed to by (V).
 */
const Type* SVFIRBuilder::getBaseTypeAndFlattenedFields(const Value* V, std::vector<LocationSet> &fields, const Value* szValue)
{
    assert(V);
    const Value* value = getBaseValueForExtArg(V);
    const Type* T = value->getType();
    while (const PointerType *ptype = SVFUtil::dyn_cast<PointerType>(T))
        T = getPtrElementType(ptype);

    u32_t numOfElems = pag->getSymbolInfo()->getNumOfFlattenElements(LLVMModuleSet::getLLVMModuleSet()->getSVFType(T));
    /// use user-specified size for this copy operation if the size is a constaint int
    if(szValue && SVFUtil::isa<ConstantInt>(szValue))
    {
        numOfElems = (numOfElems > SVFUtil::cast<ConstantInt>(szValue)->getSExtValue()) ? SVFUtil::cast<ConstantInt>(szValue)->getSExtValue() : numOfElems;
    }

    LLVMContext& context = LLVMModuleSet::getLLVMModuleSet()->getContext();
    for(u32_t ei = 0; ei < numOfElems; ei++)
    {
        LocationSet ls(ei);
        // make a ConstantInt and create char for the content type due to byte-wise copy
        const ConstantInt* offset = ConstantInt::get(context, llvm::APInt(32, ei));
        ls.addOffsetValue(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(offset), nullptr);
        fields.push_back(ls);
    }
    return T;
}

/*!
 * Add the load/store constraints and temp. nodes for the complex constraint
 * *D = *S (where D/S may point to structs).
 */
void SVFIRBuilder::addComplexConsForExt(const Value* D, const Value* S, const Value* szValue)
{
    assert(D && S);
    NodeID vnD= getValueNode(D), vnS= getValueNode(S);
    if(!vnD || !vnS)
        return;

    std::vector<LocationSet> fields;

    //Get the max possible size of the copy, unless it was provided.
    std::vector<LocationSet> srcFields;
    std::vector<LocationSet> dstFields;
    const Type* stype = getBaseTypeAndFlattenedFields(S, srcFields, szValue);
    const Type* dtype = getBaseTypeAndFlattenedFields(D, dstFields, szValue);
    if(srcFields.size() > dstFields.size())
        fields = dstFields;
    else
        fields = srcFields;

    /// If sz is 0, we will add edges for all fields.
    u32_t sz = fields.size();

    if (fields.size() == 1 && (LLVMUtil::isConstDataOrAggData(D) || LLVMUtil::isConstDataOrAggData(S)))
    {
        NodeID dummy = pag->addDummyValNode();
        addLoadEdge(vnD,dummy);
        addStoreEdge(dummy,vnS);
        return;
    }

    //For each field (i), add (Ti = *S + i) and (*D + i = Ti).
    for (u32_t index = 0; index < sz; index++)
    {
        LLVMModuleSet* llvmmodule = LLVMModuleSet::getLLVMModuleSet();
        const SVFType* dElementType = pag->getSymbolInfo()->getFlatternedElemType(llvmmodule->getSVFType(dtype), fields[index].accumulateConstantFieldIdx());
        const SVFType* sElementType = pag->getSymbolInfo()->getFlatternedElemType(llvmmodule->getSVFType(stype), fields[index].accumulateConstantFieldIdx());
        NodeID dField = getGepValVar(D,fields[index],dElementType);
        NodeID sField = getGepValVar(S,fields[index],sElementType);
        NodeID dummy = pag->addDummyValNode();
        addLoadEdge(sField,dummy);
        addStoreEdge(dummy,dField);
    }
}

void SVFIRBuilder::parseOperations(std::vector<ExtAPI::Operation>  &operations, CallBase* cs)
{
    // Record all dummy nodes
    std::map<std::string, NodeID> nodeIDMap;
    for (ExtAPI::Operation& operation : operations)
    {
        std::vector<NodeID>& operands = operation.getOperands();
        if (operation.getOperator() == "funptr_ops" || operation.getOperator() == "Rb_tree_ops")
            continue;
        for (std::string s: operation.getOperandStr())
        {
            // There is already a NodeID in nodeIDMap
            if (nodeIDMap.find(s) != nodeIDMap.end())
                operands.push_back(nodeIDMap[s]);
            else
            {
                s32_t nodeIDType = ExtAPI::getExtAPI()->getNodeIDType(s);
                if (nodeIDType >= 0)
                {
                    if( cs->arg_size() <= (u32_t) nodeIDType)
                        assert(false && "Argument out of bounds!");
                    else if (operation.getOperator() == "memcpy_like" || operation.getOperator() == "memset_like")
                    {
                        operands.push_back(nodeIDType);
                        nodeIDMap[s] = nodeIDType;
                    }
                    else
                    {
                        operands.push_back(getValueNode(cs->getArgOperand(nodeIDType)));
                        nodeIDMap[s] = getValueNode(cs->getArgOperand(nodeIDType));
                    }
                }
                else if (nodeIDType == -1)
                {
                    operands.push_back(getValueNode(cs));
                    nodeIDMap[s] = getValueNode(cs);
                }
                else if (nodeIDType == -2)
                {
                    operands.push_back(pag->addDummyValNode());
                    nodeIDMap[s] = operands[operands.size() - 1];
                }
                else if (nodeIDType == -3)
                {
                    if (SVFUtil::isa<PointerType>(cs->getType()))
                    {
                        operands.push_back(getObjectNode(cs));
                        nodeIDMap[s] = getObjectNode(cs);
                    }
                }
                else if (nodeIDType == -4)
                {
                    operands.push_back(pag->getNullPtr());
                    nodeIDMap[s] = operands[operands.size() - 1];
                }
                else if (nodeIDType == -5)
                {
                    for (char const &c : s)
                    {
                        if (std::isdigit(c) == 0)
                            assert(false && "Invalid offset!");
                    }
                    operands.push_back(atoi(s.c_str()));
                    nodeIDMap[s] = atoi(s.c_str());
                }
                else
                    assert(false && "The operand format of function operation is illegal!");
            }
        }
    }
}

/*!
 * Handle external calls
 */
void SVFIRBuilder::handleExtCall(CallBase* cs, const Function *callee)
{
    const SVFInstruction* svfinst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(cs);
    const SVFFunction* svfcallee = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(callee);

    if (isHeapAllocOrStaticExtCall(svfinst))
    {
        // case 1: ret = new obj
        if (isHeapAllocExtCallViaRet(svfinst) || isStaticExtCall(svfinst))
        {
            NodeID val = getValueNode(cs);
            NodeID obj = getObjectNode(cs);
            addAddrEdge(obj, val);
        }
        // case 2: *arg = new obj
        else
        {
            assert(isHeapAllocExtCallViaArg(svfinst) && "Must be heap alloc call via arg.");
            u32_t arg_pos = getHeapAllocHoldingArgPosition(svfcallee);
            const Value* arg = cs->getArgOperand(arg_pos);
            if (arg->getType()->isPointerTy())
            {
                NodeID vnArg = getValueNode(arg);
                NodeID dummy = pag->addDummyValNode();
                NodeID obj = pag->addDummyObjNode(LLVMModuleSet::getLLVMModuleSet()->getSVFType(arg->getType()));
                if (vnArg && dummy && obj)
                {
                    addAddrEdge(obj, dummy);
                    addStoreEdge(dummy, vnArg);
                }
            }
            else
            {
                writeWrnMsg("Arg receiving new object must be pointer type");
            }
        }
    }
    else
    {
        if (isExtCall(svfcallee))
        {
            std::string funName = ExtAPI::getExtAPI()->get_name(svfcallee);
            std::vector<ExtAPI::Operation>  allOperations = ExtAPI::getExtAPI()->getAllOperations(funName);
            if (allOperations.size() == 0)
            {
                std::string str;
                std::stringstream rawstr(str);
                rawstr << "function " << callee->getName().str() << " not in the external function summary ExtAPI.json file";
                writeWrnMsg(rawstr.str());
            }
            else
            {
                parseOperations(allOperations, cs);
                for (ExtAPI::Operation op : allOperations)
                {
                    if (op.getOperator() == "AddrStmt")
                    {
                        if (op.getOperands().size() == 2)
                            addAddrEdge(op.getOperands()[0], op.getOperands()[1]);
                        else
                            writeWrnMsg("We need two valid NodeIDs to add an Addr edge");
                    }
                    else if (op.getOperator() == "CopyStmt")
                    {
                        if (op.getOperands().size() == 2)
                            addCopyEdge(op.getOperands()[0], op.getOperands()[1]);
                        else
                            writeWrnMsg("We need two valid NodeIDs to add a Copy edge");
                    }
                    else if (op.getOperator() == "LoadStmt")
                    {
                        if (op.getOperands().size() == 2)
                            addLoadEdge(op.getOperands()[0], op.getOperands()[1]);
                        else
                            writeWrnMsg("We need two valid NodeIDs to add a Load edge");
                    }
                    else if (op.getOperator() == "StoreStmt")
                    {
                        if (op.getOperands().size() == 2)
                            addStoreEdge(op.getOperands()[0], op.getOperands()[1]);
                        else
                            writeWrnMsg("We need two valid NodeIDs to add a Store edge");
                    }
                    else if (op.getOperator() == "GepStmt")
                    {
                        if (op.getOperands().size() == 3)
                        {
                            LocationSet ls(op.getOperands()[2]);
                            addNormalGepEdge(op.getOperands()[0], op.getOperands()[1], ls);
                        }
                        else
                            writeWrnMsg("We need two valid NodeIDs and an offset to add a Gep edge");
                    }
                    else if (op.getOperator() == "BinaryOPStmt")
                    {
                        if (op.getOperands().size() == 4)
                            addBinaryOPEdge(op.getOperands()[0], op.getOperands()[1], op.getOperands()[2], op.getOperands()[3]);
                        else
                            writeWrnMsg("We need four valid NodeIDs to add a BinaryOP edge");
                    }
                    else if (op.getOperator() == "UnaryOPStmt")
                    {
                        if (op.getOperands().size() == 3)
                            addUnaryOPEdge(op.getOperands()[0], op.getOperands()[1], op.getOperands()[2]);
                        else
                            writeWrnMsg("We need three valid NodeIDs to add a UnaryOP edge");
                    }
                    else if (op.getOperator() == "CmpStmt")
                    {
                        if (op.getOperands().size() == 4)
                            addCmpEdge(op.getOperands()[0], op.getOperands()[1], op.getOperands()[2], op.getOperands()[3]);
                        else
                            writeWrnMsg("We need four valid NodeIDs to add a CmpStmt edge");
                    }
                    else if (op.getOperator() == "memset_like")
                    {
                        // this is for memset(void *str, int c, size_t n)
                        // which copies the character c (an unsigned char) to the first n characters of the string pointed to, by the argument str
                        std::vector<LocationSet> dstFields;
                        const Type* dtype = getBaseTypeAndFlattenedFields(cs->getArgOperand(op.getOperands()[0]), dstFields, cs->getArgOperand(op.getOperands()[2]));
                        u32_t sz = dstFields.size();
                        //For each field (i), add store edge *(arg0 + i) = arg1
                        for (u32_t index = 0; index < sz; index++)
                        {
                            const SVFType* dElementType = pag->getSymbolInfo()->getFlatternedElemType(LLVMModuleSet::getLLVMModuleSet()->getSVFType(dtype), dstFields[index].accumulateConstantFieldIdx());
                            NodeID dField = getGepValVar(cs->getArgOperand(op.getOperands()[0]), dstFields[index], dElementType);
                            addStoreEdge(getValueNode(cs->getArgOperand(op.getOperands()[1])),dField);
                        }
                        if(SVFUtil::isa<PointerType>(cs->getType()))
                            addCopyEdge(getValueNode(cs->getArgOperand(op.getOperands()[0])), getValueNode(cs));
                    }
                    else if (op.getOperator() == "memcpy_like")
                    {
                        /// handle strcpy
                        if(op.getOperands().size() == 3)
                            addComplexConsForExt(cs->getArgOperand(op.getOperands()[0]), cs->getArgOperand(op.getOperands()[1]), cs->getArgOperand(op.getOperands()[2]));
                        else
                            addComplexConsForExt(cs->getArgOperand(op.getOperands()[0]), cs->getArgOperand(op.getOperands()[1]), nullptr);
                    }
                    else if (op.getOperator() == "funptr_ops")
                    {
                        /// handling external function e.g., void *dlsym(void *handle, const char *funname);
                        const Value* src = cs->getArgOperand(1);
                        if(const GetElementPtrInst* gep = SVFUtil::dyn_cast<GetElementPtrInst>(src))
                            src = stripConstantCasts(gep->getPointerOperand());
                        if(const GlobalVariable* glob = SVFUtil::dyn_cast<GlobalVariable>(src))
                        {
                            if(const ConstantDataArray* constarray = SVFUtil::dyn_cast<ConstantDataArray>(glob->getInitializer()))
                            {
                                if(const Function* fun = LLVMUtil::getProgFunction(constarray->getAsCString().str()))
                                {
                                    NodeID srcNode = getValueNode(fun);
                                    addCopyEdge(srcNode,  getValueNode(cs));
                                }
                            }
                        }
                    }
                    else if (op.getOperator() == "Rb_tree_ops")
                    {
                        assert(cs->arg_size() == 4 && "_Rb_tree_insert_and_rebalance should have 4 arguments.\n");

                        const Value* vArg1 = cs->getArgOperand(1);
                        const Value* vArg3 = cs->getArgOperand(3);

                        // We have vArg3 points to the entry of _Rb_tree_node_base { color; parent; left; right; }.
                        // Now we calculate the offset from base to vArg3
                        NodeID vnArg3 = pag->getValueNode(LLVMModuleSet::getLLVMModuleSet()->getSVFValue(vArg3));
                        s32_t offset = getLocationSetFromBaseNode(vnArg3).accumulateConstantFieldIdx();

                        // We get all flattened fields of base
                        vector<LocationSet> fields;
                        const Type* type = getBaseTypeAndFlattenedFields(vArg3, fields, nullptr);

                        // We summarize the side effects: arg3->parent = arg1, arg3->left = arg1, arg3->right = arg1
                        // Note that arg0 is aligned with "offset".
                        for (s32_t i = offset + 1; i <= offset + 3; ++i)
                        {
                            if((u32_t)i >= fields.size())
                                break;
                            const SVFType* elementType = pag->getSymbolInfo()->getFlatternedElemType(LLVMModuleSet::getLLVMModuleSet()->getSVFType(type), fields[i].accumulateConstantFieldIdx());
                            NodeID vnD = getGepValVar(vArg3, fields[i], elementType);
                            NodeID vnS = getValueNode(vArg1);
                            if(vnD && vnS)
                                addStoreEdge(vnS,vnD);
                        }
                    }
                    // default
                    // illegal function operation of external function
                    else
                    {
                        assert(false && "new type of SVFStmt for external calls?");
                    }
                }
            }
        }

        /// create inter-procedural SVFIR edges for thread forks
        if (isThreadForkCall(svfinst))
        {
            if (const SVFFunction* forkedFun = SVFUtil::dyn_cast<SVFFunction>(getForkedFun(svfinst)))
            {
                forkedFun = forkedFun->getDefFunForMultipleModule();
                const SVFValue* actualParm = getActualParmAtForkSite(svfinst);
                /// pthread_create has 1 arg.
                /// apr_thread_create has 2 arg.
                assert((forkedFun->arg_size() <= 2) && "Size of formal parameter of start routine should be one");
                if (forkedFun->arg_size() <= 2 && forkedFun->arg_size() >= 1)
                {
                    const SVFArgument* formalParm = forkedFun->getArg(0);
                    /// Connect actual parameter to formal parameter of the start routine
                    if (actualParm->getType()->isPointerTy() && formalParm->getType()->isPointerTy())
                    {
                        CallICFGNode *icfgNode = pag->getICFG()->getCallICFGNode(svfinst);
                        FunEntryICFGNode *entry = pag->getICFG()->getFunEntryICFGNode(forkedFun);
                        addThreadForkEdge(pag->getValueNode(actualParm), pag->getValueNode(formalParm), icfgNode, entry);
                    }
                }
            }
            else
            {
                /// handle indirect calls at pthread create APIs e.g., pthread_create(&t1, nullptr, fp, ...);
                /// const Value* fun = ThreadAPI::getThreadAPI()->getForkedFun(inst);
                /// if(!SVFUtil::isa<Function>(fun))
                ///    pag->addIndirectCallsites(cs,pag->getValueNode(fun));
            }
            /// If forkedFun does not pass to spawnee as function type but as void pointer
            /// remember to update inter-procedural callgraph/SVFIR/SVFG etc. when indirect call targets are resolved
            /// We don't connect the callgraph here, further investigation is need to hanle mod-ref during SVFG construction.
        }

        /// create inter-procedural SVFIR edges for hare_parallel_for calls
        else if (isHareParForCall(svfinst))
        {
            if (const SVFFunction* taskFunc = SVFUtil::dyn_cast<SVFFunction>(getTaskFuncAtHareParForSite(svfinst)))
            {
                /// The task function of hare_parallel_for has 3 args.
                assert((taskFunc->arg_size() == 3) && "Size of formal parameter of hare_parallel_for's task routine should be 3");
                const SVFValue* actualParm = getTaskDataAtHareParForSite(svfinst);
                const SVFArgument* formalParm = taskFunc->getArg(0);
                /// Connect actual parameter to formal parameter of the start routine
                if (actualParm->getType()->isPointerTy() && formalParm->getType()->isPointerTy())
                {
                    CallICFGNode *icfgNode = pag->getICFG()->getCallICFGNode(svfinst);
                    FunEntryICFGNode *entry = pag->getICFG()->getFunEntryICFGNode(taskFunc);
                    addThreadForkEdge(pag->getValueNode(actualParm), pag->getValueNode(formalParm), icfgNode, entry);
                }
            }
            else
            {
                /// handle indirect calls at hare_parallel_for (e.g., hare_parallel_for(..., fp, ...);
                /// const Value* fun = ThreadAPI::getThreadAPI()->getForkedFun(inst);
                /// if(!SVFUtil::isa<Function>(fun))
                ///    pag->addIndirectCallsites(cs,pag->getValueNode(fun));
            }
        }

        /// TODO: inter-procedural SVFIR edges for thread joins
    }
}

/*!
 * Indirect call is resolved on-the-fly during pointer analysis
 */
void SVFIRBuilder::handleIndCall(CallBase* cs)
{
    const SVFInstruction* svfcall = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(cs);
    const SVFValue* svfcalledval = LLVMModuleSet::getLLVMModuleSet()->getSVFValue(cs->getCalledOperand());

    const CallICFGNode* cbn = pag->getICFG()->getCallICFGNode(svfcall);
    pag->addIndirectCallsites(cbn,pag->getValueNode(svfcalledval));
}

void SVFIRBuilder::updateCallGraph(PTACallGraph* callgraph)
{
    PTACallGraph::CallEdgeMap::const_iterator iter = callgraph->getIndCallMap().begin();
    PTACallGraph::CallEdgeMap::const_iterator eiter = callgraph->getIndCallMap().end();
    for (; iter != eiter; iter++)
    {
        const CallICFGNode* callBlock = iter->first;
        const CallBase* callbase = SVFUtil::cast<CallBase>(LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callBlock->getCallSite()));
        assert(callBlock->isIndirectCall() && "this is not an indirect call?");
        const PTACallGraph::FunctionSet& functions = iter->second;
        for (PTACallGraph::FunctionSet::const_iterator func_iter = functions.begin(); func_iter != functions.end(); func_iter++)
        {
            const Function* callee = SVFUtil::cast<Function>(LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(*func_iter));

            if (isExtCall(*func_iter))
            {
                setCurrentLocation(callee, &callee->getEntryBlock());
                handleExtCall(const_cast<CallBase*>(callbase), callee);
            }
            else
            {
                setCurrentLocation(callBlock->getCallSite(), callBlock->getCallSite()->getParent());
                handleDirectCall(const_cast<CallBase*>(callbase), callee);
            }
        }
    }

    // dump SVFIR
    if (Options::PAGDotGraph())
        pag->dump("svfir_final");
}

/*
 * TODO: more sanity checks might be needed here
 */
void SVFIRBuilder::sanityCheck()
{
    for (SVFIR::iterator nIter = pag->begin(); nIter != pag->end(); ++nIter)
    {
        (void) pag->getGNode(nIter->first);
        //TODO::
        // (1)  every source(root) node of a pag tree should be object node
        //       if a node has no incoming edge, but has outgoing edges
        //       then it has to be an object node.
        // (2)  make sure every variable should be initialized
        //      otherwise it causes the a null pointer, the aliasing relation may not be captured
        //      when loading a pointer value should make sure
        //      some value has been store into this pointer before
        //      q = load p, some value should stored into p first like store w p;
        // (3)  make sure PAGNode should not have a const expr value (pointer should have unique def)
        // (4)  look closely into addComplexConsForExt, make sure program locations(e.g.,inst bb)
        //      are set correctly for dummy gepval node
        // (5)  reduce unnecessary copy edge (const casts) and ensure correctness.
    }
}


/*!
 * Add a temp field value node according to base value and offset
 * this node is after the initial node method, it is out of scope of symInfo table
 */
NodeID SVFIRBuilder::getGepValVar(const Value* val, const LocationSet& ls, const SVFType* elementType)
{
    NodeID base = pag->getBaseValVar(getValueNode(val));
    NodeID gepval = pag->getGepValVar(curVal, base, ls);
    if (gepval==UINT_MAX)
    {
        assert(((int) UINT_MAX)==-1 && "maximum limit of unsigned int is not -1?");
        /*
         * getGepValVar can only be called from two places:
         * 1. SVFIRBuilder::addComplexConsForExt to handle external calls
         * 2. SVFIRBuilder::getGlobalVarField to initialize global variable
         * so curVal can only be
         * 1. Instruction
         * 2. GlobalVariable
         */
        assert((SVFUtil::isa<SVFInstruction, SVFGlobalValue>(curVal)) && "curVal not an instruction or a globalvariable?");

        // We assume every GepValNode and its GepEdge to the baseNode are unique across the whole program
        // We preserve the current BB information to restore it after creating the gepNode
        const SVFValue* cval = getCurrentValue();
        const SVFBasicBlock* cbb = getCurrentBB();
        setCurrentLocation(curVal, nullptr);
        LLVMModuleSet* llvmmodule = LLVMModuleSet::getLLVMModuleSet();
        NodeID gepNode= pag->addGepValNode(curVal, llvmmodule->getSVFValue(val),ls, NodeIDAllocator::get()->allocateValueId(),elementType->getPointerTo());
        addGepEdge(base, gepNode, ls, true);
        setCurrentLocation(cval, cbb);
        return gepNode;
    }
    else
        return gepval;
}


/*
 * curVal   <-------->  PAGEdge
 * Instruction          Any Edge
 * Argument             CopyEdge  (SVFIR::addFormalParamBlackHoleAddrEdge)
 * ConstantExpr         CopyEdge  (Int2PtrConstantExpr   CastConstantExpr  SVFIRBuilder::processCE)
 *                      GepEdge   (GepConstantExpr   SVFIRBuilder::processCE)
 * ConstantPointerNull  CopyEdge  (3-->2 NullPtr-->BlkPtr SVFIR::addNullPtrNode)
 *  				    AddrEdge  (0-->2 BlkObj-->BlkPtr SVFIR::addNullPtrNode)
 * GlobalVariable       AddrEdge  (SVFIRBuilder::visitGlobal)
 *                      GepEdge   (SVFIRBuilder::getGlobalVarField)
 * Function             AddrEdge  (SVFIRBuilder::visitGlobal)
 * Constant             StoreEdge (SVFIRBuilder::InitialGlobal)
 */
void SVFIRBuilder::setCurrentBBAndValueForPAGEdge(PAGEdge* edge)
{
    if (SVFModule::pagReadFromTXT())
        return;

    assert(curVal && "current Val is nullptr?");
    edge->setBB(curBB!=nullptr ? curBB : nullptr);
    edge->setValue(curVal);
    // backmap in valuToEdgeMap
    pag->mapValueToEdge(curVal, edge);
    ICFGNode* icfgNode = pag->getICFG()->getGlobalICFGNode();
    if (const SVFInstruction* curInst = SVFUtil::dyn_cast<SVFInstruction>(curVal))
    {
        const SVFFunction* srcFun = edge->getSrcNode()->getFunction();
        const SVFFunction* dstFun = edge->getDstNode()->getFunction();
        if(srcFun!=nullptr && !SVFUtil::isa<RetPE>(edge) && !SVFUtil::isa<SVFFunction>(edge->getSrcNode()->getValue()))
        {
            assert(srcFun==curInst->getFunction() && "SrcNode of the PAGEdge not in the same function?");
        }
        if(dstFun!=nullptr && !SVFUtil::isa<CallPE>(edge) && !SVFUtil::isa<SVFFunction>(edge->getDstNode()->getValue()))
        {
            assert(dstFun==curInst->getFunction() && "DstNode of the PAGEdge not in the same function?");
        }

        /// We assume every GepValVar and its GepStmt are unique across whole program
        if (!(SVFUtil::isa<GepStmt>(edge) && SVFUtil::isa<GepValVar>(edge->getDstNode())))
            assert(curBB && "instruction does not have a basic block??");

        /// We will have one unique function exit ICFGNode for all returns
        if(curInst->isRetInst())
        {
            icfgNode = pag->getICFG()->getFunExitICFGNode(curInst->getFunction());
        }
        else
        {
            if(SVFUtil::isa<RetPE>(edge))
                icfgNode = pag->getICFG()->getRetICFGNode(curInst);
            else
                icfgNode = pag->getICFG()->getICFGNode(curInst);
        }
    }
    else if (const SVFArgument* arg = SVFUtil::dyn_cast<SVFArgument>(curVal))
    {
        assert(curBB && (curBB->getParent()->getEntryBlock() == curBB));
        icfgNode = pag->getICFG()->getFunEntryICFGNode(arg->getParent());
    }
    else if (SVFUtil::isa<SVFConstant>(curVal) ||
             SVFUtil::isa<SVFFunction>(curVal) ||
             SVFUtil::isa<SVFMetadataAsValue>(curVal))
    {
        if (!curBB)
            pag->addGlobalPAGEdge(edge);
        else
        {
            icfgNode = pag->getICFG()->getICFGNode(curBB->front());
        }
    }
    else
    {
        assert(false && "what else value can we have?");
    }

    pag->addToSVFStmtList(icfgNode,edge);
    icfgNode->addSVFStmt(edge);
    if(const CallPE* callPE = SVFUtil::dyn_cast<CallPE>(edge))
    {
        CallICFGNode* callNode = const_cast<CallICFGNode*>(callPE->getCallSite());
        FunEntryICFGNode* entryNode = const_cast<FunEntryICFGNode*>(callPE->getFunEntryICFGNode());
        if(ICFGEdge* edge = pag->getICFG()->hasInterICFGEdge(callNode,entryNode, ICFGEdge::CallCF))
            SVFUtil::cast<CallCFGEdge>(edge)->addCallPE(callPE);
    }
    else if(const RetPE* retPE = SVFUtil::dyn_cast<RetPE>(edge))
    {
        RetICFGNode* retNode = const_cast<RetICFGNode*>(retPE->getCallSite()->getRetICFGNode());
        FunExitICFGNode* exitNode = const_cast<FunExitICFGNode*>(retPE->getFunExitICFGNode());
        if(ICFGEdge* edge = pag->getICFG()->hasInterICFGEdge(exitNode, retNode, ICFGEdge::RetCF))
            SVFUtil::cast<RetCFGEdge>(edge)->addRetPE(retPE);
    }
}


/*!
 * Get a base SVFVar given a pointer
 * Return the source node of its connected normal gep edge
 * Otherwise return the node id itself
 * s32_t offset : gep offset
 */
LocationSet SVFIRBuilder::getLocationSetFromBaseNode(NodeID nodeId)
{
    SVFVar* node  = pag->getGNode(nodeId);
    SVFStmt::SVFStmtSetTy& geps = node->getIncomingEdges(SVFStmt::Gep);
    /// if this node is already a base node
    if(geps.empty())
        return LocationSet(0);

    assert(geps.size()==1 && "one node can only be connected by at most one gep edge!");
    SVFVar::iterator it = geps.begin();
    const GepStmt* gepEdge = SVFUtil::cast<GepStmt>(*it);
    if(gepEdge->isVariantFieldGep())
        return LocationSet(0);
    else
        return gepEdge->getLocationSet();
}
