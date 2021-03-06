//===- cxa_pnacl_sjlj_exception.cpp - PNaCl SJLJ-based exception handling--===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This implements setjmp()/longjmp()-based (SJLJ) C++ exception
// handling for PNaCl.  This uses the C++ exception info tables
// generated by the PNaClSjLjEH LLVM pass.
//
// Each __pnacl_eh_sjlj_Unwind_*() function below provides the
// definition of _Unwind_*().
//
// The "__pnacl_eh_sjlj" prefix is added so that PNaCl's SJLJ
// (setjmp()/longjmp()-based) implementation of C++ exception handling
// can coexist with other implementations in the same build of
// libc++/libcxxabi.  When SJLJ EH is enabled, each
// __pnacl_eh_sjlj_Unwind_*() symbol will get renamed to _Unwind_*()
// when linking a PNaCl pexe.
//
//===----------------------------------------------------------------------===//

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <typeinfo>

#include "cxa_exception.hpp"
#include "cxa_handlers.hpp"
#include "private_typeinfo.h"

using namespace __cxxabiv1;


// Exception info written by ExceptionInfoWriter.cpp.

struct action_table_entry {
    int32_t clause_id;
    uint32_t next_clause_list_id;
};

extern const struct action_table_entry __pnacl_eh_action_table[];
extern const __shim_type_info *const __pnacl_eh_type_table[];
extern const int32_t __pnacl_eh_filter_table[];

// Data structures used by PNaClSjLjEH.cpp.

struct landing_pad_result {
    void *exception_obj;
    uint32_t matched_clause_id;
};

struct exception_frame {
    union {
        jmp_buf jmpbuf;
        struct landing_pad_result result;
    };
    struct exception_frame *next;
    uint32_t clause_list_id;
};

__thread struct exception_frame *__pnacl_eh_stack;


// Returns whether the thrown exception (specified by throw_type and
// obj) matches none of the exception types in a C++ exception
// specification (specified by filter_id).
static bool
exception_spec_can_catch(const __shim_type_info *throw_type, void *obj,
                         int32_t filter_id)
{
    const int32_t *filter_ptr =
        &__pnacl_eh_filter_table[-filter_id - 1];
    for (; *filter_ptr != 0; ++filter_ptr)
    {
        const __shim_type_info *catch_type =
            __pnacl_eh_type_table[*filter_ptr - 1];
        // We ignore the modified value of obj here.
        if (catch_type->can_catch(throw_type, obj))
            return false;
    }
    // No type matched, so we have an exception specification error.
    return true;
}

// Returns whether the thrown exception (specified by throw_type and
// obj) matches the given landingpad clause (clause_id).
//
// If the exception matches and the clause is a "catch" clause, this
// adjusts *obj to upcast it to the type specified in the "catch"
// clause.  (For example, if throw_type uses multiple inheritance and
// derives from multiple base classes, this might involve adding a
// constant offset to *obj.)
static bool
does_clause_match(const __shim_type_info *throw_type, void **obj,
                  int32_t clause_id)
{
    // Handle "cleanup" clause.
    if (clause_id == 0)
        return true;

    // Handle "filter" clause.
    if (clause_id < 0)
        return exception_spec_can_catch(throw_type, obj, clause_id);

    // Handle "catch" clause.
    const __shim_type_info *catch_type = __pnacl_eh_type_table[clause_id - 1];
    if (catch_type == NULL)
        return true;
    return catch_type->can_catch(throw_type, *obj);
}

// Returns whether the given frame should be entered in order to
// handle the thrown exception (specified by throw_type and *obj).  If
// so, this adjusts *obj (see does_clause_match()) and sets
// *result_clause_id.
static bool
does_frame_match(const __shim_type_info *throw_type, void **obj,
                 struct exception_frame *frame, int32_t *result_clause_id)
{
    for (int32_t clause_list_id = frame->clause_list_id; clause_list_id != 0; )
    {
        const struct action_table_entry *list_node =
            &__pnacl_eh_action_table[clause_list_id - 1];
        if (does_clause_match(throw_type, obj, list_node->clause_id))
        {
            *result_clause_id = list_node->clause_id;
            return true;
        }
        clause_list_id = list_node->next_clause_list_id;
    }
    return false;
}

// Search for a stack frame that will handle the given exception,
// starting from frame.  The exception is specified by throw_type and
// *obj.
//
// If a frame is found that will handle the exception, this adjusts
// *obj (to upcast it to the "catch" type, if there is one), sets
// *result_frame and *result_clause_id to the frame and clause ID that
// matched the exception, and returns true.
static bool
find_match(const __shim_type_info *throw_type, void **obj,
           struct exception_frame *frame,
           struct exception_frame **result_frame, int32_t *result_clause_id)
{
    for (; frame != NULL; frame = frame->next)
    {
        if (does_frame_match(throw_type, obj, frame, result_clause_id))
        {
            *result_frame = frame;
            return true;
        }
    }
    return false;
}

