
#undef DEBUG

#include <llvm/Support/Debug.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/CallSite.h>
#include <llvm/Support/GraphWriter.h>

#include "Passes.h"
#include "DelegateExtractor.hpp"

using namespace llvm;

StringRef getColorString(unsigned ColorNumber) {
  static const int NumColors = 20;
  static const char* Colors[NumColors] = {
    "red", "blue", "green", "gold", "cyan", "purple", "orange",
    "darkgreen", "coral", "deeppink", "deepskyblue", "orchid", "brown", "yellowgreen",
    "midnightblue", "firebrick", "peachpuff", "yellow", "limegreen", "khaki"};
  return Colors[ColorNumber % NumColors];
}

namespace Grappa {
  
  Value* search(Value* v) {
    if (auto gep = dyn_cast<GetElementPtrInst>(v)) {
      if (!gep->isInBounds()) return v;
      if (gep->hasIndices()) {
        if (gep->getPointerAddressSpace() == GLOBAL_SPACE) {
          auto idx = gep->getOperand(1);
          if (idx != ConstantInt::get(idx->getType(), 0)) {
            return v;
          }
        }
      }
      return search(gep->getPointerOperand());
    }
    if (auto c = dyn_cast<CastInst>(v)) {
      auto vv = search(c->getOperand(0));
      if (isa<PointerType>(vv->getType()))
        return vv;
      else
        return v;
    }
    if (auto c = dyn_cast<ConstantExpr>(v)) {
      auto ci = c->getAsInstruction();
      auto vv = search(ci);
      delete ci;
      return vv;
    }
    
    return v;
  }
  
  void setProvenance(Instruction* inst, Value* ptr) {
    inst->setMetadata("grappa.prov", MDNode::get(inst->getContext(), ptr));
  }
  
  Value* getProvenance(Instruction* inst) {
    if (auto m = inst->getMetadata("grappa.prov")) {
      return m->getOperand(0);
    }
    return nullptr;
  }
  
  bool isGlobalPtr(Value* v)    { return dyn_cast_addr<GLOBAL_SPACE>(v->getType()); }
  bool isSymmetricPtr(Value* v) { return dyn_cast_addr<SYMMETRIC_SPACE>(v->getType()); }
  bool isStatic(Value* v)       { return isa<GlobalVariable>(v); }
  bool isConst(Value* v)       { return isa<Constant>(v) || isa<BasicBlock>(v); }
  bool isStack(Value* v)        { return isa<AllocaInst>(v) || isa<Argument>(v); }
  
  bool isAnchor(Instruction* inst) {
    Value* ptr = getProvenance(inst);
    if (ptr)
      return isGlobalPtr(ptr) || isStack(ptr);
    else
      return false;
  }
  
  void ExtractorPass::analyzeProvenance(Function& fn, AnchorSet& anchors) {
    for (auto& bb: fn) {
      for (auto& i : bb) {
        Value *prov = nullptr;
        if (auto l = dyn_cast<LoadInst>(&i)) {
          prov = search(l->getPointerOperand());
        } else if (auto s = dyn_cast<StoreInst>(&i)) {
          prov = search(s->getPointerOperand());
        }
        if (prov) {
          setProvenance(&i, prov);
          if (isAnchor(&i)) {
            anchors.insert(&i);
          }
        }
      }
    }
  }
  
  
  struct CandidateRegion;
  using CandidateMap = std::map<Instruction*,CandidateRegion*>;
  
  struct CandidateRegion {
    static long id_counter;
    long ID;
    
    Instruction* entry;
    std::map<Instruction*,Instruction*> exits;
    
    Value* target_ptr;
    SmallSet<Value*,4> valid_ptrs;
    
    CandidateMap& candidates;
    
    CandidateRegion(Value* target_ptr, Instruction* entry, CandidateMap& candidates):
      ID(id_counter++), entry(entry), target_ptr(target_ptr), candidates(candidates) {}
    
    template< typename F >
    void visit(F yield) {
      UniqueQueue<Instruction*> q;
      q.push(entry);
      
      while (!q.empty()) {
        BasicBlock::iterator it(q.pop());
        auto bb = it->getParent();
        while (it != bb->end()) {
          if (exits.count(it)) break;
          auto iit = it;
          it++;
          yield(iit);
        }
        if (it == bb->end()) {
          for (auto sb = succ_begin(bb), sbe = succ_end(bb); sb != sbe; sb++) {
            q.push(sb->begin());
          }
        }
      }
    }
    
