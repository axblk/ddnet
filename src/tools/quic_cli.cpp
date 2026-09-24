#include <base/hash.h>
#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/rust.h>
#include <base/str.h>

#include <engine/shared/quic.h>

#include <exception>

static bool WriteFile(const char *pPath, const rust::Vec<uint8_t> &vData)
{
	IOHANDLE File = io_open(pPath, IOFLAG_WRITE);
	if(!File)
	{
		log_error("quic_cli", "failed to open '%s' for writing", pPath);
		return false;
	}
	const bool Written = io_write(File, vData.data(), vData.size()) == vData.size();
	io_close(File);
	if(!Written)
		log_error("quic_cli", "failed to write '%s'", pPath);
	return Written;
}

int main(int argc, const char **argv)
{
	// A panic in the Rust half should fail the same way an assertion does.
	rust_panic_use_dbg_assert();
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();
	if(argc != 5 || str_comp(argv[1], "generate") != 0)
	{
		log_error("quic_cli", "usage: quic_cli generate <name> <cert.der> <key.der>");
		return -1;
	}

	ModernQuic::QuicIdentity Identity;
	try
	{
		Identity = ModernQuic::quic_generate_identity(argv[2]);
	}
	catch(const std::exception &Error)
	{
		log_error("quic_cli", "failed to generate a certificate: %s", Error.what());
		return -1;
	}
	if(!WriteFile(argv[3], Identity.certificate_der) || !WriteFile(argv[4], Identity.private_key_der))
		return -1;

	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(sha256(Identity.certificate_der.data(), Identity.certificate_der.size()), aSha256, sizeof(aSha256));
	log_info("quic_cli", "certificate sha256:%s", aSha256);
	return 0;
}
