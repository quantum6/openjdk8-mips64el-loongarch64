/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2021, Loongson Technology. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "oops/markOop.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/monitorChunk.hpp"
#include "runtime/signature.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "vmreg_loongarch.inline.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#include "runtime/vframeArray.hpp"
#endif

#ifdef ASSERT
void RegisterMap::check_location_valid() {
}
#endif


// Profiling/safepoint support
// for Profiling - acting on another frame. walks sender frames
// if valid.
// frame profile_find_Java_sender_frame(JavaThread *thread);

bool frame::safe_for_sender(JavaThread *thread) {
  address   sp = (address)_sp;
  address   fp = (address)_fp;
  bool sp_safe = (sp != NULL &&
      (sp <= thread->stack_base()) &&
      (sp >= thread->stack_base() - thread->stack_size()));
  bool fp_safe = (fp != NULL &&
      (fp <= thread->stack_base()) &&
      (fp >= thread->stack_base() - thread->stack_size()));
  if (sp_safe && fp_safe) {
    CodeBlob *cb = CodeCache::find_blob_unsafe(_pc);
    // First check if frame is complete and tester is reliable
    // Unfortunately we can only check frame complete for runtime stubs and nmethod
    // other generic buffer blobs are more problematic so we just assume they are
    // ok. adapter blobs never have a frame complete and are never ok.
    if (cb != NULL && !cb->is_frame_complete_at(_pc)) {
      if (cb->is_nmethod() || cb->is_adapter_blob() || cb->is_runtime_stub()) {
        return false;
      }
    }
    return true;
  }
  // Note: fp == NULL is not really a prerequisite for this to be safe to
  // walk for c2. However we've modified the code such that if we get
  // a failure with fp != NULL that we then try with FP == NULL.
  // This is basically to mimic what a last_frame would look like if
  // c2 had generated it.
  if (sp_safe && fp == NULL) {
    CodeBlob *cb = CodeCache::find_blob_unsafe(_pc);
    // frame must be complete if fp == NULL as fp == NULL is only sensible
    // if we are looking at a nmethod and frame complete assures us of that.
    if (cb != NULL && cb->is_frame_complete_at(_pc) && cb->is_compiled_by_c2()) {
      return true;
    }
  }
  return false;
}


void frame::patch_pc(Thread* thread, address pc) {
  address* pc_addr = &(((address*) sp())[-1]);
  if (TracePcPatching) {
    tty->print_cr("patch_pc at address " INTPTR_FORMAT " [" INTPTR_FORMAT " -> " INTPTR_FORMAT "]",
                  p2i(pc_addr), p2i(*pc_addr), p2i(pc));
  }

  RegisterMap map((JavaThread *)thread, false);
  frame check = ((JavaThread *)thread)->last_frame();
  if (id() != check.id())
  {
    while (id() != check.sender(&map).id()) {
      check = check.sender(&map);
    }
    if (check.is_interpreted_frame())
      *(check.fp() + 1) = (intptr_t)pc;
    else
      ((address *)_sp)[-1]  = pc;
  }

  _cb = CodeCache::find_blob(pc);
  if (_cb != NULL && _cb->is_nmethod() && ((nmethod*)_cb)->is_deopt_pc(_pc)) {
    address orig = (((nmethod*)_cb)->get_original_pc(this));
    assert(orig == _pc, "expected original to be stored before patching");
    _deopt_state = is_deoptimized;
    // leave _pc as is
  } else {
    _deopt_state = not_deoptimized;
    _pc = pc;
  }
}

bool frame::is_interpreted_frame() const  {
  return Interpreter::contains(pc());
}

int frame::frame_size(RegisterMap* map) const {
  frame sender = this->sender(map);
  return sender.sp() - sp();
}

intptr_t* frame::entry_frame_argument_at(int offset) const {
  // convert offset to index to deal with tsi
  int index = (Interpreter::expr_offset_in_bytes(offset)/wordSize);
  // Entry frame's arguments are always in relation to unextended_sp()
  return &unextended_sp()[index];
}

// sender_sp
#ifdef CC_INTERP
intptr_t* frame::interpreter_frame_sender_sp() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  // QQQ why does this specialize method exist if frame::sender_sp() does same thing?
  // seems odd and if we always know interpreted vs. non then sender_sp() is really
  // doing too much work.
  return get_interpreterState()->sender_sp();
}

// monitor elements

BasicObjectLock* frame::interpreter_frame_monitor_begin() const {
  return get_interpreterState()->monitor_base();
}

BasicObjectLock* frame::interpreter_frame_monitor_end() const {
  return (BasicObjectLock*) get_interpreterState()->stack_base();
}

