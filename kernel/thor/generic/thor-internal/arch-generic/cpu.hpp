#pragma once

#include <thor-internal/arch/cpu.hpp>

#include <concepts>

namespace thor {

// Make sure that Continuation provides all the generic members.
template <typename T>
concept ValidContinuation = requires(T cont) {
	{ cont.sp } -> std::same_as<void *&>;
};
static_assert(ValidContinuation<Continuation>);


// Make sure that FaultImageAccessor provides all the generic methods.
template <typename T>
concept ValidFaultImageAccessor = requires(T acc) {
	{ acc.ip() } -> std::same_as<Word *>;
	{ acc.sp() } -> std::same_as<Word *>;
	// TODO(qookie): This is only used by handleGeneralProtectionFault in main.cpp
	{ acc.code() } -> std::same_as<Word *>;

	{ acc.inKernelDomain() } -> std::same_as<bool>;
	{ acc.allowUserPages() } -> std::same_as<bool>;
};
static_assert(ValidFaultImageAccessor<FaultImageAccessor>);


// Make sure that IrqImageAccessor provides all the generic methods.
template <typename T>
concept ValidIrqImageAccessor = requires(T acc) {
	{ acc.inPreemptibleDomain() } -> std::same_as<bool>;
	{ acc.inThreadDomain() } -> std::same_as<bool>;
	{ acc.inManipulableDomain() } -> std::same_as<bool>;
	{ acc.inFiberDomain() } -> std::same_as<bool>;
	{ acc.inIdleDomain() } -> std::same_as<bool>;
};
static_assert(ValidIrqImageAccessor<IrqImageAccessor>);


// Make sure that SyscallImageAccessor provides all the generic methods.
template <typename T>
concept ValidSyscallImageAccessor = requires(T acc) {
	{ acc.number() } -> std::same_as<Word *>;
	{ acc.in0() } -> std::same_as<Word *>;
	{ acc.in1() } -> std::same_as<Word *>;
	{ acc.in2() } -> std::same_as<Word *>;
	{ acc.in3() } -> std::same_as<Word *>;
	{ acc.in4() } -> std::same_as<Word *>;
	{ acc.in5() } -> std::same_as<Word *>;
	{ acc.in6() } -> std::same_as<Word *>;
	{ acc.in7() } -> std::same_as<Word *>;
	{ acc.in8() } -> std::same_as<Word *>;

	{ acc.error() } -> std::same_as<Word *>;
	{ acc.out0() } -> std::same_as<Word *>;
	{ acc.out1() } -> std::same_as<Word *>;
};
static_assert(ValidSyscallImageAccessor<SyscallImageAccessor>);


// Make sure that UserContext provides all the generic methods.
template <typename T>
concept ValidUserContext = requires(T ctx, CpuData *data) {
	{ T::deactivate() } -> std::same_as<void>;
	{ ctx.migrate(data) } -> std::same_as<void>;
};
static_assert(ValidUserContext<UserContext>);


// Make sure that FiberContext provides all the generic methods.
template <typename T>
concept ValidFiberContext = requires(T ctx, UniqueKernelStack stack) {
	T{std::move(stack)};
};
static_assert(ValidFiberContext<FiberContext>);


// Make sure that Executor provides all the generic methods.
template <typename T>
concept ValidExecutor = requires(T *ex,
		FaultImageAccessor f, IrqImageAccessor i, SyscallImageAccessor s,
		AbiParameters abi, UserContext *user, FiberContext *fiber) {
	// Constructors
	T{user, abi};
	T{fiber, abi};
	// Register state accessors
	{ ex->arg0() } -> std::same_as<Word *>;
	{ ex->arg1() } -> std::same_as<Word *>;
	{ ex->result0() } -> std::same_as<Word *>;
	{ ex->result1() } -> std::same_as<Word *>;
	// Save/restore/work
	{ saveExecutor(ex, f) } -> std::same_as<void>;
	{ saveExecutor(ex, i) } -> std::same_as<void>;
	{ saveExecutor(ex, s) } -> std::same_as<void>;
	{ workOnExecutor(ex) } -> std::same_as<void>;
	{ restoreExecutor(ex) } -> std::same_as<void>;
};
static_assert(ValidExecutor<Executor>);


// Clean KASAN shadow for stack space before the continuation.
void scrubStack(FaultImageAccessor accessor, Continuation cont);
void scrubStack(IrqImageAccessor accessor, Continuation cont);
void scrubStack(SyscallImageAccessor accessor, Continuation cont);
void scrubStack(Executor *executor, Continuation cont);

// Restores the current executor from its saved image.
// This is functions does the heavy lifting during task switch.
[[noreturn]] void restoreExecutor(Executor *executor);

// Save state from the given image into the given executor.
void saveExecutor(Executor *executor, FaultImageAccessor accessor);
void saveExecutor(Executor *executor, IrqImageAccessor accessor);
void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

// Schedule the executor to run its thread's work queue before resuming.
void workOnExecutor(Executor *executor);


// TODO(qookie): These two functions should probably be renamed
// and moved out of here.
struct Thread;

// Set the current thread on this CPU.
// Note: This does not invoke restoreExecutor!
void switchExecutor(smarter::borrowed_ptr<Thread> executor);

// Get the current thread on this CPU.
smarter::borrowed_ptr<Thread> activeExecutor();


// Validate AssemblyCpuData and PlatformCpuData definitions.
static_assert(offsetof(AssemblyCpuData, selfPointer) == 0);
static_assert(std::derived_from<PlatformCpuData, AssemblyCpuData>);

// Get a pointer to this CPU's PlatformCpuData instance.
PlatformCpuData *getPlatformCpuData();

// Determine whether this address belongs to the higher half.
bool isHigherHald(uintptr_t address);


// Determine whether the fault is an UAR fault, and handle it appropriately if so.
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

// Permit kernel access to user pages.
void enableUserAccess();

// Deny kernel access to user pages.
void disableUserAccess();


// Calls the given function on the given stack.
void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument);