// Search for a non-cleanup stack frame that will handle the given
// exception, starting from frame.  Returns whether a matching frame
// was found.
static bool
is_exception_caught(const __shim_type_info *throw_type, void *obj,
                    struct exception_frame *frame)
{
    for (; frame != NULL; frame = frame->next)
    {
        int32_t clause_id;
        if (does_frame_match(throw_type, &obj, frame, &clause_id)
            && clause_id != 0)
            return true;
    }
    return false;
}

static __cxa_exception *
get_exception_header_from_ue(struct _Unwind_Exception *ue_header)
{
    return (__cxa_exception *) (ue_header + 1) - 1;
}

static __cxa_dependent_exception *
get_dependent_exception_from_ue(struct _Unwind_Exception *ue_header)
{
    return (__cxa_dependent_exception *) (ue_header + 1) - 1;
}

static void *
get_object_from_ue(struct _Unwind_Exception *ue_header)
{
    if (ue_header->exception_class == kOurDependentExceptionClass)
    {
        return get_dependent_exception_from_ue(ue_header)->primaryException;
    }
    return ue_header + 1;
}

// handle_exception() is called by _Unwind_RaiseException().  It
// unwinds the stack, looking for the first C++ destructor or catch()
// block to pass control to.  In LLVM terms, it searches for the first
// matching invoke/landingpad instruction.  When it finds a match,
// this implementation passes control to the landingpad block by
// longjmp()'ing to it.
//
// In a traditional implementation (based on the Itanium ABI),
// _Unwind_RaiseException() is implemented in a library (libgcc_eh)
// that is separate from libcxxabi.  It calls back to libcxxabi's
// personality function (__gxx_personality_v0()) to determine whether
// a call on the stack has a handler for the exception.
// __gxx_personality_v0() knows that ue_header was allocated by
// libcxxabi's __cxa_allocate_exception() and can downcast it to
// libcxxabi's __cxa_exception type.
//
// In contrast, in the implementation below, the functionality of the
// personality function is folded into _Unwind_RaiseException(), so
// this implements C++-specific matching of exceptions and downcasts
// ue_header to __cxa_exception immediately.
//
// This function returns if stack unwinding did not find any stack
// frames that match the exception being thrown.
static void
handle_exception(struct _Unwind_Exception *ue_header, bool check_for_catch)
{
    __cxa_exception *xh = get_exception_header_from_ue(ue_header);

    void *obj = get_object_from_ue(ue_header);
    struct exception_frame *frame;
    int32_t clause_id;
    if (!find_match((__shim_type_info *) xh->exceptionType, &obj,
                    __pnacl_eh_stack, &frame, &clause_id))
        return;

    // Check that there is a non-cleanup handler for the exception.
    // If not, we should abort before running cleanup handlers
    // (i.e. destructors).
    //
    // This is mainly a convenience for debugging.  It means that if
    // the program throws an uncaught exception, the location of the
    // "throw" will be on the stack when the program aborts.  If we
    // ran cleanup handlers before aborting, this context would be
    // lost.
    //
    // This is optional in the C++ standard, which says "If no
    // matching handler is found, the function std::terminate() is
    // called; whether or not the stack is unwound before this call to
    // std::terminate() is implementation-defined".
    if (check_for_catch && clause_id == 0 &&
        !is_exception_caught((__shim_type_info *) xh->exceptionType, obj,
                             frame->next))
        return;

    __pnacl_eh_stack = frame->next;

    // Save adjusted exception pointer so that it can be returned by
    // __cxa_begin_catch() when entering a catch() block.
    xh->adjustedPtr = obj;

    // Save the clause ID so that if the landingpad block calls
    // __cxa_call_unexpected() and the std::set_unexpected() handler
    // throws an exception, we can re-check that exception against the
    // exception specification.
    xh->handlerSwitchValue = clause_id;

    // exception_frame uses the same location for storing the jmp_buf
    // and the landing_pad_result, so we must make a copy of the
    // jmp_buf first.
    jmp_buf jmpbuf_copy;
    memcpy(&jmpbuf_copy, &frame->jmpbuf, sizeof(jmpbuf_copy));

    // Return to the landingpad block, passing it two values.
    frame->result.exception_obj = ue_header;
    frame->result.matched_clause_id = clause_id;
    longjmp(jmpbuf_copy, 1);
}


