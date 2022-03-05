//#define EIR_DEBUG
#include <stdint.h>
#include <assert.h>
#include <eir-internal/debug.hpp>
#include <eir/interface.hpp>
#include <render-text.hpp>
#include <dtb.hpp>
#include "../cpio.hpp"
#include <frg/manual_box.hpp>
#include <frg/tuple.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

#include <arch/bit.hpp>
#include <arch/variable.hpp>

namespace eir {

namespace pmu {
	inline constexpr arch::mem_space space{0x10480000};

	inline constexpr arch::bit_register<uint32_t> resetStat{0x0404};
	inline constexpr arch::bit_register<uint32_t> wdtDisable{0x0408};
	inline constexpr arch::bit_register<uint32_t> maskReset{0x040c};

	inline constexpr arch::field<uint32_t, bool> maskBit{23, 1};

	void disableWdt() {
		space.store(wdtDisable, space.load(wdtDisable) / maskBit(false));
		space.store(maskReset, space.load(maskReset) / maskBit(false));
	}

} // namespace pmu


namespace wdt {
	inline constexpr arch::mem_space space{0x10170000};

	inline constexpr arch::bit_register<uint32_t> config{0x00};

	inline constexpr arch::field<uint32_t, bool> resetEn{0, 1};
	inline constexpr arch::field<uint32_t, bool> irqEn{1, 1};
	inline constexpr arch::field<uint32_t, bool> wdtEn{5, 1};

	void disable() {
		pmu::disableWdt();

		space.store(config, space.load(config)
				/ resetEn(false)
				/ irqEn(false)
				/ wdtEn(false));
	}
} // namespace wdt


namespace decon {
	constexpr arch::mem_space space{0x14830000};

	void setTrigger(bool en) {
		constexpr arch::bit_register<uint32_t> trigcon{0x6b0};
		constexpr arch::field<uint32_t, bool> hwDispIf0{4, 1};

		space.store(trigcon, space.load(trigcon) / hwDispIf0(en));
	}

	void updateStandalone() {
		constexpr arch::bit_register<uint32_t> update{0x710};
		constexpr arch::field<uint32_t, bool> standalone{0, 1};

		space.store(update, space.load(update) / standalone(true));
	}

	void activateWindow(uint32_t win) {
		arch::bit_register<uint32_t> wincon{0x0050 + (win * 4)};
		constexpr arch::field<uint32_t, bool> enable{0, 1};

		space.store(wincon, space.load(wincon) / enable(true));

		updateStandalone();
	}

	void directOnOff(bool en) {
		constexpr arch::bit_register<uint32_t> vidcon0{0x90};
		constexpr arch::field<uint32_t, bool> enableFlag{0, 1};
		constexpr arch::field<uint32_t, bool> enable{1, 1};

		space.store(vidcon0, space.load(vidcon0) / enable(en) / enableFlag(en));
	}


	void activateWindowDma() {
		directOnOff(true);
		updateStandalone();
	}

	void init() {
		activateWindow(0);
		activateWindowDma();
		setTrigger(true);
	}
} // namespace decon

void debugPrintChar(char) {
}

extern "C" void eirExynosMain(uintptr_t deviceTreePtr) {
	wdt::disable();

	decon::init();

	setFbInfo((void *)0x67000000, 1080, 1920, 1080 * 4);

	initProcessorEarly();

	DeviceTree dt{reinterpret_cast<void *>(deviceTreePtr)};

	eir::infoLogger() << "DTB pointer " << dt.data() << frg::endlog;
	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dt.size()} << frg::endlog;

	DeviceTreeNode chosenNode;
	bool hasChosenNode = false;

	DeviceTreeNode memoryNodes[32];
	size_t nMemoryNodes = 0;

	dt.rootNode().discoverSubnodes(
		[](DeviceTreeNode &node) {
			return !memcmp("memory@", node.name(), 7)
				|| !memcmp("chosen", node.name(), 7);
		},
		[&](DeviceTreeNode node) {
			if (!memcmp("chosen", node.name(), 7)) {
				assert(!hasChosenNode);

				chosenNode = node;
				hasChosenNode = true;
			} else {
				assert(nMemoryNodes < 32);

				memoryNodes[nMemoryNodes++] = node;
			}
			infoLogger() << "Node \"" << node.name() << "\" discovered" << frg::endlog;
		});

	infoLogger() << "FOO" << frg::endlog;

	uint32_t addressCells = 2, sizeCells = 1;

	for (auto prop : dt.rootNode().properties()) {
		if (!memcmp("#address-cells", prop.name(), 15)) {
			addressCells = prop.asU32();
		} else if (!memcmp("#size-cells", prop.name(), 12)) {
			sizeCells = prop.asU32();
		}
	}

