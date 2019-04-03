


#include "Test.h"


/*

 So here I want to try a concept I have that will make it possible to jump
 between JIT emulated ARM code and native code in Einstein. I have shown that
 this works before, but this will hopefully a much cleaner approach.

 JIT interpretation runs in a loop across arrays of ioUnits. The PC register
 determines which ioUnit to execute next. The JIT emulator has no concept
 of stacks and heaps, subroutines or interrupt. It just executes ioUnits one
 by one.

 Native code has a tremendous speed factor over JIT in Einstein. Not only do we
 save the processor the work of interpreting commands and the executing units
 with a facto of 10 to 50 over the original command, we can also take shortcut
 when accessing hardware registers, the coprocessor, or doing FP math.

 To make native code fast, it should also execute in the native order of things
 by storing temporary variables and by calling other subroutines using the
 native stack. The JIT Interpreter can call native code easily at any time.
 This is great if we can start implementing native code at the leaves of the
 call tree (leaves would be functions that do not call other functions), working
 our way up to the brancehs and ultimately the root.

 But real life is different. First of all, some leaves are rarely if ever
 called, but would be implemented first by this logic, creating a lot of work
 and very little benefit. Also, code is recursive. When implementing a major
 function, like the thread dispatch, we will eventually find out that after
 a slew of function calls, it may call itself again later, locking the native
 code development in the same loop. Lastly, code is not linear. Interrupts
 can occur at any time, and many function calls in NewtonOS can call the
 scheduler, which may switch from one task to another.

 The solution is to use a rather uncommon API that emulates the Newton hardware
 well: Fibers. Fibers are a kind of thread, but no two fibers can run at the
 same time. Netwons have a single core CPU and preemptive multitasking, which
 is very much like fibres. The only exception are the two hardware interrupts
 which work more like threads, but can be emulated with fibers with a trick.

 Everything that can be interrupted needs to run in a fiber, and everything
 that interrupts other code also needs to run in fibers. So we will evetually
 create an initial fiber, one fiber for every asynchronous CPU mode, and one
 fiber for every system task that the OS creates. For simplicity, I will put the
 SWI in its own fiber for now. So here are a list of fibers:

 Fiber 0: JIT Emulator  ---------------------------------
 Fiber 1: IRQ Interrupt ---------------------------------
 Fiber 2: FIQ Interrupt ---------------------------------
 Fiber 3: SWI Interrupt ---------------------------------
 Fiber n: TTask n       ---------------------------------

 I mentioned that only one fiber can run at a time. So lets say we run a task
 int native mode which then calls an SWI that is available native, execpt for a
 subroutine that has not been translated yet. The SWI returns to the native
 caller, which then also calls a JIT function. This would be the call graph:

 Fiber 0: JIT Emulator  -------#===#-------#===#---------
 Fiber 1: IRQ Interrupt -------|---|-------|---|---------
 Fiber 2: FIQ Interrupt -------|---|-------|---|---------
 Fiber 3: SWI Interrupt ---#===#---#===#---|---|---------
 Fiber n: TTask n      >===#-----------#===#---#=====-----

 We see that the JIT section is completely agnostic to the type of code it runs
 wheras the fibers are strictly allocated to a task.

 So what we need is an implementation of fibers (see K/Threads/TFiber), a way
 to call native subroutines from JIT, using the correct fiber, and a way to call
 JIT subroutines from native, and eventually return to the correct fiber. To
 not make this any more complicated as needed, I limit those mode changes to
 function calls (ARM 'bl' commenads).

 We need one stub that takes "C++" style parameters, converts them into ARM
 notation, sets a return address, and jumps to teh JIT fiber. We need a second
 kind of stub that chooses the right fiber and converts ARM arguments to "C++"
 calls. We also need to modify the Scheduler to switch from fiber to fiber or
 fiber to JIT or JIT to fiber.


 Memory Emulation
 ----------------

 Einstein is running in emulated memory. Let's say we have a class:

 class TQueue {
 public:
     TQueue *pPrev;
     TQueue *pNext;
 };

 The correct way to read the value of a
 member variable of a class would be:

 TQueue *q = x;
 KUInt32 tQueuePtr = (KUInt32)(uintptr_t)q;
 KUInt32 tNext = tMemoryManager->Read(tQueuePtr+4);
 TQueue *next = (TQueue*)(uintptr_t)tNext;

 But we would much rather write this:

 TQueue *q = x;
 TQueue *next = q->GetNext();

 This is moch more readable and has the huge advantage that this would compile
 and run even outside of Einstein, assuming that the Get method is implemented
 accordingly. I have decided to create macros that build getters and setters
 for both cases, inside Einstein or as standalones:

 class TQueue {
 #if EINSTEIN
 GetNext() { return (TQueue*)tMemoryManager->Read(((KUInt32)this)+4); }
 #else
 TQueue *pNext;
 GetNext() { return pNext; }
 #endif
 }

 The additional conversions should be optimized away by the compiler, and not
 being able to access pNext should make up for the improved readability. This
 even works in 64-bit mode, emulating a 32-bit machine.

 */