#else // CC_INTERP

intptr_t* frame::interpreter_frame_sender_sp() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  return (intptr_t*) at(interpreter_frame_sender_sp_offset);
}

void frame::set_interpreter_frame_sender_sp(intptr_t* sender_sp) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  int_at_put(interpreter_frame_sender_sp_offset, (intptr_t) sender_sp);
}


// monitor elements

BasicObjectLock* frame::interpreter_frame_monitor_begin() const {
  return (BasicObjectLock*) addr_at(interpreter_frame_monitor_block_bottom_offset);
}

BasicObjectLock* frame::interpreter_frame_monitor_end() const {
  BasicObjectLock* result = (BasicObjectLock*) *addr_at(interpreter_frame_monitor_block_top_offset);
  // make sure the pointer points inside the frame
  assert((intptr_t) fp() >  (intptr_t) result, "result must <  than frame pointer");
  assert((intptr_t) sp() <= (intptr_t) result, "result must >= than stack pointer");
  return result;
}

void frame::interpreter_frame_set_monitor_end(BasicObjectLock* value) {
  *((BasicObjectLock**)addr_at(interpreter_frame_monitor_block_top_offset)) = value;
}

// Used by template based interpreter deoptimization
void frame::interpreter_frame_set_last_sp(intptr_t* sp) {
  *((intptr_t**)addr_at(interpreter_frame_last_sp_offset)) = sp;
}
#endif // CC_INTERP

frame frame::sender_for_entry_frame(RegisterMap* map) const {
  assert(map != NULL, "map must be set");
  // Java frame called from C; skip all C frames and return top C
  // frame of that chunk as the sender
  JavaFrameAnchor* jfa = entry_frame_call_wrapper()->anchor();
  assert(!entry_frame_is_first(), "next Java fp must be non zero");
  assert(jfa->last_Java_sp() > sp(), "must be above this frame on stack");
  map->clear();
  assert(map->include_argument_oops(), "should be set by clear");
  if (jfa->last_Java_pc() != NULL ) {
    frame fr(jfa->last_Java_sp(), jfa->last_Java_fp(), jfa->last_Java_pc());
    return fr;
  }
  frame fr(jfa->last_Java_sp(), jfa->last_Java_fp());
  return fr;
}

frame frame::sender_for_interpreter_frame(RegisterMap* map) const {
  // sp is the raw sp from the sender after adapter or interpreter extension
  intptr_t* sp = (intptr_t*) at(interpreter_frame_sender_sp_offset);

  // This is the sp before any possible extension (adapter/locals).
  //intptr_t* unextended_sp = interpreter_frame_sender_sp();

  // The interpreter and compiler(s) always save FP in a known
  // location on entry. We must record where that location is
  // so this if FP was live on callout from c2 we can find
  // the saved copy no matter what it called.

  // Since the interpreter always saves FP if we record where it is then
  // we don't have to always save FP on entry and exit to c2 compiled
  // code, on entry will be enough.
#ifdef COMPILER2
  if (map->update_map()) {
    map->set_location(FP->as_VMReg(), (address) addr_at(link_offset));
  }
#endif /* COMPILER2 */
  return frame(sp, link(), sender_pc());
}


//------------------------------------------------------------------------------
// frame::verify_deopt_original_pc
//
// Verifies the calculated original PC of a deoptimization PC for the
// given unextended SP.  The unextended SP might also be the saved SP
// for MethodHandle call sites.
#ifdef ASSERT
void frame::verify_deopt_original_pc(nmethod* nm, intptr_t* unextended_sp, bool is_method_handle_return) {
  frame fr;

  // This is ugly but it's better than to change {get,set}_original_pc
  // to take an SP value as argument.  And it's only a debugging
  // method anyway.
  fr._unextended_sp = unextended_sp;

  address original_pc = nm->get_original_pc(&fr);
  assert(nm->insts_contains(original_pc), "original PC must be in nmethod");
  assert(nm->is_method_handle_return(original_pc) == is_method_handle_return, "must be");
}
#endif


//------------------------------------------------------------------------------
// frame::adjust_unextended_sp
void frame::adjust_unextended_sp() {
  // On LOONGARCH, sites calling method handle intrinsics and lambda forms are treated
  // as any other call site. Therefore, no special action is needed when we are
  // returning to any of these call sites.

  nmethod* sender_nm = (_cb == NULL) ? NULL : _cb->as_nmethod_or_null();
  if (sender_nm != NULL) {
    // If the sender PC is a deoptimization point, get the original PC.
    if (sender_nm->is_deopt_entry(_pc) ||
        sender_nm->is_deopt_mh_entry(_pc)) {
      DEBUG_ONLY(verify_deopt_original_pc(sender_nm, _unextended_sp));
    }
  }
}