    void expandRegion() {
      UniqueQueue<Instruction*> worklist;
      worklist.push(entry);
      
      SmallSet<BasicBlock*,8> bbs;
      SmallSetVector<BasicBlock*,8> try_again;
      
      while (!worklist.empty()) {
        auto i = BasicBlock::iterator(worklist.pop());
        auto bb = i->getParent();
        
        while ( i != bb->end() && validInRegion(i) ) {
          candidates[i] = this;
          i++;
        }
        
        if (i == bb->end()) {
          bbs.insert(bb);
          for (auto sb = succ_begin(bb), se = succ_end(bb); sb != se; sb++) {
            
            // at least first instruction is valid
            auto target = (*sb)->begin();
            bool valid = validInRegion(target);
            
            // all predecessors already in region
            if (valid) for_each(pb, *sb, pred) {
              bool b = (bbs.count(*pb) > 0);
              if (!b) {
                outs() << "!! disallowing -- invalid preds:\n" << *bb << "\n" << **pb;
                try_again.insert(*sb);
              }
              valid &= b;
            }
            
            if (valid) {
              worklist.push(target);
            } else {
              // exit
              auto ex = i->getPrevNode();
              if (exits.count(target) > 0 && exits[target] != ex) {
                assert(false && "unhandled case");
              } else {
                exits[target] = ex;
              }
            }
          }
        } else {
          exits[i] = i->getPrevNode();
        }
        
        // check try-again list
        for (auto bb : try_again) {
          bool valid = true;
          for_each(pb, bb, pred) valid &= (bbs.count(*pb) > 0);
          assert(exits.count(bb->begin()));
          exits.erase(bb->begin());
          try_again.remove(bb);
          worklist.push(bb->begin());
          break;
        }
      }
    }
    
    bool validInRegion(Instruction* i) {
      if (i->mayReadOrWriteMemory()) {
        if (auto p = getProvenance(i)) {
          if (valid_ptrs.count(p) || isSymmetricPtr(p) || isStatic(p) || isConst(p)) {
            return true;
          }
        } else if (isa<CallInst>(i) || isa<InvokeInst>(i)) {
          // do nothing for now
          auto cs = CallSite(i);
          if (auto fn = cs.getCalledFunction()) {
            if (fn->hasFnAttribute("unbound") || fn->doesNotAccessMemory()) {
              return true;
            }
          }
        } else {
          errs() << "!! no provenance:" << *i;
        }
        return false;
      } else {
        return true;
      }
    }
    
