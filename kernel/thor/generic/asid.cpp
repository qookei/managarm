#include <thor-internal/arch-generic/asid.hpp>

#include <thor-internal/cpu-data.hpp>
#include <thor-internal/arch/ints.hpp>

namespace thor {

namespace {

void shootInBinding(PageBinding &binding, ShootNode *node) {
	// If we're invalidating a lot of pages, just invalidate the
	// whole ASID instead.
	if((node->size >> kPageShift) >= 64) {
		invalidateAsid(binding.id());
	} else {
		for(size_t off = 0; off < node->size; off += kPageSize)
			invalidatePage(binding.id(),
					reinterpret_cast<void *>(node->address + off));
	}
}

} // namespace anonymous


ShootNodeList
PageBinding::completeShootdown_(PageSpace *space, uint64_t afterSequence, bool doShootdown) {
	ShootNodeList complete;

	if(!space->shootQueue_.empty()) {
		auto current = space->shootQueue_.back();
		while(current->sequence_ > afterSequence) {
			auto predecessor = current->queueNode.previous;

			if(doShootdown) {
				shootInBinding(*this, current);
			}

			// Signal completion of the shootdown.
			if(current->initiatorCpu_ != getCpuData()) {
				if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
					auto it = space->shootQueue_.iterator_to(current);
					space->shootQueue_.erase(it);
					complete.push_front(current);
				}
			}

			if(!predecessor)
				break;
			current = predecessor;
		}
	}

	// If not just doing a TLB shootdown, we're unbinding this
	// page space.
	if(!doShootdown) {
		space->numBindings_--;
		if(!space->numBindings_ && space->retireNode_) {
			space->retireNode_->complete();
			space->retireNode_ = nullptr;
		}
	}

	return complete;
}

bool PageBinding::isPrimary() {
	assert(!intsAreEnabled());
	auto &context = getCpuData()->asidData->pageContext;

	return context.primaryBinding_ == this;
}

void PageBinding::rebind() {
	assert(!intsAreEnabled());
	assert(boundSpace_);
	auto &context = getCpuData()->asidData->pageContext;

	// The global binding should always be current
	assert(id_ != globalBindingId);
	switchToPageTable(boundSpace_->rootTable(), id_, false);

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;
}

void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(!boundSpace_ || boundSpace_.get() != space.get()); // This would be unnecessary work.
	auto &context = getCpuData()->asidData->pageContext;

	auto unboundSpace = boundSpace_;
	auto unboundSequence = alreadyShotSequence_;

	// Bind the new space.
	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->mutex_);

		targetSeq = space->shootSequence_;
		space->numBindings_++;
	}

	boundSpace_ = space;
	alreadyShotSequence_ = targetSeq;

	if(id_ != globalBindingId) {
		switchToPageTable(boundSpace_->rootTable(), id_, true);
	}

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;

	// Mark every shootdown request in the unbound space as shot-down.
	ShootNodeList complete;
	if(unboundSpace) {
		auto lock = frg::guard(&boundSpace_->mutex_);

		complete = completeShootdown_(
			unboundSpace.get(),
			unboundSequence,
			false);
	}

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

void PageBinding::unbind() {
	assert(!intsAreEnabled());
	assert(id_ != globalBindingId);

	if(!boundSpace_)
		return;

	// Perform shootdown.
	if(isPrimary()) {
		// If this is the primary binding, switch away, as the
		// page tables are about to be freed after this is
		// complete.
		switchAwayFromPageTable(id_);
	} else {
		invalidateAsid(id_);
	}

	ShootNodeList complete;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);

		complete = completeShootdown_(
			boundSpace_.get(),
			alreadyShotSequence_,
			false);
	}

	boundSpace_ = nullptr;
	alreadyShotSequence_ = 0;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

void PageBinding::shootdown() {
	assert(!intsAreEnabled());

	if(!boundSpace_)
		return;

	// If we retire the space anyway, just flush the whole ASID.
	if(boundSpace_->wantToRetire_.load(std::memory_order_acquire)) {
		unbind();
		return;
	}

	ShootNodeList complete;
	uint64_t targetSeq;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);

		complete = completeShootdown_(
			boundSpace_.get(),
			alreadyShotSequence_,
			true);

		targetSeq = boundSpace_->shootSequence_;
	}

	alreadyShotSequence_ = targetSeq;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}


void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {
	auto &bindings = getCpuData()->asidData->bindings;

	size_t lruIdx = 0;
	for(size_t i = 0; i < bindings.size(); i++) {
		// If the space is currently bound, always keep that binding.
		auto bound = bindings[i].boundSpace();
		if(bound && bound.get() == space.get()) {
			if(!bindings[i].isPrimary())
				bindings[i].rebind();
			return;
		}

		// Otherwise, prefer the LRU binding.
		if(bindings[i].primaryStamp() < bindings[lruIdx].primaryStamp())
			lruIdx = i;
	}

	bindings[lruIdx].rebind(space);
}


PageSpace::PageSpace(PhysicalAddr rootTable)
: rootTable_{rootTable}, numBindings_{0}, shootSequence_{0} { }

PageSpace::~PageSpace() {
	assert(!numBindings_);
}


void PageSpace::retire(RetireNode *node) {
	bool anyBindings;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		anyBindings = numBindings_;
		if(anyBindings) {
			retireNode_ = node;
			wantToRetire_.store(true, std::memory_order_release);
		}
	}

	if(!anyBindings)
		node->complete();

	sendShootdownIpi();
}


bool PageSpace::submitShootdown(ShootNode *node) {
	assert(!(node->address & (kPageSize - 1)));
	assert(!(node->size & (kPageSize - 1)));

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		auto unshotBindings = numBindings_;

		auto &globalBinding = getCpuData()->asidData->globalBinding;
		auto &bindings = getCpuData()->asidData->bindings;

		// Perform synchronous shootdown.
		if(globalBinding.boundSpace().get() == this) {
			assert(unshotBindings);
			shootInBinding(globalBinding, node);
			unshotBindings--;
		} else {
			for(size_t i = 0; i < bindings.size(); i++) {
				if(bindings[i].boundSpace().get() != this)
					continue;

				assert(unshotBindings);
				shootInBinding(bindings[i], node);
				unshotBindings--;
			}
		}

		if(!unshotBindings)
			return true;

		node->initiatorCpu_ = getCpuData();
		node->sequence_ = ++shootSequence_;
		node->bindingsToShoot_ = unshotBindings;
		shootQueue_.push_back(node);
	}

	sendShootdownIpi();
	return false;
}


} // namespace thor
