// [AsmJit]
// Complete JIT Assembler for C++ Language.
//
// [License]
// Zlib - See COPYING file in this package.

// [Guard]
#ifndef _ASMJIT_CORE_COMPILER_H
#define _ASMJIT_CORE_COMPILER_H

// [Dependencies - AsmJit]
#include "../core/assembler.h"
#include "../core/context.h"
#include "../core/func.h"
#include "../core/operand.h"
#include "../core/podvector.h"

// [Api-Begin]
#include "../core/apibegin.h"

namespace AsmJit
{

// ============================================================================
// [Forward Declarations]
// ============================================================================

struct CompilerContext;
struct CompilerFuncDecl;
struct CompilerFuncEnd;
struct CompilerInst;
struct CompilerItem;
struct CompilerTarget;

// ============================================================================
// [AsmJit::CompilerState]
// ============================================================================

//! @brief Compiler state base.
struct CompilerState
{
};

// ============================================================================
// [AsmJit::CompilerVar]
// ============================================================================

//! @brief Compiler variable base.
struct CompilerVar
{
	// --------------------------------------------------------------------------
	// [Accessors]
	// --------------------------------------------------------------------------

	//! @brief Get variable name.
	const char *getName() const { return this->_name; }
	//! @brief Get variable id.
	uint32_t getId() const { return this->_id; }

	//! @brief Get variable type.
	uint32_t getType() const { return this->_type; }
	//! @brief Get variable class.
	uint32_t getClass() const { return this->_class; }
	//! @brief Get variable priority.
	uint32_t getPriority() const { return this->_priority; }
	//! @brief Get variable size.
	uint32_t getSize() const { return this->_size; }

	//! @brief Get whether the variable is a function argument.
	bool isArgument() const { return !!(this->_isRegArgument | this->_isMemArgument); }
	//! @brief Get whether the variable is a function argument passed through register.
	bool isRegArgument() const { return !!this->_isRegArgument; }
	//! @brief Get whether the variable is a function argument passed through memory.
	bool isMemArgument() const { return !!this->_isMemArgument; }

	//! @brief Get variable content can be calculated by a simple instruction.
	bool isCalculated() const { return !!this->_isCalculated; }

	// --------------------------------------------------------------------------
	// [Members]
	// --------------------------------------------------------------------------

	//! @brief Variable name.
	const char *_name;
	//! @brief Variable id.
	uint32_t _id;

	//! @brief Variable type.
	uint8_t _type;
	//! @brief Variable class.
	uint8_t _class;
	//! @brief Variable priority.
	uint8_t _priority;

	//! @brief Whether the variable is a function argument passed through register.
	uint8_t _isRegArgument : 1;
	//! @brief Whether the variable is a function argument passed through memory.
	uint8_t _isMemArgument : 1;
	//! @brief Whether variable content can be calculated by a simple instruction.
	//!
	//! This is used mainly by MMX and SSE2 code. This flag indicates that 
	//! register allocator should never reserve memory for this variable, because
	//! the content can be generated by a single instruction (for example PXOR).
	uint8_t _isCalculated : 1;
	//! @internal.
	uint8_t _unused : 5;

	//! @brief Variable size.
	uint32_t _size;
};

// ============================================================================
// [AsmJit::Compiler]
// ============================================================================

//! @brief Compiler.
//!
//! @sa @ref Assembler.
struct Compiler
{
	// --------------------------------------------------------------------------
	// [Construction / Destruction]
	// --------------------------------------------------------------------------

	//! @brief Create a new @ref Compiler instance.
	ASMJIT_API Compiler(Context *context);
	//! @brief Destroy the @ref Compiler instance.
	ASMJIT_API virtual ~Compiler();

	// --------------------------------------------------------------------------
	// [Context]
	// --------------------------------------------------------------------------

	//! @brief Get code generator.
	Context *getContext() const { return this->_context; }

	// --------------------------------------------------------------------------
	// [Memory Management]
	// --------------------------------------------------------------------------

	//! @brief Get zone memory manager.
	ZoneMemory &getZoneMemory() { return this->_zoneMemory; }

	//! @brief Get link memory manager.
	ZoneMemory &getLinkMemory() { return this->_linkMemory; }

	// --------------------------------------------------------------------------
	// [Logging]
	// --------------------------------------------------------------------------

	//! @brief Get logger.
	Logger *getLogger() const { return this->_logger; }

	//! @brief Set logger to @a logger.
	ASMJIT_API virtual void setLogger(Logger *logger);

	// --------------------------------------------------------------------------
	// [Error Handling]
	// --------------------------------------------------------------------------

	//! @brief Get error code.
	uint32_t getError() const { return this->_error; }

	//! @brief Set error code.
	//!
	//! This method is virtual, because higher classes can use it to catch all
	//! errors.
	ASMJIT_API virtual void setError(uint32_t error);

	// --------------------------------------------------------------------------
	// [Properties]
	// --------------------------------------------------------------------------

	//! @brief Get compiler property.
	ASMJIT_API virtual uint32_t getProperty(uint32_t propertyId);
	//! @brief Set compiler property.
	ASMJIT_API virtual void setProperty(uint32_t propertyId, uint32_t value);