    Function* extractRegion(GlobalPtrInfo& ginfo, DataLayout& layout) {
      errs() << "target_ptr =>" << *target_ptr << "\n";
      
      auto mod = entry->getParent()->getParent()->getParent();
      auto& ctx = entry->getContext();
      auto ty_i16 = Type::getInt16Ty(ctx);
      auto ty_void_ptr = Type::getInt8PtrTy(ctx);
      auto ty_void_gptr = Type::getInt8PtrTy(ctx, GLOBAL_SPACE);
      auto i64 = [&](int64_t v) { return ConstantInt::get(Type::getInt64Ty(ctx), v); };
      auto idx = [&](int i) { return ConstantInt::get(Type::getInt32Ty(ctx), i); };
      
      SmallSet<BasicBlock*,8> bbs;
      
      //////////////////////////////////////////////////////////////
      // first slice and dice at boundaries and build up set of BB's
      auto bb_in = entry->getParent();
      outs() << "entry =>" << *entry << "\n";
      outs() << "bb_in => " << bb_in->getName() << "\n";

      auto old_fn = bb_in->getParent();
      
      auto name = "d" + Twine(ID);
      
      outs() << "name => " << name << "\n";

      if (BasicBlock::iterator(entry) != bb_in->begin()) {
        bb_in = bb_in->splitBasicBlock(entry, name+".eblk");
      }
      outs() << "bb_in => " << bb_in->getName() << "\n";
      bbs.insert(bb_in);
      
      for (auto e : exits) {
        auto before_exit = e.second;
        auto after_exit = e.first;
        
        auto bb_exit = before_exit->getParent();
        if (bb_exit == after_exit->getParent()) {
          bb_exit->splitBasicBlock(after_exit, name+".exit");
        }
        bbs.insert(bb_exit);
      }
      
      visit([&](BasicBlock::iterator it){ bbs.insert(it->getParent()); });
      
      /////////////////////////
      // find inputs/outputs
      auto definedInRegion = [&](Value* v) {
        if (auto i = dyn_cast<Instruction>(v))
          if (bbs.count(i->getParent()))
            return true;
        return false;
      };
      auto definedInCaller = [&](Value* v) {
        if (isa<Argument>(v)) return true;
        if (auto i = dyn_cast<Instruction>(v))
          if (!bbs.count(i->getParent()))
            return true;
        return false;
      };
      
      ValueSet inputs, outputs;
      visit([&](BasicBlock::iterator it){
        for_each_op(o, *it)  if (definedInCaller(*o)) inputs.insert(*o);
        for_each_use(u, *it) if (!definedInRegion(*u)) { outputs.insert(it); break; }
      });
      
      /////////////////////////////////////////////
      // create struct types for inputs & outputs
      SmallVector<Type*,8> in_types, out_types;
      for (auto& p : inputs)  {  in_types.push_back(p->getType()); }
      for (auto& p : outputs) { out_types.push_back(p->getType()); }
      
      auto in_struct_ty = StructType::get(ctx, in_types);
      auto out_struct_ty = StructType::get(ctx, out_types);
      
      /////////////////////////
      // create function shell
      auto new_fn = Function::Create(
                      FunctionType::get(ty_i16, (Type*[]){ ty_void_ptr, ty_void_ptr }, false),
                      GlobalValue::InternalLinkage, name, mod);
      
      auto bb_entry = BasicBlock::Create(ctx, name+".entry", new_fn);
      
      IRBuilder<> b(bb_entry);
      
      auto argi = new_fn->arg_begin();
      auto in_arg  = b.CreateBitCast(argi++, in_struct_ty->getPointerTo(), "struct.in");
      auto out_arg = b.CreateBitCast(argi++, out_struct_ty->getPointerTo(), "struct.out");
      
      /////////////////////////////
      // now clone blocks
      ValueToValueMapTy clone_map;
      for (auto bb : bbs) {
        clone_map[bb] = CloneBasicBlock(bb, clone_map, ".clone", new_fn);
      }
      
      ///////////////////////////
      // remap and load inputs
      for (int i=0; i < inputs.size(); i++) {
        auto v = inputs[i];
        clone_map[v] = b.CreateLoad(b.CreateGEP(in_arg, { idx(0), idx(i) }),
                                    "in." + v->getName());
      }
      
      auto bb_in_clone = cast<BasicBlock>(clone_map[bb_in]);
      b.CreateBr(bb_in_clone);
      
      auto bb_ret = BasicBlock::Create(ctx, name+".ret", new_fn);
      auto ty_ret = ty_i16;
      
      // create phi for selecting which return value to use
      // make sure it's first thing in BB
      b.SetInsertPoint(bb_ret);
      auto phi_ret = b.CreatePHI(ty_ret, exits.size(), "ret.phi");
      // return from end of created block
      b.CreateRet(phi_ret);
      
      ////////////////////////////////
      // store outputs at last use
      for (int i = 0; i < outputs.size(); i++) {
        assert(clone_map.count(outputs[i]) > 0);
        auto v = cast<Instruction>(clone_map[outputs[i]]);
        // insert at end of (cloned) block containing the (remapped) value
        b.SetInsertPoint(v->getParent()->getTerminator());
        b.CreateStore(v, b.CreateGEP(out_arg, { idx(0), idx(i) }, "out."+v->getName()));
      }
      
      /////////////////////////////////////////////////////////////////
      // (in original function)
      /////////////////////////////////////////////////////////////////
      
      // put allocas at top of function
      b.SetInsertPoint(old_fn->begin()->begin());
      auto in_alloca =  b.CreateAlloca(in_struct_ty,  0, name+".struct.in");
      auto out_alloca = b.CreateAlloca(out_struct_ty, 0, name+".struct.out");
      
      //////////////
      // emit call
      auto bb_call = BasicBlock::Create(ctx, name+".call", old_fn, bb_in);
      b.SetInsertPoint(bb_call);

      // replace uses of bb_in
      for_each(it, bb_in, pred) {
        auto bb = *it;
        if (bbs.count(bb) == 0) {
          bb->getTerminator()->replaceUsesOfWith(bb_in, bb_call);
        }
      }
      
      // copy inputs into struct
      for (int i = 0; i < inputs.size(); i++) {
        b.CreateStore(inputs[i], b.CreateGEP(in_alloca, {idx(0),idx(i)}, name+".gep.in"));
      }

      auto target_core = b.CreateCall(ginfo.get_core_fn, (Value*[]){
        b.CreateBitCast(target_ptr, ty_void_gptr)
      }, name+".target_core");
      
      auto call = b.CreateCall(ginfo.call_on_fn, {
        target_core, new_fn,
        b.CreateBitCast(in_alloca, ty_void_ptr),
        i64(layout.getTypeAllocSize(in_struct_ty)),
        b.CreateBitCast(out_alloca, ty_void_ptr),
        i64(layout.getTypeAllocSize(out_struct_ty))
      }, name+".call_on");
      
      auto exit_switch = b.CreateSwitch(call, bb_call, exits.size());
      
      // switch among exit blocks
      int exit_id = 0;
      for (auto& e : exits) {
        auto before_exit = e.second;
        auto after_exit = e.first;
        auto bb_exit = after_exit->getParent();
        assert(bb_exit->getParent() == old_fn);
        assert(static_cast<Instruction*>(bb_exit->begin()) == after_exit);
        assert(before_exit->getParent() != after_exit->getParent());
        
        auto exit_code = ConstantInt::get(ty_ret, exit_id++);
        
        assert(clone_map.count(before_exit));
        assert(clone_map.count(before_exit->getParent()));
        auto bb_pred = cast<BasicBlock>(clone_map[before_exit->getParent()]);
        assert(bb_pred->getParent() == new_fn);
        
        // hook up exit from region with phi node in return block
        phi_ret->addIncoming(exit_code, bb_pred);
        
        // jump to old exit block when call returns the corresponding code
        exit_switch->addCase(exit_code, after_exit->getParent());
        assert(exit_switch->getParent()->getParent() == old_fn);
        
        // rewrite any phi's in exit bb to get their values from bb_call
        for (auto& inst : *bb_exit) {
          if (auto phi = dyn_cast<PHINode>(&inst)) {
            int i;
            while ((i = phi->getBasicBlockIndex(bb_pred)) >= 0)
              phi->setIncomingBlock(i, bb_call);
          }
        }
        
        before_exit->getParent()->replaceAllUsesWith(bb_call);
        
        // in extracted fn, remap branches outside to bb_ret
        clone_map[bb_exit] = bb_ret;
      }
      
      // use clone_map to remap values in new function
      // (including branching to new bb_ret instead of exit blocks)
      for_each(inst, new_fn, inst) {
        RemapInstruction(&*inst, clone_map, RF_IgnoreMissingEntries);
      }
      
      // load outputs (also rewriting uses, so have to do this *after* remap above)
      b.SetInsertPoint(exit_switch);
      for (int i = 0; i < outputs.size(); i++) {
        auto v = outputs[i];
        auto ld = b.CreateLoad(b.CreateGEP(out_alloca, {idx(0), idx(i)}, "out."+v->getName()));
        v->replaceAllUsesWith(ld);
      }
      
      for (auto& bb : *new_fn) {
        for_each_use(u, bb) {
          if (auto ui = dyn_cast<Instruction>(*u)) {
            if (ui->getParent()->getParent() != new_fn) {
              errs() << "use =>" << *ui << "\n";
              assert(false && "!! use escaped");
            }
          }
        }
        for_each(sb, &bb, succ) {
          assert(sb->getParent() == new_fn);
        }
        for (auto& i : bb) {
          for_each_use(u, i) {
            if (auto ui = dyn_cast<Instruction>(*u)) {
              assert(ui->getParent()->getParent() == new_fn);
            }
          }
        }
      }
      
      // TODO: fixup global* uses in new_fn
      
      outs() << "~~~~~~~~~~~~~~~\n";
      for (auto bb : bbs) {
        outs() << *bb;
      }
      outs() << "~~~~~~~~~~~~~~~\n";
      
      // verify that all uses of these bbs are contained
      for (auto bb : bbs) {
        for (auto u = bb->use_begin(), ue = bb->use_end(); u != ue; u++) {
          if (auto uu = dyn_cast<Instruction>(*u)) {
            if (bbs.count(uu->getParent()) == 0) {
              auto uubb = uu->getParent();
              errs() << "use escaped => " << *uubb << *bb;
              assert(uubb->getParent() == old_fn);
              assert(false);
            }
          }
        }
      }
//        for (auto& i : *bb) {
//          for (auto u = i.use_begin(), ue = i.use_end(); u != ue; u++) {
//            if (auto uu = dyn_cast<Instruction>(*u)) {
//              if (bbs.count(uu->getParent()) == 0) {
//                errs() << "use escaped => " << *uu << *bb;
//                assert(false);
//              }
//            }
//          }
//        }
//      }

      // delete old bbs
//      for (auto bb : bbs) for (auto& i : *bb) i.dropAllReferences();
//      for (auto bb : bbs) bb->eraseFromParent();
      
      outs() << "-------------------------------\n";
      outs() << *new_fn;
      outs() << "-------------------------------\n";
      outs() << *bb_call;
      
      return new_fn;
    }
    