//------------------------------sender_for_compiled_frame-----------------------
frame frame::sender_for_compiled_frame(RegisterMap* map) const {
  assert(map != NULL, "map must be set");

  // frame owned by optimizing compiler
  assert(_cb->frame_size() >= 0, "must have non-zero frame size");

  intptr_t* sender_sp = sender_sp = sp() + _cb->frame_size();
#ifdef ASSERT
  const bool c1_compiled = _cb->is_compiled_by_c1();
  bool native = _cb->is_nmethod() && ((nmethod*)_cb)->is_native_method();
  if (c1_compiled && native) {
    assert(sender_sp == fp() + frame::sender_sp_offset, "incorrect frame size");
  }
#endif // ASSERT
  // On Intel the return_address is always the word on the stack
  // the fp in compiler points to sender fp, but in interpreter, fp points to return address,
  // so getting sender for compiled frame is not same as interpreter frame.
  // we hard code here temporarily
  // spark
  address sender_pc = (address) *(sender_sp-1);

  intptr_t** saved_fp_addr = (intptr_t**) (sender_sp - frame::sender_sp_offset);

  if (map->update_map()) {
    // Tell GC to use argument oopmaps for some runtime stubs that need it.
    // For C1, the runtime stub might not have oop maps, so set this flag
    // outside of update_register_map.
    map->set_include_argument_oops(_cb->caller_must_gc_arguments(map->thread()));
    if (_cb->oop_maps() != NULL) {
      OopMapSet::update_register_map(this, map);
    }

    // Since the prolog does the save and restore of epb there is no oopmap
    // for it so we must fill in its location as if there was an oopmap entry
    // since if our caller was compiled code there could be live jvm state in it.
    map->set_location(FP->as_VMReg(), (address) saved_fp_addr);
  }
  assert(sender_sp != sp(), "must have changed");
  return frame(sender_sp, *saved_fp_addr, sender_pc);
}

frame frame::sender(RegisterMap* map) const {
  // Default is we done have to follow them. The sender_for_xxx will
  // update it accordingly
  map->set_include_argument_oops(false);

  if (is_entry_frame())       return sender_for_entry_frame(map);
  if (is_interpreted_frame()) return sender_for_interpreter_frame(map);
  assert(_cb == CodeCache::find_blob(pc()),"Must be the same");

  if (_cb != NULL) {
    return sender_for_compiled_frame(map);
  }
  // Must be native-compiled frame, i.e. the marshaling code for native
  // methods that exists in the core system.
  return frame(sender_sp(), link(), sender_pc());
}


bool frame::interpreter_frame_equals_unpacked_fp(intptr_t* fp) {
  assert(is_interpreted_frame(), "must be interpreter frame");
  Method* method = interpreter_frame_method();
  // When unpacking an optimized frame the frame pointer is
  // adjusted with:
  int diff = (method->max_locals() - method->size_of_parameters()) *
    Interpreter::stackElementWords;
  printf("^^^^^^^^^^^^^^^adjust fp in deopt fp = 0%lx \n", (intptr_t)(fp - diff));
  return _fp == (fp - diff);
}

void frame::pd_gc_epilog() {
  // nothing done here now
}

bool frame::is_interpreted_frame_valid(JavaThread* thread) const {
// QQQ
#ifdef CC_INTERP
#else
  assert(is_interpreted_frame(), "Not an interpreted frame");
  // These are reasonable sanity checks
  if (fp() == 0 || (intptr_t(fp()) & (wordSize-1)) != 0) {
    return false;
  }
  if (sp() == 0 || (intptr_t(sp()) & (wordSize-1)) != 0) {
    return false;
  }
  if (fp() + interpreter_frame_initial_sp_offset < sp()) {
    return false;
  }
  // These are hacks to keep us out of trouble.
  // The problem with these is that they mask other problems
  if (fp() <= sp()) {        // this attempts to deal with unsigned comparison above
    return false;
  }

  // do some validation of frame elements

  // first the method

  Method* m = *interpreter_frame_method_addr();

  // validate the method we'd find in this potential sender
  if (!m->is_valid_method()) return false;

  // stack frames shouldn't be much larger than max_stack elements

  //if (fp() - sp() > 1024 + m->max_stack()*Interpreter::stackElementSize()) {
  if (fp() - sp() > 4096) {  // stack frames shouldn't be large.
    return false;
  }

  // validate bci/bcx

  intptr_t  bcx    = interpreter_frame_bcx();
  if (m->validate_bci_from_bcx(bcx) < 0) {
    return false;
  }

  // validate ConstantPoolCache*

  ConstantPoolCache* cp = *interpreter_frame_cache_addr();

  if (cp == NULL || !cp->is_metaspace_object()) return false;

  // validate locals

  address locals =  (address) *interpreter_frame_locals_addr();

  if (locals > thread->stack_base() || locals < (address) fp()) return false;

  // We'd have to be pretty unlucky to be mislead at this point

#endif // CC_INTERP
  return true;
}