	// --------------------------------------------------------------------------
	// [Clear / Reset]
	// --------------------------------------------------------------------------

	//! @brief Clear everything, but not deallocate buffers.
	//!
	//! @note This method will destroy your code.
	ASMJIT_API void clear();

	//! @brief Free internal buffer, all emitters and NULL all pointers.
	//!
	//! @note This method will destroy your code.
	ASMJIT_API void reset();

	//! @brief Called by clear() and reset() to clear all data related to derived
	//! class implementation.
	ASMJIT_API virtual void _purge();

	// --------------------------------------------------------------------------
	// [Item Management]
	// --------------------------------------------------------------------------

	//! @brief Get first item.
	CompilerItem *getFirstItem() const { return this->_first; }

	//! @brief Get last item.
	CompilerItem *getLastItem() const { return this->_last; }

	//! @brief Get current item.
	//!
	//! @note If this method returns @c NULL it means that nothing has been 
	//! emitted yet.
	CompilerItem *getCurrentItem() const { return this->_current; }

	//! @brief Get current function.
	CompilerFuncDecl *getFunc() const { return this->_func; }

	//! @brief Set current item to @a item and return the previous current one.
	ASMJIT_API CompilerItem *setCurrentItem(CompilerItem *item);

	//! @brief Add item after current item to @a item and set current item to 
	//! @a item.
	ASMJIT_API void addItem(CompilerItem *item);

	//! @brief Add item after @a ref.
	ASMJIT_API void addItemAfter(CompilerItem *item, CompilerItem *ref);

	//! @brief Remove item @a item.
	ASMJIT_API void removeItem(CompilerItem *item);

	// --------------------------------------------------------------------------
	// [Comment]
	// --------------------------------------------------------------------------

	//! @brief Emit a single comment line.
	//!
	//! @note Comment is not directly sent to logger, but instead it's stored as
	//! @ref CompilerComment item emitted when @c serialize() method is called.
	ASMJIT_API void comment(const char *fmt, ...);

	// --------------------------------------------------------------------------
	// [Embed]
	// --------------------------------------------------------------------------

	//! @brief Embed data.
	ASMJIT_API void embed(const void *data, size_t len);

	// --------------------------------------------------------------------------
	// [Members]
	// --------------------------------------------------------------------------

	//! @brief ZoneMemory allocator, used to allocate compiler items.
	ZoneMemory _zoneMemory;
	//! @brief ZoneMemory allocator, used to alloc small data structures like
	//! linked lists.
	ZoneMemory _linkMemory;

	//! @brief Context.
	Context *_context;
	//! @brief Logger.
	Logger *_logger;

	//! @brief Error code.
	uint32_t _error;
	//! @brief Properties.
	uint32_t _properties;
	//! @brief Contains options for next emitted instruction, clear after each emit.
	uint32_t _emitOptions;
	//! @brief Whether compiler was finished the job (register allocator, etc...).
	uint32_t _finished;

	//! @brief First item.
	CompilerItem *_first;
	//! @brief Last item.
	CompilerItem *_last;
	//! @brief Current item.
	CompilerItem *_current;
	//! @brief Current function.
	CompilerFuncDecl *_func;

	//! @brief Targets.
	PodVector<CompilerTarget *> _targets;
	//! @brief Variables.
	PodVector<CompilerVar *> _vars;

	//! @brief Compiler context instance, only available after prepare().
	CompilerContext *_cc;

	//! @brief Variable name id (used to generate unique names per function).
	int _varNameId;
};

// ============================================================================
// [AsmJit::Compiler - Helpers]
// ============================================================================

template<typename T, typename Compiler> inline T *Compiler_newItem(Compiler *self)
{
	void *addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self);
}

template<typename T, typename Compiler, typename P1> inline T *Compiler_newItem(Compiler *self, P1 p1)
{
	void *addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self, p1);
}

template<typename T, typename Compiler, typename P1, typename P2> inline T *Compiler_newItem(Compiler *self, P1 p1, P2 p2)
{
	void *addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self, p1, p2);
}

template<typename T, typename Compiler, typename P1, typename P2, typename P3> inline T *Compiler_newItem(Compiler *self, P1 p1, P2 p2, P3 p3)
{
	void* addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self, p1, p2, p3);
}

template<typename T, typename Compiler, typename P1, typename P2, typename P3, typename P4> inline T *Compiler_newItem(Compiler *self, P1 p1, P2 p2, P3 p3, P4 p4)
{
	void *addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self, p1, p2, p3, p4);
}

template<typename T, typename Compiler, typename P1, typename P2, typename P3, typename P4, typename P5> inline T *Compiler_newItem(Compiler *self, P1 p1, P2 p2, P3 p3, P4 p4, P5 p5)
{
	void *addr = self->getZoneMemory().alloc(sizeof(T));
	return new(addr) T(self, p1, p2, p3, p4, p5);
}

} // AsmJit namespace

// [Api-End]
#include "../core/apiend.h"

// [Guard]
#endif // _ASMJIT_CORE_COMPILER_H