// Calls the given functor with the given arguments on the given stack.
template<typename F, typename... Args>
void runOnStack(F functor, StackBase stack, Args... args) {
	struct Context {
		Context(F functor, Args... args)
		: functor(std::move(functor)), args(std::move(args)...) { }

		F functor;
		frg::tuple<Args...> args;
	};

	Context original(std::move(functor), std::forward<Args>(args)...);
	doRunOnStack([] (void *context, void *previousSp) {
		Context stolen = std::move(*static_cast<Context *>(context));
		frg::apply(std::move(stolen.functor),
				frg::tuple_cat(frg::make_tuple(Continuation{previousSp}),
						std::move(stolen.args)));
	}, stack.sp, &original);
}


// Copies the current state into the executor and calls the supplied function.
extern "C" void doForkExecutor(Executor *executor, void (*functor)(void *), void *context);

// Save the current SIMD register state into the given executor.
void saveCurrentSimdState(Executor *executor);

// Copies the current state into the executor and calls the supplied functor.
template<typename F>
void forkExecutor(F functor, Executor *executor) {
	auto delegate = [] (void *p) {
		auto fp = static_cast<F *>(p);
		(*fp)();
	};

	saveCurrentSimdState(executor);
	doForkExecutor(executor, delegate, &functor);
}


// Fill buffer with entropy obtained from the CPU.
Error getEntropyFromCpu(void *buffer, size_t size);


// Arm the preemption timer to fire in nanos nanoseconds.
void armPreemption(uint64_t nanos);

// Disarm the preemption timer.
void disarmPreemption();

// Check whether the preemption timer is armed.
bool preemptionIsArmed();

// Get the raw timestamp in preemption timer ticks.
uint64_t getRawTimestampCounter();

} // namespace thor