BasicType frame::interpreter_frame_result(oop* oop_result, jvalue* value_result) {
#ifdef CC_INTERP
  // Needed for JVMTI. The result should always be in the interpreterState object
  assert(false, "NYI");
  interpreterState istate = get_interpreterState();
#endif // CC_INTERP
  assert(is_interpreted_frame(), "interpreted frame expected");
  Method* method = interpreter_frame_method();
  BasicType type = method->result_type();

  intptr_t* tos_addr;
  if (method->is_native()) {
    // Prior to calling into the runtime to report the method_exit the possible
    // return value is pushed to the native stack. If the result is a jfloat/jdouble
    // then ST0 is saved. See the note in generate_native_result
    tos_addr = (intptr_t*)sp();
    if (type == T_FLOAT || type == T_DOUBLE) {
      tos_addr += 2;
    }
  } else {
    tos_addr = (intptr_t*)interpreter_frame_tos_address();
  }

  switch (type) {
    case T_OBJECT  :
    case T_ARRAY   : {
      oop obj;
      if (method->is_native()) {
#ifdef CC_INTERP
        obj = istate->_oop_temp;
#else
        obj = cast_to_oop(at(interpreter_frame_oop_temp_offset));
#endif // CC_INTERP
      } else {
        oop* obj_p = (oop*)tos_addr;
        obj = (obj_p == NULL) ? (oop)NULL : *obj_p;
      }
      assert(obj == NULL || Universe::heap()->is_in(obj), "sanity check");
      *oop_result = obj;
      break;
    }
    case T_BOOLEAN : value_result->z = *(jboolean*)tos_addr; break;
    case T_BYTE    : value_result->b = *(jbyte*)tos_addr; break;
    case T_CHAR    : value_result->c = *(jchar*)tos_addr; break;
    case T_SHORT   : value_result->s = *(jshort*)tos_addr; break;
    case T_INT     : value_result->i = *(jint*)tos_addr; break;
    case T_LONG    : value_result->j = *(jlong*)tos_addr; break;
    case T_FLOAT   : value_result->f = *(jfloat*)tos_addr; break;
    case T_DOUBLE  : value_result->d = *(jdouble*)tos_addr; break;
    case T_VOID    : /* Nothing to do */ break;
    default        : ShouldNotReachHere();
  }

  return type;
}


intptr_t* frame::interpreter_frame_tos_at(jint offset) const {
  int index = (Interpreter::expr_offset_in_bytes(offset)/wordSize);
  return &interpreter_frame_tos_address()[index];
}

#ifndef PRODUCT

#define DESCRIBE_FP_OFFSET(name) \
  values.describe(frame_no, fp() + frame::name##_offset, #name)

void frame::describe_pd(FrameValues& values, int frame_no) {
  if (is_interpreted_frame()) {
    DESCRIBE_FP_OFFSET(interpreter_frame_sender_sp);
    DESCRIBE_FP_OFFSET(interpreter_frame_last_sp);
    DESCRIBE_FP_OFFSET(interpreter_frame_method);
    DESCRIBE_FP_OFFSET(interpreter_frame_mdx);
    DESCRIBE_FP_OFFSET(interpreter_frame_cache);
    DESCRIBE_FP_OFFSET(interpreter_frame_locals);
    DESCRIBE_FP_OFFSET(interpreter_frame_bcx);
    DESCRIBE_FP_OFFSET(interpreter_frame_initial_sp);
  }
}
#endif

intptr_t *frame::initial_deoptimization_info() {
  // used to reset the saved FP
  return fp();
}

intptr_t* frame::real_fp() const {
  if (_cb != NULL) {
    // use the frame size if valid
    int size = _cb->frame_size();
    if (size > 0) {
      return unextended_sp() + size;
    }
  }
  // else rely on fp()
  assert(! is_compiled_frame(), "unknown compiled frame size");
  return fp();
}

#ifndef PRODUCT
// This is a generic constructor which is only used by pns() in debug.cpp.
frame::frame(void* sp, void* fp, void* pc) {
  init((intptr_t*)sp, (intptr_t*)fp, (address)pc);
}
#endif