    void printHeader() {
      outs() << "Candidate " << ID << ":\n";
      outs() << "  entry:\n  " << *entry << "\n";
      outs() << "  valid_ptrs:\n";
      for (auto p : valid_ptrs) outs() << "  " << *p << "\n";
      outs() << "  exits:\n";
      for (auto p : exits) outs() << "  " << *p.first << "\n     =>" << *p.second << "\n";
      outs() << "\n";
    }
    
    void prettyPrint(BasicBlock* bb = nullptr) {
      outs() << "~~~~~~~~~~~~~~~~~~~~~~~\n";
      printHeader();
      
      BasicBlock::iterator i;
      if (!bb) {
        bb = entry->getParent();
        i = BasicBlock::iterator(entry);
        if (i != bb->begin()) {
          outs() << *i->getPrevNode() << "\n";
        }
      } else {
        i = bb->begin();
        outs() << bb->getName() << ":\n";
      }
      outs() << "--------------------\n";
      
      for (; i != bb->end(); i++) {
        if (exits.count(i)) {
          outs() << "--------------------\n";
          i++;
          if (i != bb->end()) outs() << *i << "\n";
          break;
        }
        outs() << *i << "\n";
      }
      if (i == bb->end()) {
        for_each(sb, bb, succ) {
          prettyPrint(*sb);
        }
      }
    }
    
