#pragma once

#include <atomic>

#include <assert.h>
#include <frg/list.hpp>
#include <smarter.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>
#include <thor-internal/physical.hpp>

#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/cursor.hpp>

namespace thor {

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);

struct KernelPageSpace : PageSpace {
	static void initialize();

	static KernelPageSpace &global();

	// TODO(qookie): This should be private, but the ctor is invoked by frigg
	explicit KernelPageSpace(PhysicalAddr ttbr1);

	KernelPageSpace(const KernelPageSpace &) = delete;

	KernelPageSpace &operator= (const KernelPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode caching_mode);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);


	template<typename R>
	struct ShootdownOperation;

	struct [[nodiscard]] ShootdownSender {
		using value_type = void;

		template<typename R>
		friend ShootdownOperation<R>
		connect(ShootdownSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		KernelPageSpace *self;
		VirtualAddr address;
		size_t size;
	};

	ShootdownSender shootdown(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct ShootdownOperation : private ShootNode {
		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		virtual ~ShootdownOperation() = default;

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		bool start_inline() {
			ShootNode::address = s_.address;
			ShootNode::size = s_.size;
			if(s_.self->submitShootdown(this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		ShootdownSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<ShootdownSender>
	operator co_await(ShootdownSender sender) {
		return {sender};
	}

private:
	frg::ticket_spinlock _mutex;
};

inline constexpr uint64_t kPageValid = 1;
inline constexpr uint64_t kPageTable = (1 << 1);
inline constexpr uint64_t kPageL3Page = (1 << 1);
inline constexpr uint64_t kPageXN = (uint64_t(1) << 54);
inline constexpr uint64_t kPagePXN = (uint64_t(1) << 53);
inline constexpr uint64_t kPageShouldBeWritable = (uint64_t(1) << 55);
inline constexpr uint64_t kPageNotGlobal = (1 << 11);
inline constexpr uint64_t kPageAccess = (1 << 10);
inline constexpr uint64_t kPageRO = (1 << 7);
inline constexpr uint64_t kPageUser = (1 << 6);
inline constexpr uint64_t kPageInnerSh = (3 << 8);
inline constexpr uint64_t kPageOuterSh = (2 << 8);
inline constexpr uint64_t kPageWb = (0 << 2);
inline constexpr uint64_t kPagenGnRnE = (2 << 2);
inline constexpr uint64_t kPagenGnRE = (3 << 2);
inline constexpr uint64_t kPageUc = (4 << 2);
inline constexpr uint64_t kPageAddress = 0xFFFFFFFFF000;

struct ClientCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & kPageValid;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & kPageAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if (!(pte & kPageValid))
			return 0;

		PageStatus ps = page_status::present;
		if ((pte & kPageShouldBeWritable) && !(pte & kPageRO)) {
			ps |= page_status::dirty;
		}

		return ps;
	}

	static PageStatus pteClean(uint64_t *ptePtr) {
		uint64_t pte = __atomic_load_n(ptePtr, __ATOMIC_RELAXED);
		auto ps = ptePageStatus(pte);

		if(ps & page_status::dirty) {
			auto newPte = pte | kPageRO;
			// If this PTE is dirty, reset the read-only
			// bit, but only if the PTE wasn't modified by
			// a different core in the meantime.
			__atomic_compare_exchange_n(ptePtr, &pte, newPte, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
		}

		return ps;
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		uint64_t pte = physical | kPageValid | kPageL3Page
			| kPageAccess | kPageRO | kPageNotGlobal
			| kPageUser;

		if (flags & page_access::write)
			pte |= kPageShouldBeWritable;
		if (!(flags & page_access::execute))
			pte |= kPageXN | kPagePXN;
		if (cachingMode == CachingMode::writeCombine)
			pte |= kPageUc | kPageOuterSh;
		else if (cachingMode == CachingMode::uncached)
			pte |= kPagenGnRnE | kPageOuterSh;
		else if (cachingMode == CachingMode::mmio)
			pte |= kPagenGnRE | kPageOuterSh;
		else if (cachingMode == CachingMode::mmioNonPosted)
			pte |= kPagenGnRnE | kPageOuterSh;
		else {
			assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
			pte |= kPageWb | kPageInnerSh;
		}

		return pte;
	}


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & kPageValid;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & kPageAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | kPageValid | kPageTable;
	}
};
static_assert(CursorPolicy<ClientCursorPolicy>);


struct ClientPageSpace : PageSpace {
	using Cursor = thor::PageCursor<ClientCursorPolicy>;

	ClientPageSpace();

	ClientPageSpace(const ClientPageSpace &) = delete;

	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access,
			uint32_t flags, CachingMode caching_mode);
	PageStatus unmapSingle4k(VirtualAddr pointer);
	PageStatus cleanSingle4k(VirtualAddr pointer);
	bool isMapped(VirtualAddr pointer);
	bool updatePageAccess(VirtualAddr pointer);

private:
	frg::ticket_spinlock _mutex;
};

} // namespace thor
