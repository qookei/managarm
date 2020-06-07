
#include "kernel.hpp"

namespace thor {

namespace {
	constexpr bool logScheduling = false;
	constexpr bool logNextBest = false;
	constexpr bool logUpdates = false;
	constexpr bool logIdle = false;
	constexpr bool logTimeSlice = false;

	constexpr bool disablePreemption = false;

	// Minimum length of a preemption time slice in ns.
	constexpr int64_t sliceGranularity = 10'000'000;
}

int ScheduleEntity::orderPriority(const ScheduleEntity *a, const ScheduleEntity *b) {
	return b->priority - a->priority; // Prefer larger priority.
}

bool ScheduleEntity::scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b) {
	return a->baseUnfairness - a->refProgress
			> b->baseUnfairness - b->refProgress; // Prefer greater unfairness.
}

ScheduleEntity::ScheduleEntity()
: state{ScheduleState::null}, priority{0}, _refClock{0}, _runTime{0},
		refProgress{0}, baseUnfairness{0} { }

ScheduleEntity::~ScheduleEntity() {
	assert(state == ScheduleState::null);
}

void Scheduler::associate(ScheduleEntity *entity, Scheduler *scheduler) {
//	frigg::infoLogger() << "associate " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::null);
	entity->_scheduler = scheduler;
	entity->state = ScheduleState::attached;
}

void Scheduler::unassociate(ScheduleEntity *entity) {
	// TODO: This is only really need to assert against _current.
	auto irqLock = frigg::guard(&irqMutex());

	auto self = entity->_scheduler;
	assert(self);

	assert(entity->state == ScheduleState::attached);
	assert(entity != self->_current);
	entity->_scheduler = nullptr;
	entity->state = ScheduleState::null;
}

void Scheduler::setPriority(ScheduleEntity *entity, int priority) {
	auto scheduleLock = frigg::guard(&irqMutex());

	auto self = entity->_scheduler;
	assert(self);

	// Otherwise, we would have to remove-reinsert into the queue.
	assert(entity == self->_current);

	entity->priority = priority;
	self->_needPreemptionUpdate = true;
}

void Scheduler::resume(ScheduleEntity *entity) {
//	frigg::infoLogger() << "resume " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::attached);

	auto self = entity->_scheduler;
	assert(self);
	assert(entity != self->_current);
	bool wasEmpty;
	{
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&self->_mutex);

		entity->state = ScheduleState::pending;

		wasEmpty = self->_pendingList.empty();
		self->_pendingList.push_back(entity);
	}

	if(wasEmpty) {
		if(self == &getCpuData()->scheduler) {
			sendPingIpi(self->_cpuContext->localApicId);
		}else{
			sendPingIpi(self->_cpuContext->localApicId);
		}
	}
}

void Scheduler::suspendCurrent() {
	auto scheduleLock = frigg::guard(&irqMutex());

	auto self = localScheduler();
	auto entity = self->_current;
	assert(entity);
//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;

	// Update the unfairness on suspend.
	self->_updateEntityStats(entity);
	entity->state = ScheduleState::attached;

	self->_current = nullptr;
	self->_needPreemptionUpdate = true;
}

Scheduler::Scheduler(CpuData *cpu_context)
: _cpuContext{cpu_context}, _current{nullptr},
		_numWaiting{0}, _refClock{0}, _systemProgress{0} { }

Progress Scheduler::_liveUnfairness(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);

	auto delta_progress = _systemProgress - entity->refProgress;
	if(entity == _current) {
		return entity->baseUnfairness - _numWaiting * delta_progress;
	}else{
		return entity->baseUnfairness + delta_progress;
	}
}

int64_t Scheduler::_liveRuntime(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	if(entity == _current) {
		return entity->_runTime + (_refClock - entity->_refClock);
	}else{
		return entity->_runTime;
	}
}

void Scheduler::update() {
	// Returns the reciprocal in 0.8 fixed point format.
	auto fixedInverse = [] (uint32_t x) -> uint32_t {
		assert(x < (1 << 6));
		return static_cast<uint32_t>(1 << 8) / x;
	};

	// Number of waiting/running threads.
	auto n = _numWaiting;
	if(_current)
		n++;

	assert(haveTimer());
	auto now = systemClockSource()->currentNanos();
	auto deltaTime = now - _refClock;
	_refClock = now;
	if(n)
		_systemProgress += deltaTime * fixedInverse(n);

	if(_current)
		_updateCurrentEntity();

	// Finally, process all pending entities.
	frg::intrusive_list<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::default_list_hook<ScheduleEntity>,
			&ScheduleEntity::listHook
		>
	> pendingSnapshot;
	{
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		pendingSnapshot.splice(pendingSnapshot.end(), _pendingList);
	}
	if(!pendingSnapshot.empty())
		_needPreemptionUpdate = true;
	while(!pendingSnapshot.empty()) {
		auto entity = pendingSnapshot.pop_front();
		assert(entity->state == ScheduleState::pending);

		// Update the unfairness reference.
		entity->refProgress = _systemProgress;
		entity->_refClock = _refClock;
		entity->state = ScheduleState::active;

		_waitQueue.push(entity);
		_numWaiting++;
	}
}