    static void dotBB(raw_ostream& o, CandidateMap& candidates, BasicBlock* bb, CandidateRegion* this_region = nullptr) {
      o << "  \"" << bb << "\" [label=<\n";
      o << "  <table cellborder='0' border='0'>\n";
      o << "    <tr><td align='left'>" << bb->getName() << "</td></td>\n";
      
      for (auto& i : *bb) {
        std::string s;
        raw_string_ostream os(s);
        os << i;
        s = DOT::EscapeString(s);
        
        o << "    <tr><td align='left'>";
        
        if (candidates[&i]) {
          o << "<font color='"
            << getColorString(candidates[&i]->ID)
            << "'>" << s << "</font>";
        } else {
          o << s;
        }
        o << "</td></tr>\n";
      }
      
      o << "  </table>\n";
      o << "  >];\n";
      
      for_each(sb, bb, succ) {
        o << "  \"" << bb << "\"->\"" << *sb << "\"\n";
        if (this_region && candidates[sb->begin()] == this_region) {
          dotBB(o, candidates, *sb, this_region);
        }
      }
    }
    
    void dumpToDot() {
      Function& F = *entry->getParent()->getParent();
      
      auto s = F.getParent()->getModuleIdentifier();
      auto base = s.substr(s.rfind("/")+1);
      
      std::string _s;
      raw_string_ostream fname(_s);
      fname <<  "dots/" << base << "." << ID << ".dot";
      
      std::string err;
      outs() << "dot => " << fname.str() << "\n";
      raw_fd_ostream o(fname.str().c_str(), err);
      if (err.size()) {
        errs() << "dot error: " << err;
      }
      
      o << "digraph Candidate {\n";
      o << "  node[shape=record];\n";
      dotBB(o, candidates, entry->getParent());
      o << "}\n";
      
      o.close();
    }
    
