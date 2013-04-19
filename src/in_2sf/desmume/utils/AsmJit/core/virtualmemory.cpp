// [AsmJit]
// Complete JIT Assembler for C++ Language.
//
// [License]
// Zlib - See COPYING file in this package.

#define ASMJIT_EXPORTS

// [Dependencies - AsmJit]
#include "../core/intutil.h"
#include "../core/virtualmemory.h"

// [Dependencies - Windows]
#ifdef ASMJIT_WINDOWS
# include "windowsh_wrapper.h"
#endif // ASMJIT_WINDOWS

// [Dependencies - Posix]
#ifdef ASMJIT_POSIX
# include <sys/types.h>
# include <sys/mman.h>
# include <unistd.h>
#endif // ASMJIT_POSIX

// [Api-Begin]
#include "../core/apibegin.h"

namespace AsmJit
{

// ============================================================================
// [AsmJit::VirtualMemory - Windows]
// ============================================================================

#ifdef ASMJIT_WINDOWS
struct VirtualMemoryLocal
{
	VirtualMemoryLocal()
	{
		SYSTEM_INFO info;
		GetSystemInfo(&info);

		this->alignment = info.dwAllocationGranularity;
		this->pageSize = IntUtil::roundUpToPowerOf2<uint32_t>(info.dwPageSize);
	}

	size_t alignment;
	size_t pageSize;
};

static VirtualMemoryLocal &vm()
{
	static VirtualMemoryLocal vm;
	return vm;
};

void *VirtualMemory::alloc(size_t length, size_t *allocated, bool canExecute)
{
	return allocProcessMemory(GetCurrentProcess(), length, allocated, canExecute);
}

void VirtualMemory::free(void *addr, size_t length)
{
	return freeProcessMemory(GetCurrentProcess(), addr, length);
}

void *VirtualMemory::allocProcessMemory(HANDLE hProcess, size_t length, size_t *allocated, bool canExecute)
{
	// VirtualAlloc rounds allocated size to page size automatically.
	size_t msize = IntUtil::roundUp(length, vm().pageSize);

	// Windows XP SP2 / Vista allow Data Excution Prevention (DEP).
	WORD protect = canExecute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
	LPVOID mbase = VirtualAllocEx(hProcess, nullptr, msize, MEM_COMMIT | MEM_RESERVE, protect);
	if (!mbase)
		return nullptr;

	ASMJIT_ASSERT(IntUtil::isAligned<size_t>(reinterpret_cast<size_t>(mbase), vm().alignment));

	if (allocated)
		*allocated = msize;
	return mbase;
}

void VirtualMemory::freeProcessMemory(HANDLE hProcess, void *addr, size_t /* length */)
{
	VirtualFreeEx(hProcess, addr, 0, MEM_RELEASE);
}

size_t VirtualMemory::getAlignment()
{
	return vm().alignment;
}

size_t VirtualMemory::getPageSize()
{
	return vm().pageSize;
}
#endif // ASMJIT_WINDOWS

// ============================================================================
// [AsmJit::VirtualMemory - Posix]
// ============================================================================

#ifdef ASMJIT_POSIX

// MacOS uses MAP_ANON instead of MAP_ANONYMOUS.
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif // MAP_ANONYMOUS

struct VirtualMemoryLocal
{
	VirtualMemoryLocal()
	{
		this->alignment = this->pageSize = ::getpagesize();
	}

	size_t alignment;
	size_t pageSize;
};

static VirtualMemoryLocal &vm()
{
	static VirtualMemoryLocal vm;
	return vm;
}

void *VirtualMemory::alloc(size_t length, size_t *allocated, bool canExecute)
{
	size_t msize = IntUtil::roundUp<size_t>(length, vm().pageSize);
	int protection = PROT_READ | PROT_WRITE | (canExecute ? PROT_EXEC : 0);

	void *mbase = ::mmap(nullptr, msize, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mbase == MAP_FAILED)
		return nullptr;

	if (allocated)
		*allocated = msize;
	return mbase;
}

void VirtualMemory::free(void *addr, size_t length)
{
	munmap(addr, length);
}

size_t VirtualMemory::getAlignment()
{
	return vm().alignment;
}

size_t VirtualMemory::getPageSize()
{
	return vm().pageSize;
}
#endif // ASMJIT_POSIX

} // AsmJit namespace

// [Api-End]
#include "../core/apiend.h"