// Note: this function only returns true if there is a *strictly better* entity
//       that we can schedule. In particular, if there are no waiters,
//       this function returns false, *even if* no entity is currently running.
bool Scheduler::wantReschedule() {
	assert(!intsAreEnabled());

	// If there are no waiters, we keep the current entity.
	// Otherwise, if the current entity is not active anymore, we always switch.
	if(_waitQueue.empty())
		return false;

	if(!_current)
		return true;
	assert(_current->state == ScheduleState::active);

	// Switch based on entity priority.
	if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po > 0) {
		return true;
	}else if(po < 0) {
		return false;
	}

	// Switch based on unfairness.
	auto diff = _liveUnfairness(_current) + sliceGranularity * 256
			- _liveUnfairness(_waitQueue.top());
	return diff < 0;
}

void Scheduler::reschedule() {
	assert(!intsAreEnabled());

	if(_current)
		_unschedule();
	_schedule();
	_needPreemptionUpdate = true;
}

void Scheduler::commitReschedule() {
	assert(!_current);
	assert(_needPreemptionUpdate);

	if(!_scheduled) {
		if(logScheduling || logIdle)
			frigg::infoLogger() << "System is idle" << frigg::endLog;
		suspendSelf();
		frigg::panicLogger() << "Return from suspendSelf()" << frigg::endLog;
	}

	_current = _scheduled;
	_scheduled = nullptr;
	_sliceClock = _refClock;
	_updatePreemption();
	_needPreemptionUpdate = false;

	_current->invoke();
	frigg::panicLogger() << "Return from ScheduleEntity::invoke()" << frigg::endLog;
	__builtin_unreachable();
}

void Scheduler::commitNoReschedule() {
	if(_needPreemptionUpdate) {
		_updatePreemption();
		_needPreemptionUpdate = false;
	}
}

void Scheduler::_unschedule() {
	assert(_current);

	// Decrease the unfairness at the end of the time slice.
	_updateEntityStats(_current);

	if(_current->state == ScheduleState::active) {
		_waitQueue.push(_current);
		_numWaiting++;
	}

	_current = nullptr;
}

void Scheduler::_schedule() {
	assert(!_current);
	assert(!_scheduled);

	if(_waitQueue.empty())
		return;

	auto entity = _waitQueue.top();
	_waitQueue.pop();
	_numWaiting--;

	// Increase the unfairness at the start of the time slice.
	assert(entity->state == ScheduleState::active);
	_updateWaitingEntity(entity);
	_updateEntityStats(entity);

	if(logScheduling) {
//		frigg::infoLogger() << "System progress: " << (_systemProgress / 256) / (1000 * 1000)
//				<< " ms" << frigg::endLog;
		frigg::infoLogger() << "Running entity with priority: " << entity->priority
				<< ", unfairness: " << (_liveUnfairness(entity) / 256) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(entity) / (1000 * 1000)
				<< " ms (" << (_numWaiting + 1) << " active threads)" << frigg::endLog;
	}
	if(logNextBest && !_waitQueue.empty())
		frigg::infoLogger() << "    Next entity has priority: " << _waitQueue.top()->priority
				<< ", unfairness: " << (_liveUnfairness(_waitQueue.top()) / 256) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(_waitQueue.top()) / (1000 * 1000)
				<< " ms" << frigg::endLog;

	_scheduled = entity;
}

// Returns true if preemption should be done immediately.
void Scheduler::_updatePreemption() {
	if(disablePreemption)
		return;

	// Disable preemption if there are no other threads.
	if(_waitQueue.empty()) {
		disarmPreemption();
		return;
	}

	// If there was no current entity, we would have rescheduled.
	assert(_current);
	assert(_current->state == ScheduleState::active);

	if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po < 0) {
		// Disable preemption if we have higher priority.
		disarmPreemption();
		return;
	}else{
		// If there was an entity with higher priority, we would have rescheduled.
		assert(!po);
	}

	auto diff = _liveUnfairness(_current) + sliceGranularity * 256
			- _liveUnfairness(_waitQueue.top());
	// If the unfairness was too small, we would have rescheduled.
	assert(diff >= 0);

	auto slice = diff / 256;
	if(logTimeSlice)
		frigg::infoLogger() << "Scheduling time slice: "
				<< slice / 1000 << " us" << frigg::endLog;
	armPreemption(slice);
	return;
}

void Scheduler::_updateCurrentEntity() {
	assert(_current);

	auto delta_progress = _systemProgress - _current->refProgress;
	if(logUpdates)
		frigg::infoLogger() << "Running thread unfairness decreases by: "
				<< ((_numWaiting * delta_progress) / 256) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frigg::endLog;
	_current->baseUnfairness -= _numWaiting * delta_progress;
	_current->refProgress = _systemProgress;
}

void Scheduler::_updateWaitingEntity(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	assert(entity != _current);

	if(logUpdates)
		frigg::infoLogger() << "Waiting thread unfairness increases by: "
				<< ((_systemProgress - entity->refProgress) / 256) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frigg::endLog;
	entity->baseUnfairness += _systemProgress - entity->refProgress;
	entity->refProgress = _systemProgress;
}

void Scheduler::_updateEntityStats(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active
			|| entity == _current);

	if(entity == _current)
		entity->_runTime += _refClock - entity->_refClock;
	entity->_refClock = _refClock;
}

Scheduler *localScheduler() {
	return &getCpuData()->scheduler;
}

frigg::UnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

} // namespace thor