// This implements _Unwind_RaiseException().  This is called when
// raising an exception for the first time, i.e. for the statement
// "throw EXPR;".  The compiler lowers "throw EXPR;" to:
//  * a call to __cxa_allocate_exception() to allocate memory;
//  * a call to __cxa_throw() which throws the exception by calling
//    _Unwind_RaiseException().
extern "C" _Unwind_Reason_Code
__pnacl_eh_sjlj_Unwind_RaiseException(struct _Unwind_Exception *ue_header)
{
    handle_exception(ue_header, true);
    return _URC_END_OF_STACK;
}

// This is the equivalent of _Unwind_Resume() from libgcc_eh, but we
// use a different name for PNaCl SJLJ to avoid accidental collisions
// with libgcc_eh.
//
// This is called by a landingpad block as a final step after it has
// run C++ destructors.  This is only called by a landingpad if it did
// not enter a catch() block.
//
// This function never returns.
extern "C" void
__pnacl_eh_resume(struct _Unwind_Exception *ue_header)
{
    // Pass check_for_catch=false so that unwinding does not take O(n^2)
    // time in the number of cleanup landingpads entered before entering
    // the catch() block.
    handle_exception(ue_header, false);

    // We've run C++ destructors (cleanup handlers), but no further
    // handlers were found, so abort.  We should not reach here, because
    // __pnacl_eh_sjlj_Unwind_RaiseException() already checked that
    // there was a handler for this exception other than cleanup
    // handlers.
    __cxa_begin_catch(ue_header);
    std::terminate();
}

// _Unwind_Resume_or_Rethrow() is called when rethrowing an
// exception, i.e. for the statement "throw;" (with no arguments).
// The compiler lowers "throw;" to a call to __cxa_rethrow(), which
// calls this function.
extern "C" _Unwind_Reason_Code
__pnacl_eh_sjlj_Unwind_Resume_or_Rethrow(struct _Unwind_Exception *ue_header)
{
    return __pnacl_eh_sjlj_Unwind_RaiseException(ue_header);
}

// A convenience function that calls the exception_cleanup field.
// Based on the definition in libgcc_eh's unwind.inc.
//
// This is called when a catch() block that handles an exception exits
// without rethrowing the exception.  This is called by
// __cxa_end_catch().  The compiler generates a call to
// __cxa_end_catch() at the end of a catch() block.
extern "C" void
__pnacl_eh_sjlj_Unwind_DeleteException(struct _Unwind_Exception *exc)
{
    if (exc->exception_cleanup)
        (*exc->exception_cleanup)(_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}

// This function implements __cxa_call_unexpected(), which is called
// by a landingpad block when an exception is thrown that doesn't
// match a function's exception spec (i.e. a "throw(...)" attribute on
// a function).  Calls to __cxa_call_unexpected() are generated by the
// C++ front end.
//
// This calls the handler registered with std::set_unexpected().  This
// handler is allowed to throw, in which case we must re-check the
// resulting exception against the original exception specification.
//
// The reason that __cxa_call_unexpected() is called by landingpad
// code rather than by the personality function is so that the
// landingpad code can run destructors first.
//
// This is loosely based on the __cxa_call_unexpected() implementation
// in cxa_personality.cpp.
//
// This function never returns.
extern "C" void
__pnacl_eh_sjlj_cxa_call_unexpected(struct _Unwind_Exception *ue_header)
{
    // Mark the exception as being handled, so that the
    // set_unexpected() handler can rethrow it.
    __cxa_begin_catch(ue_header);

    // Ensure that the corresponding __cxa_end_catch() call happens on
    // all paths out of this function.
    struct do_end_catch
    {
        ~do_end_catch() { __cxa_end_catch(); }
    } do_end_catch_obj;

    __cxa_exception *old_exception_header =
        get_exception_header_from_ue(ue_header);
    int32_t filter_id = old_exception_header->handlerSwitchValue;
    std::unexpected_handler u_handler = old_exception_header->unexpectedHandler;
    std::terminate_handler t_handler = old_exception_header->terminateHandler;

    try
    {
        std::__unexpected(u_handler);
    }
    catch (...)
    {
        __cxa_eh_globals *globals = __cxa_get_globals_fast();
        __cxa_exception *new_exception_header = globals->caughtExceptions;

        // If the handler threw an exception that is allowed by the
        // original exception spec, allow this exception to propagate.
        if (!exception_spec_can_catch(
                (const __shim_type_info *) new_exception_header->exceptionType,
                get_object_from_ue(&new_exception_header->unwindHeader),
                filter_id))
            throw;

        // Otherwise, if the original exception spec allows
        // std::bad_exception, throw an exception of that type.
        std::bad_exception be;
        const __shim_type_info *be_type =
            (const __shim_type_info *) &typeid(be);
        if (!exception_spec_can_catch(be_type, &be, filter_id))
            throw be;
    }
    std::__terminate(t_handler);
}
