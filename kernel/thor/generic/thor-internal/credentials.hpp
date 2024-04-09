#pragma once

#include <smarter.hpp>
#include <frg/string.hpp>

namespace thor {

struct Credentials {
	Credentials();

	const char *credentials() {
		return _credentials;
	}
protected:
	char _credentials[16];
};

smarter::shared_ptr<Credentials> getTokenCredentialsByType(frg::string_view type);

} // namespace thor