    static void dumpToDot(Function& F, CandidateMap& candidates, const Twine& name) {
      auto s = F.getParent()->getModuleIdentifier();
      auto base = s.substr(s.rfind("/")+1);
      
      std::string _s;
      raw_string_ostream fname(_s);
      fname <<  "dots/" << base << "." << name << ".dot";
      
      std::string err;
      outs() << "dot => " << fname.str() << "\n";
      raw_fd_ostream o(fname.str().c_str(), err);
      if (err.size()) {
        errs() << "dot error: " << err;
      }
      
      o << "digraph TaskFunction {\n";
      o << "  label=\"" << demangle(F.getName()) << "\"";
      o << "  node[shape=record];\n";
      
      for (auto& bb : F) {
        dotBB(o, candidates, &bb);
      }
      
      o << "}\n";
      
      o.close();
    }

  };
  
  long CandidateRegion::id_counter = 0;
  
  bool ExtractorPass::runOnModule(Module& M) {
    outs() << "Running extractor...\n";
    bool changed = false;
    
    auto layout = new DataLayout(&M);
    
//    if (! ginfo.init(M) ) return false;
    bool found_functions = ginfo.init(M);
    if (!found_functions)
      outs() << "Didn't find Grappa primitives, disabling extraction.\n";
    
    //////////////////////////
    // Find 'task' functions
    for (auto& F : M) {
      if (F.hasFnAttribute("async")) {
        task_fns.insert(&F);
      }
    }
    
    outs() << "task_fns.count => " << task_fns.size() << "\n";
    outs().flush();
//    std::vector<DelegateExtractor*> candidates;
    
    CandidateMap candidate_map;
    int ct = 0;
    
    UniqueQueue<Function*> worklist;
    for (auto fn : task_fns) worklist.push(fn);
    
    while (!worklist.empty()) {
      auto fn = worklist.pop();
      
      AnchorSet anchors;
      analyzeProvenance(*fn, anchors);
      
      std::map<Value*,CandidateRegion*> candidates;
      
      for (auto a : anchors) {
        auto p = getProvenance(a);
        if (candidate_map[a]) {
          outs() << "anchor already in another delegate:\n";
          outs() << "  anchor =>" << *a << "\n";
          outs() << "  other  =>" << *candidate_map[a]->entry << "\n";
        } else if (isGlobalPtr(p)) {
          auto r = new CandidateRegion(p, a, candidate_map);
          r->valid_ptrs.insert(p);
          r->expandRegion();
          
          r->printHeader();
          
          r->visit([&](BasicBlock::iterator i){
            if (candidate_map[i] != r) {
              errs() << "!! bad visit: " << *i << "\n";
              assert(false);
            }
          });
          
          candidates[a] = r;
        }
      }
      
      auto taskname = "task" + Twine(ct++);
      if (candidates.size() > 0) {
//        CandidateRegion::dumpToDot(*fn, candidate_map, taskname);
      }
      
      for_each(it, *fn, inst) {
        auto inst = &*it;
        if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
          CallSite cs(inst);
          if (cs.getCalledFunction()) worklist.push(cs.getCalledFunction());
        }
      }
      
      if (found_functions) {
        for (auto e : candidates) {
          auto cnd = e.second;
          auto new_fn = cnd->extractRegion(ginfo, *layout);
          
//          CandidateRegion::dumpToDot(*new_fn, candidate_map, taskname+".d"+Twine(cnd->ID));
        }
      }
      
      for (auto c : candidates) delete c.second;
    }
    
//    outs() << "<< anchors:\n";
//    for (auto a : anchors) {
//      outs() << "  " << *a << "\n";
//    }
//    outs() << ">>>>>>>>>>\n";
    outs().flush();
    
    return changed;
  }
    
  bool ExtractorPass::doInitialization(Module& M) {
    outs() << "-- Grappa Extractor --\n";
    outs().flush();
    return false;
  }
  
  bool ExtractorPass::doFinalization(Module& M) {
    outs().flush();
    return true;
  }
  
  char ExtractorPass::ID = 0;
  
  //////////////////////////////
  // Register optional pass
  static RegisterPass<ExtractorPass> X( "grappa-ex", "Grappa Extractor", false, false );
  
}