	assert(nMemoryNodes && hasChosenNode);

	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;
	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "At 0x" << frg::hex_fmt{ent.address}
			<< ", ends at 0x" << frg::hex_fmt{ent.address + ent.size}
			<< " (0x" << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;

		reservedRegions[nReservedRegions++] = {ent.address, ent.size};
	}
	eir::infoLogger() << "End of memory reservation entries" << frg::endlog;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {eirStart, eirEnd - eirStart};

	uintptr_t initrd = 0;
	if (auto p = chosenNode.findProperty("linux,initrd-start"); p) {
		if (p->size() == 4)
			initrd = p->asU32();
		else if (p->size() == 8)
			initrd = p->asU64();
		else
			assert(!"Invalid linux,initrd-start size");

		eir::infoLogger() << "Initrd is at " << (void *)initrd << frg::endlog;
	} else {
		initrd = 0x8000000;
		eir::infoLogger() << "Assuming initrd is at " << (void *)initrd << frg::endlog;
	}

	CpioRange cpio_range{reinterpret_cast<void *>(initrd)};

	auto initrd_end = reinterpret_cast<uintptr_t>(cpio_range.eof());
	eir::infoLogger() << "Initrd ends at " << (void *)initrd_end << frg::endlog;

	reservedRegions[nReservedRegions++] = {initrd, initrd_end - initrd};
	reservedRegions[nReservedRegions++] = {deviceTreePtr, dt.size()};

	for (int i = 0; i < nMemoryNodes; i++) {
		auto reg = memoryNodes[i].findProperty("reg");
		assert(reg);

		size_t j = 0;
		while (j < reg->size()) {
			auto base = reg->asPropArrayEntry(addressCells, j);
			j += addressCells * 4;

			auto size = reg->asPropArrayEntry(sizeCells, j);
			j += sizeCells * 4;

			createInitialRegions({base, size}, {reservedRegions, nReservedRegions});
		}
	}

	setupRegionStructs();

	eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		eir::infoLogger() << "    Memory region [" << i << "]."
				<< " Base: 0x" << frg::hex_fmt{regions[i].address}
				<< ", length: 0x" << frg::hex_fmt{regions[i].size} << frg::endlog;
		if(regions[i].regionType == RegionType::allocatable)
			eir::infoLogger() << "        Buddy tree at 0x" << frg::hex_fmt{regions[i].buddyTree}
					<< ", overhead: 0x" << frg::hex_fmt{regions[i].buddyOverhead}
					<< frg::endlog;
	}

	frg::span<uint8_t> kernel_image{nullptr, 0};

	for (auto entry : cpio_range) {
		if (entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	assert(kernel_image.data() && kernel_image.size());

	uint64_t kernel_entry = 0;
	initProcessorPaging(kernel_image.data(), kernel_entry);

	auto info_ptr = generateInfo("");

	auto module = bootAlloc<EirModule>();
	module->physicalBase = initrd;
	module->length = initrd_end - initrd;

	char *name_ptr = bootAlloc<char>(11);
	memcpy(name_ptr, "initrd.cpio", 11);
	module->namePtr = mapBootstrapData(name_ptr);
	module->nameLength = 11;

	info_ptr->numModules = 1;
	info_ptr->moduleInfo = mapBootstrapData(module);

	info_ptr->dtbPtr = deviceTreePtr;
	info_ptr->dtbSize = dt.size();


	auto framebuf = &info_ptr->frameBuffer;
	framebuf->fbAddress = 0x67000000;
	framebuf->fbPitch = 1080 * 4;
	framebuf->fbWidth = 1080;
	framebuf->fbHeight = 1920;
	framebuf->fbBpp = 32;
	framebuf->fbType = 0;

	for(address_t pg = 0; pg < 1080 * 4 * 1920; pg += 0x1000)
		mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, 0x67000000 + pg,
				PageFlags::write, CachingMode::mmio);
	mapKasanShadow(0xFFFF'FE00'4000'0000, 1080 * 4 * 1920);
	unpoisonKasanShadow(0xFFFF'FE00'4000'0000, 1080 * 4 * 1920);
	framebuf->fbEarlyWindow = 0xFFFF'FE00'4000'0000;

	//mapSingle4kPage(0xFFFF'0000'0000'0000, mmioBase + 0x201000,
	//		PageFlags::write, CachingMode::mmio);
	mapKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
	unpoisonKasanShadow(0xFFFF'0000'0000'0000, 0x1000);

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;

	eirEnterKernel(eirTTBR[0] + 1, eirTTBR[1] + 1, kernel_entry,
			0xFFFF'FE80'0001'0000, 0xFFFF'FE80'0001'0000);

	while(true);
}

} // namespace eir
