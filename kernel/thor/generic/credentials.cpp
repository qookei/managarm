#include <stdint.h>

#include <thor-internal/credentials.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/kernel_heap.hpp>

#include <frg/hash_map.hpp>

namespace thor {

Credentials::Credentials() {
	size_t progress = 0;

	// The chance of a collision is very low. To have a 50% probability that
	// we collide 2 UUIDs, we'd need to generate about 10^18 of them.
	// XXX(qookie): Verify that there indeed are no collisions?
	//              Although that seems like a waste of time...
	while (progress < 16) {
		progress += generateRandomBytes(
			_credentials + progress,
			16 - progress);
	}

	// Set the UUID to version 4 ...
	_credentials[6] &= 0x0f;
	_credentials[6] |= 0x40;

	// ... and variant 1.
	_credentials[8] &= 0x3f;
	_credentials[8] |= 0x80;
}

frg::manual_box<frg::hash_map<
	frg::string_view,
	smarter::shared_ptr<Credentials>,
	frg::hash<frg::string_view>,
	KernelAlloc
>> tokenCredentials;

smarter::shared_ptr<Credentials> getTokenCredentialsByType(frg::string_view type) {
	if (!tokenCredentials.valid())
		tokenCredentials.initialize(frg::hash<frg::string_view>{}, *kernelAlloc);

	auto it = tokenCredentials->find(type);
	if (it != tokenCredentials->end()) {
		return it->get<1>();
	}

	auto creds = smarter::allocate_shared<Credentials>(*kernelAlloc);
	tokenCredentials->insert(type, creds);
	return creds;
}

} // namespace thor
