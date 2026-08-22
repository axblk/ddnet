#include <base/hash.h>
#include <base/rust.h>

#include <engine/shared/quic.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
	void WriteFile(const char *pPath, rust::Vec<std::uint8_t> vData)
	{
		std::ofstream File(pPath, std::ios::binary | std::ios::trunc);
		if(!File.write(reinterpret_cast<const char *>(vData.data()), static_cast<std::streamsize>(vData.size())))
			throw std::runtime_error(std::string("cannot write ") + pPath);
	}

	int GenerateIdentity(int Argc, const char **ppArgv)
	{
		if(Argc != 5)
			return 2;
		auto Identity = ModernQuic::quic_generate_identity(ppArgv[2]);
		const SHA256_DIGEST Fingerprint = sha256(Identity.certificate_der.data(), Identity.certificate_der.size());
		char aFingerprint[SHA256_MAXSTRSIZE];
		sha256_str(Fingerprint, aFingerprint, sizeof(aFingerprint));
		WriteFile(ppArgv[3], std::move(Identity.certificate_der));
		WriteFile(ppArgv[4], std::move(Identity.private_key_der));
		std::cout << "certificate sha256:" << aFingerprint << '\n';
		return 0;
	}
}

int main(int Argc, const char **ppArgv)
{
	// A panic in the Rust half should fail the same way an assertion does.
	rust_panic_use_dbg_assert();
	try
	{
		if(Argc >= 2 && std::strcmp(ppArgv[1], "generate") == 0)
			return GenerateIdentity(Argc, ppArgv);
	}
	catch(const std::exception &Error)
	{
		std::cerr << Error.what() << '\n';
		return 1;
	}
	std::cerr << "usage: quic_cli generate <name> <cert.der> <key.der>\n";
	return 2;
}