// --- proteus variables

TARMProcessor *CPU = nullptr;
TMemory *MEM = nullptr;
TInterruptManager *INT = nullptr;

typedef void (*FPtr)();

enum {
	kFiberAbort = 1,
	kFiberIdle,
	kFiberCallNative,
	kFiberCallJIT,
	kFiberReturn,
	kFiberUnknown
};

class TProteusFiber : public TFiber
{
public:
	TProteusFiber() { }
	KSInt32 Task(KSInt32 inReason=0, void* inUserData=0L)
	{
		SwitchToJIT();
		return 0;
	}
	void SwitchToJIT() {
		for (;;) {
			FPtr fn;
			int reason = Suspend(kFiberCallJIT);
			switch (reason) {
				case kFiberReturn:
					return;
				case kFiberCallNative:
					fn = (FPtr)GetUserData();
					fn();
					break;
				default:
					fprintf(stderr, "ERROR: TSVCFiber::SwitchToJIT, unexpected Resume(%d)\n", reason);
			}
		}
	}
};

TProteusFiber *svcFiber = nullptr;

/*
 * This injection initializes the Proteus system by setting up a few
 * variables for fast acces in later calls.
 */
T_ROM_INJECTION(0x00000000, "Initialize Proteus") {
	CPU = ioCPU;
	MEM = ioCPU->GetMemory();
	INT = MEM->GetInterruptManager();
	if (!svcFiber) {
		svcFiber = new TProteusFiber();
		svcFiber->Run(kFiberCallJIT);
	}
	return ioUnit;
}

TProteusFiber *findCurrentFiber()
{
	if (CPU->GetMode()==CPU->kSupervisorMode)
		return svcFiber;
	return nullptr;
}

/*
 Let's implement this part of the SWI:
 stmdb   sp!, {r0}                       @ 0x003AD80C 0xE92D0001 - .-..
 bl      VEC_StartScheduler              @ 0x003AD810 0xEB5D66F1 - .]f.
 ldmia   sp!, {r0}                       @ 0x003AD814 0xE8BD0001 - ....
*/

void swi_native_test()
{
	PUSH(SP, R0);

	LR = 0x007FFFF0;
	PC = 0x001CC4A8+4;

	TProteusFiber *fiber = findCurrentFiber();
	fiber->SwitchToJIT();

	POP(SP, R0);
	PC = 0x003AD818+4;
}

T_ROM_INJECTION(0x007FFFF0, "ReturnToFiber") {
	findCurrentFiber()->Resume(kFiberReturn, nullptr);
	return nullptr;
}

T_ROM_INJECTION(0x003AD80C, "SWI Test") {
	// Make sure that the current task has an associated fiber
	TFiber *fiber = findCurrentFiber();
	// if no fiber is found, fall back to the JIT emulation
	if (!fiber) return ioUnit;
	// if we have a fiber, tell it to run the native code
	fiber->Resume(kFiberCallNative, (void*)swi_native_test);
	return nullptr;
}

/*
mov     r4, r0                          @ 0x000E589C 0xE1A04000 - ..@.
*/
void swi_native_test_2()
{
	R4 = R0;
	PC = 0x000E58A0 + 4;
}

T_ROM_INJECTION(0x000E589C, "SWI Test 2") {
	// Make sure that the current task has an associated fiber
	TFiber *fiber = findCurrentFiber();
	// if no fiber is found, fall back to the JIT emulation
	if (!fiber) return ioUnit;
	// if we have a fiber, tell it to run the native code
	fiber->Resume(kFiberCallNative, (void*)swi_native_test_2);
	return nullptr;
}









